//! CLI entry point for `aegis-trace` (DO-178C & S1000D Issue 5.0 BREX Compiler).

use aegis_trace::s1000d::{generate_s1000d_fault_xml, render_trace_matrix_md, verify_traceability};
use aegis_trace::scanner::scan_directory;
use aegis_trace::srs::SrsDocument;
use aegis_trace::TraceError;
use std::env;
use std::fs;
use std::path::{Path, PathBuf};
use std::process::ExitCode;

struct CliConfig {
    srs_path: PathBuf,
    root_dir: PathBuf,
    out_xml: Option<PathBuf>,
    out_matrix: Option<PathBuf>,
}

fn parse_args() -> Result<CliConfig, TraceError> {
    let mut srs_path: Option<PathBuf> = None;
    let mut root_dir = PathBuf::from(".");
    let mut out_xml: Option<PathBuf> = None;
    let mut out_matrix: Option<PathBuf> = None;

    let mut args = env::args().skip(1);
    while let Some(arg) = args.next() {
        match arg.as_str() {
            "scan" | "verify" => {}
            "--srs" => {
                let val = args
                    .next()
                    .ok_or_else(|| TraceError::Parse("missing value for --srs".to_string()))?;
                srs_path = Some(PathBuf::from(val));
            }
            "--root" => {
                let val = args
                    .next()
                    .ok_or_else(|| TraceError::Parse("missing value for --root".to_string()))?;
                root_dir = PathBuf::from(val);
            }
            "--out-xml" => {
                let val = args
                    .next()
                    .ok_or_else(|| TraceError::Parse("missing value for --out-xml".to_string()))?;
                out_xml = Some(PathBuf::from(val));
            }
            "--out-matrix" => {
                let val = args.next().ok_or_else(|| {
                    TraceError::Parse("missing value for --out-matrix".to_string())
                })?;
                out_matrix = Some(PathBuf::from(val));
            }
            other => {
                return Err(TraceError::Parse(format!("unknown argument: {other}")));
            }
        }
    }

    let srs = srs_path.ok_or_else(|| {
        TraceError::Parse(
            "usage: aegis-trace scan --srs <SRS.yaml> [--root <dir>] [--out-xml <file.xml>] [--out-matrix <file.md>]".to_string(),
        )
    })?;

    Ok(CliConfig {
        srs_path: srs,
        root_dir,
        out_xml,
        out_matrix,
    })
}

fn write_output_file(path: &Path, content: &str) -> Result<(), TraceError> {
    if let Some(parent) = path.parent() {
        if !parent.as_os_str().is_empty() {
            fs::create_dir_all(parent).map_err(|e| {
                TraceError::Io(format!("failed to create directory {}: {e}", parent.display()))
            })?;
        }
    }
    fs::write(path, content).map_err(|e| {
        TraceError::Io(format!("failed to write output {}: {e}", path.display()))
    })
}

fn run() -> Result<(), TraceError> {
    let cfg = parse_args()?;
    let srs = SrsDocument::from_file(&cfg.srs_path)?;
    let records = scan_directory(&cfg.root_dir)?;
    let coverage = verify_traceability(&srs, &records)?;

    let total_links: usize = coverage.iter().map(|c| c.traces.len()).sum();
    println!(
        "[AEGIS-TRACE] Document '{}' ({}): {}/{} requirements verified ({} trace links).",
        srs.document_id,
        srs.dal_level,
        coverage.len(),
        srs.requirements.len(),
        total_links
    );

    for cov in &coverage {
        for trace in &cov.traces {
            println!(
                "  -> [{}] {} | {} | {}:{} ({}) | SHA-256: {}",
                cov.req_id,
                cov.expected_fault,
                cov.expected_hazard,
                trace.file_path,
                trace.line_number,
                trace.symbol_name,
                &trace.body_sha256[..16]
            );
        }
    }

    if let Some(xml_path) = &cfg.out_xml {
        let xml = generate_s1000d_fault_xml(&srs, &coverage);
        write_output_file(xml_path, &xml)?;
        println!(
            "[AEGIS-TRACE] S1000D Issue 5.0 Fault Data Module written to: {}",
            xml_path.display()
        );
    }

    if let Some(matrix_path) = &cfg.out_matrix {
        let md = render_trace_matrix_md(&srs, &coverage);
        write_output_file(matrix_path, &md)?;
        println!(
            "[AEGIS-TRACE] DO-178C Traceability Matrix written to: {}",
            matrix_path.display()
        );
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
