//! Source Code Traceability Scanner (`@req`, `@fault`, `@hazard`) & Body Fingerprinter.

use crate::sha256::sha256_hex;
use crate::TraceError;
use std::fs;
use std::path::{Path, PathBuf};

/// Represents a traced code artifact extracted from C, C++, or Rust source files.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct TraceRecord {
    /// Source file relative or absolute path.
    pub file_path: String,
    /// 1-based line number where the annotation block begins.
    pub line_number: usize,
    /// Traced requirement ID (`@req`).
    pub req_id: String,
    /// Associated Diagnostic Trouble Code (`@fault`).
    pub fault_code: String,
    /// Associated MIL-STD-882E hazard category (`@hazard`).
    pub hazard_cat: String,
    /// Identified function or type symbol name.
    pub symbol_name: String,
    /// Normalized SHA-256 fingerprint of the traced declaration/function body.
    pub body_sha256: String,
}

/// Recursively scans `root` for `.c`, `.h`, `.cpp`, `.hpp`, and `.rs` files and extracts trace records.
///
/// # Errors
/// Returns [`TraceError`] if directory traversal or file reading fails.
pub fn scan_directory(root: &Path) -> Result<Vec<TraceRecord>, TraceError> {
    let mut files = Vec::new();
    collect_source_files(root, &mut files)?;
    files.sort();

    let mut records = Vec::new();
    for file in files {
        let mut file_records = scan_file(&file, root)?;
        records.append(&mut file_records);
    }
    Ok(records)
}

fn collect_source_files(dir: &Path, out: &mut Vec<PathBuf>) -> Result<(), TraceError> {
    let entries = fs::read_dir(dir).map_err(|e| {
        TraceError::Io(format!("failed to read directory {}: {e}", dir.display()))
    })?;

    for entry_res in entries {
        let entry = entry_res.map_err(|e| TraceError::Io(e.to_string()))?;
        let path = entry.path();
        let file_name = entry.file_name();
        let name_str = file_name.to_string_lossy();

        if path.is_dir() {
            if matches!(
                name_str.as_ref(),
                ".git" | "build" | "target" | "reports" | "05-trace-s1000d-cli"
            ) {
                continue;
            }
            collect_source_files(&path, out)?;
        } else if let Some(ext) = path.extension().and_then(|e| e.to_str()) {
            if matches!(ext, "c" | "h" | "cpp" | "hpp" | "rs" | "java") {
                out.push(path);
            }
        }
    }
    Ok(())
}

/// Scans a single source file for `@req`, `@fault`, and `@hazard` annotations.
///
/// # Errors
/// Returns [`TraceError`] if the file cannot be read.
pub fn scan_file(path: &Path, root: &Path) -> Result<Vec<TraceRecord>, TraceError> {
    let content = fs::read_to_string(path).map_err(|e| {
        TraceError::Io(format!("failed to read source file {}: {e}", path.display()))
    })?;
    let rel_path = path
        .strip_prefix(root)
        .unwrap_or(path)
        .to_string_lossy()
        .replace('\\', "/");
    Ok(scan_source_str(&content, &rel_path))
}

/// Scans an in-memory source string and extracts all [`TraceRecord`] entries.
#[must_use]
pub fn scan_source_str(content: &str, file_label: &str) -> Vec<TraceRecord> {
    let lines: Vec<&str> = content.lines().collect();
    let mut records = Vec::new();

    let mut pending_reqs: Vec<(usize, String)> = Vec::new();
    let mut pending_fault = String::new();
    let mut pending_hazard = String::new();

    let mut idx = 0;
    while idx < lines.len() {
        let line = lines[idx].trim();

        if let Some(val) = extract_tag_value(line, "@req") {
            pending_reqs.push((idx + 1, val));
            idx += 1;
            continue;
        }
        if let Some(val) = extract_tag_value(line, "@fault") {
            pending_fault = val;
            idx += 1;
            continue;
        }
        if let Some(val) = extract_tag_value(line, "@hazard") {
            pending_hazard = val;
            idx += 1;
            continue;
        }

        let is_comment_or_empty = line.is_empty()
            || line.starts_with("/*")
            || line.starts_with('*')
            || line.starts_with("*/")
            || line.starts_with("//");

        if !pending_reqs.is_empty() && !is_comment_or_empty {
            let (body_end, symbol_name, body_hash) = extract_code_block(&lines, idx);
            for (req_line, req_id) in pending_reqs.drain(..) {
                records.push(TraceRecord {
                    file_path: file_label.to_string(),
                    line_number: req_line,
                    req_id,
                    fault_code: pending_fault.clone(),
                    hazard_cat: pending_hazard.clone(),
                    symbol_name: symbol_name.clone(),
                    body_sha256: body_hash.clone(),
                });
            }
            pending_fault.clear();
            pending_hazard.clear();
            idx = body_end + 1;
            continue;
        }

        idx += 1;
    }

    records
}

fn extract_tag_value(line: &str, tag: &str) -> Option<String> {
    let pos = line.find(tag)?;
    let after = &line[pos + tag.len()..];
    let token = after
        .trim()
        .trim_end_matches("*/")
        .split_whitespace()
        .next()?;
    let cleaned = token.trim_end_matches(':');
    if cleaned.is_empty() {
        None
    } else {
        Some(cleaned.to_string())
    }
}

fn extract_code_block(lines: &[&str], start_idx: usize) -> (usize, String, String) {
    let mut normalized = Vec::new();
    let mut brace_depth: i32 = 0;
    let mut seen_open_brace = false;
    let mut end_idx = start_idx;
    let mut signature_buf = String::new();

    for (offset, raw_line) in lines[start_idx..].iter().enumerate() {
        let current_idx = start_idx + offset;
        end_idx = current_idx;
        let line_no_comment = strip_line_comment(raw_line).trim();

        if !seen_open_brace {
            if !signature_buf.is_empty() {
                signature_buf.push(' ');
            }
            signature_buf.push_str(line_no_comment);
        }

        for ch in line_no_comment.chars() {
            if !ch.is_whitespace() {
                normalized.push(ch as u8);
            }
            if ch == '{' {
                brace_depth += 1;
                seen_open_brace = true;
            } else if ch == '}' {
                brace_depth -= 1;
            }
        }

        if seen_open_brace && brace_depth <= 0 {
            if line_no_comment.ends_with(';') || !signature_buf.starts_with("typedef") {
                break;
            }
        } else if !seen_open_brace && line_no_comment.ends_with(';') {
            break;
        }
    }

    let symbol_name = parse_symbol_name(&signature_buf);
    let digest = sha256_hex(&normalized);
    (end_idx, symbol_name, digest)
}

fn strip_line_comment(line: &str) -> &str {
    if let Some(pos) = line.find("//") {
        &line[..pos]
    } else {
        line
    }
}

fn parse_symbol_name(signature: &str) -> String {
    let trimmed = signature.trim();
    if trimmed.starts_with("typedef struct") {
        if let Some((_, after_struct)) = trimmed.split_once("typedef struct") {
            if let Some(name) = after_struct.split_whitespace().next() {
                let clean = name.trim_matches('{');
                if !clean.is_empty() {
                    return clean.to_string();
                }
            }
        }
    }
    if let Some(paren_pos) = trimmed.find('(') {
        let before_paren = trimmed[..paren_pos].trim();
        if let Some(last_tok) = before_paren.split_whitespace().last() {
            let clean = last_tok.trim_start_matches('*');
            if !clean.is_empty() {
                return clean.to_string();
            }
        }
    }
    "anonymous_block".to_string()
}
