//! CLI entry point for `aegis-trace` (DO-178C & S1000D Issue 5.0 BREX Compiler).

use aegis_trace::s1000d::{
    check_orphan_requirements, generate_s1000d_fault_xml, render_trace_lock,
    render_trace_matrix_md, verify_trace_lock, verify_traceability,
};
use aegis_trace::scanner::scan_directory;
use aegis_trace::srs::SrsDocument;
use aegis_trace::TraceError;
use std::env;
use std::fs;
use std::path::{Path, PathBuf};
use std::process::ExitCode;

struct CliConfig {
    srs_paths: Vec<PathBuf>,
    root_dir: PathBuf,
    out_xml: Option<PathBuf>,
    out_matrix: Option<PathBuf>,
    write_lock: Option<PathBuf>,
    verify_lock: Option<PathBuf>,
    check_orphans: bool,
}

fn parse_args() -> Result<CliConfig, TraceError> {
    let mut cfg = CliConfig {
        srs_paths: Vec::new(),
        root_dir: PathBuf::from("."),
        out_xml: None,
        out_matrix: None,
        write_lock: None,
        verify_lock: None,
        check_orphans: false,
    };

    let mut args = env::args().skip(1);
    while let Some(arg) = args.next() {
        match arg.as_str() {
            "scan" | "verify" => {}
            "--srs" => {
                let val = args
                    .next()
                    .ok_or_else(|| TraceError::Parse("missing value for --srs".to_string()))?;
                cfg.srs_paths.push(PathBuf::from(val));
            }
            "--srs-dir" => {
                let val = args.next().ok_or_else(|| {
                    TraceError::Parse("missing value for --srs-dir".to_string())
                })?;
                collect_srs_dir(Path::new(&val), &mut cfg.srs_paths)?;
            }
            "--root" => {
                let val = args
                    .next()
                    .ok_or_else(|| TraceError::Parse("missing value for --root".to_string()))?;
                cfg.root_dir = PathBuf::from(val);
            }
            "--out-xml" => {
                let val = args
                    .next()
                    .ok_or_else(|| TraceError::Parse("missing value for --out-xml".to_string()))?;
                cfg.out_xml = Some(PathBuf::from(val));
            }
            "--out-matrix" => {
                let val = args.next().ok_or_else(|| {
                    TraceError::Parse("missing value for --out-matrix".to_string())
                })?;
                cfg.out_matrix = Some(PathBuf::from(val));
            }
            "--write-lock" => {
                let val = args.next().ok_or_else(|| {
                    TraceError::Parse("missing value for --write-lock".to_string())
                })?;
                cfg.write_lock = Some(PathBuf::from(val));
            }
            "--verify-lock" => {
                let val = args.next().ok_or_else(|| {
                    TraceError::Parse("missing value for --verify-lock".to_string())
                })?;
                cfg.verify_lock = Some(PathBuf::from(val));
            }
            "--check-orphans" => cfg.check_orphans = true,
            other => return Err(TraceError::Parse(format!("unknown argument: {other}"))),
        }
    }

    if cfg.srs_paths.is_empty() {
        return Err(TraceError::Parse(
            "usage: aegis-trace scan (--srs <SRS.yaml> | --srs-dir <dir>) [--root <dir>] [--out-xml <file>] [--out-matrix <file>] [--write-lock <file>] [--verify-lock <file>] [--check-orphans]"
                .to_string(),
        ));
    }

    Ok(cfg)
}

fn collect_srs_dir(dir: &Path, out: &mut Vec<PathBuf>) -> Result<(), TraceError> {
    let entries = fs::read_dir(dir)
        .map_err(|e| TraceError::Io(format!("failed to read SRS dir {}: {e}", dir.display())))?;
    let mut paths = Vec::new();
    for entry_res in entries {
        let entry = entry_res.map_err(|e| TraceError::Io(e.to_string()))?;
        let p = entry.path();
        if p.extension().and_then(|e| e.to_str()) == Some("yaml") {
            paths.push(p);
        }
    }
    paths.sort();
    out.extend(paths);
    Ok(())
}

fn write_output_file(path: &Path, content: &str) -> Result<(), TraceError> {
    if let Some(parent) = path.parent() {
        if !parent.as_os_str().is_empty() {
            fs::create_dir_all(parent).map_err(|e| {
                TraceError::Io(format!("failed to create directory {}: {e}", parent.display()))
            })?;
        }
    }
    fs::write(path, content)
        .map_err(|e| TraceError::Io(format!("failed to write output {}: {e}", path.display())))
}

fn run() -> Result<(), TraceError> {
    let cfg = parse_args()?;
    let records = scan_directory(&cfg.root_dir)?;

    let mut srs_docs = Vec::with_capacity(cfg.srs_paths.len());
    for path in &cfg.srs_paths {
        srs_docs.push(SrsDocument::from_file(path)?);
    }

    if cfg.check_orphans {
        check_orphan_requirements(&srs_docs, &records)?;
        println!(
            "[AEGIS-TRACE] Zero orphan @req tags verified across {} source links.",
            records.len()
        );
    }

    for srs in &srs_docs {
        let coverage = verify_traceability(srs, &records)?;
        let total_links: usize = coverage.iter().map(|c| c.traces.len()).sum();
        println!(
            "[AEGIS-TRACE] Document '{}' ({}): {}/{} requirements verified ({} trace links).",
            srs.document_id,
            srs.dal_level,
            coverage.len(),
            srs.requirements.len(),
            total_links
        );

        if srs_docs.len() == 1 {
            if let Some(xml_path) = &cfg.out_xml {
                let xml = generate_s1000d_fault_xml(srs, &coverage);
                write_output_file(xml_path, &xml)?;
            }
            if let Some(matrix_path) = &cfg.out_matrix {
                let md = render_trace_matrix_md(srs, &coverage);
                write_output_file(matrix_path, &md)?;
            }
        }
    }

    if let Some(lock_out) = &cfg.write_lock {
        let lock = render_trace_lock(&records);
        write_output_file(lock_out, &lock)?;
        println!("[AEGIS-TRACE] Trace lockfile written to: {}", lock_out.display());
    }

    if let Some(lock_in) = &cfg.verify_lock {
        let content = fs::read_to_string(lock_in).map_err(|e| {
            TraceError::Io(format!("failed to read lockfile {}: {e}", lock_in.display()))
        })?;
        verify_trace_lock(&content, &records)?;
        println!("[AEGIS-TRACE] Lockfile integrity verified: {}", lock_in.display());
    }

    Ok(())
}

fn main() -> ExitCode {
    match run() {
        Ok(()) => ExitCode::SUCCESS,
        Err(err) => {
            eprintln!("{err}");
            ExitCode::FAILURE
        }
    }
}
