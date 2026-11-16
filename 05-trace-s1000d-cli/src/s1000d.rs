//! DO-178C Traceability Verifier & S1000D Issue 5.0 BREX (`fault.xsd`) XML Generator.

use crate::scanner::TraceRecord;
use crate::srs::SrsDocument;
use crate::TraceError;
use std::collections::HashSet;
use std::fmt::Write as _;

/// Validates that no source trace tag references an unknown `@req` outside `known_srs_docs`.
///
/// # Errors
/// Returns [`TraceError::VerificationFailed`] if any orphan `@req` annotation is detected.
pub fn check_orphan_requirements(
    srs_docs: &[SrsDocument],
    records: &[TraceRecord],
) -> Result<(), TraceError> {
    let mut known_ids = HashSet::new();
    for doc in srs_docs {
        for req in &doc.requirements {
            known_ids.insert(req.id.as_str());
        }
    }
    for trace in records {
        if !known_ids.contains(trace.req_id.as_str()) {
            return Err(TraceError::VerificationFailed(format!(
                "orphan @req '{}' at {}:{} ({}) not found in any SRS document",
                trace.req_id, trace.file_path, trace.line_number, trace.symbol_name
            )));
        }
    }
    Ok(())
}

/// Renders deterministic sorted lockfile entries (`req_id|file_path|symbol_name|body_sha256`).
#[must_use]
pub fn render_trace_lock(records: &[TraceRecord]) -> String {
    let mut entries: Vec<String> = records
        .iter()
        .map(|r| format!("{}|{}|{}|{}", r.req_id, r.file_path, r.symbol_name, r.body_sha256))
        .collect();
    entries.sort();
    entries.dedup();
    let mut out = String::new();
    for line in entries {
        out.push_str(&line);
        out.push('\n');
    }
    out
}

/// Verifies current scanned trace records strictly match `lock_content`.
///
/// # Errors
/// Returns [`TraceError::VerificationFailed`] on any SHA-256 drift or added/removed trace link.
pub fn verify_trace_lock(lock_content: &str, records: &[TraceRecord]) -> Result<(), TraceError> {
    let expected: Vec<&str> = lock_content
        .lines()
        .map(str::trim)
        .filter(|l| !l.is_empty() && !l.starts_with('#'))
        .collect();
    let actual_str = render_trace_lock(records);
    let actual: Vec<&str> = actual_str
        .lines()
        .map(str::trim)
        .filter(|l| !l.is_empty())
        .collect();

    if expected == actual {
        return Ok(());
    }

    for entry in &actual {
        if !expected.contains(entry) {
            return Err(TraceError::VerificationFailed(format!(
                "trace.lock drift detected for entry '{entry}' (re-audit and update requirements/trace.lock)"
            )));
        }
    }
    Err(TraceError::VerificationFailed(
        "trace.lock mismatch: traced requirement removed or modified".to_string(),
    ))
}

/// Verification status of a single SRS requirement against scanned source code.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct RequirementCoverage {
    /// Requirement ID (`SRS-...`).
    pub req_id: String,
    /// Expected fault code (`DTC-...`).
    pub expected_fault: String,
    /// Expected hazard category (`MIL-882E-...`).
    pub expected_hazard: String,
    /// Matching source trace records.
    pub traces: Vec<TraceRecord>,
}

/// Validates that every requirement in `srs` is implemented and tagged in `records`.
///
/// # Errors
/// Returns [`TraceError::VerificationFailed`] if any requirement is untraced or has mismatched
/// `@fault` / `@hazard` annotations.
pub fn verify_traceability(
    srs: &SrsDocument,
    records: &[TraceRecord],
) -> Result<Vec<RequirementCoverage>, TraceError> {
    let mut report = Vec::with_capacity(srs.requirements.len());

    for req in &srs.requirements {
        let matching: Vec<TraceRecord> = records
            .iter()
            .filter(|r| r.req_id == req.id)
            .cloned()
            .collect();

        if matching.is_empty() {
            return Err(TraceError::VerificationFailed(format!(
                "DO-178C gap: requirement '{}' ({}) has zero source trace links",
                req.id, req.title
            )));
        }

        for trace in &matching {
            if trace.fault_code != req.fault {
                return Err(TraceError::VerificationFailed(format!(
                    "fault mismatch for {} at {}:{}: expected '{}', found '{}'",
                    req.id, trace.file_path, trace.line_number, req.fault, trace.fault_code
                )));
            }
            if trace.hazard_cat != req.hazard {
                return Err(TraceError::VerificationFailed(format!(
                    "hazard mismatch for {} at {}:{}: expected '{}', found '{}'",
                    req.id, trace.file_path, trace.line_number, req.hazard, trace.hazard_cat
                )));
            }
        }

        report.push(RequirementCoverage {
            req_id: req.id.clone(),
            expected_fault: req.fault.clone(),
            expected_hazard: req.hazard.clone(),
            traces: matching,
        });
    }

    Ok(report)
}

/// Renders a Markdown DO-178C Traceability & SHA-256 Integrity Matrix.
#[must_use]
pub fn render_trace_matrix_md(srs: &SrsDocument, coverage: &[RequirementCoverage]) -> String {
    let mut out = String::new();
    let _ = writeln!(
        out,
        "# DO-178C Bidirectional Traceability Matrix — `{}`\n",
        srs.document_id
    );
    let _ = writeln!(out, "- **Subsystem:** `{}`", srs.subsystem);
    let _ = writeln!(out, "- **Assurance Level:** `{}`", srs.dal_level);
    let _ = writeln!(out, "- **S1000D DMC:** `{}`\n", srs.s1000d_dmc);
    let _ = writeln!(
        out,
        "| Requirement ID | DAL | Hazard Category | DTC Fault | Source Location | Symbol | Body SHA-256 (Normalized) |"
    );
    let _ = writeln!(
        out,
        "| :--- | :--- | :--- | :--- | :--- | :--- | :--- |"
    );

    for cov in coverage {
        for trace in &cov.traces {
            let _ = writeln!(
                out,
                "| `{}` | `{}` | `{}` | `{}` | `{}:{}` | `{}` | `{}` |",
                cov.req_id,
                srs.dal_level,
                cov.expected_hazard,
                cov.expected_fault,
                trace.file_path,
                trace.line_number,
                trace.symbol_name,
                trace.body_sha256
            );
        }
    }

    out
}

/// Renders an S1000D Issue 5.0 BREX-compliant Fault Isolation Data Module (`fault.xsd` profile).
#[must_use]
pub fn generate_s1000d_fault_xml(
    srs: &SrsDocument,
    coverage: &[RequirementCoverage],
) -> String {
    let mut xml = String::new();
    let _ = writeln!(xml, "<?xml version=\"1.0\" encoding=\"UTF-8\"?>");
    let _ = writeln!(
        xml,
        "<dmodule xmlns:xsi=\"http://www.w3.org/2001/XMLSchema-instance\" xsi:noNamespaceSchemaLocation=\"http://www.s1000d.org/S1000D_5-0/xml_schema_flat/fault.xsd\">"
    );
    write_ident_and_status_section(&mut xml, srs);
    let _ = writeln!(xml, "  <content>");
    let _ = writeln!(xml, "    <faultIsolation>");

    for req in &srs.requirements {
        let cov_opt = coverage.iter().find(|c| c.req_id == req.id);
        let _ = writeln!(
            xml,
            "      <faultIsolationProcedure id=\"FIP-{}\" faultCode=\"{}\">",
            escape_xml(&req.id),
            escape_xml(&req.fault)
        );
        let _ = writeln!(
            xml,
            "        <fault faultCode=\"{}\"><faultDescr><simplePara>[{}] {} — {}</simplePara></faultDescr></fault>",
            escape_xml(&req.fault),
            escape_xml(&req.hazard),
            escape_xml(&req.id),
            escape_xml(&req.title)
        );
        let _ = writeln!(xml, "        <isolationProcedure>");
        let _ = writeln!(
            xml,
            "          <preliminaryRqmts><reqCondGroup><noConds/></reqCondGroup></preliminaryRqmts>"
        );
        let _ = writeln!(
            xml,
            "          <isolationStep id=\"STEP-{}-01\">",
            escape_xml(&req.id)
        );
        let _ = writeln!(
            xml,
            "            <action><simplePara>{}</simplePara></action>",
            escape_xml(&req.isolation_action)
        );
        if let Some(cov) = cov_opt {
            for trace in &cov.traces {
                let _ = writeln!(
                    xml,
                    "            <isolationStepQuestion><simplePara>Verified Symbol: {} ({}:{}) | SHA-256: {}</simplePara></isolationStepQuestion>",
                    escape_xml(&trace.symbol_name),
                    escape_xml(&trace.file_path),
                    trace.line_number,
                    escape_xml(&trace.body_sha256)
                );
            }
        }
        let _ = writeln!(xml, "          </isolationStep>");
        let _ = writeln!(
            xml,
            "          <closeRqmts><reqCondGroup><noConds/></reqCondGroup></closeRqmts>"
        );
        let _ = writeln!(xml, "        </isolationProcedure>");
        let _ = writeln!(xml, "      </faultIsolationProcedure>");
    }

    let _ = writeln!(xml, "    </faultIsolation>");
    let _ = writeln!(xml, "  </content>");
    let _ = writeln!(xml, "</dmodule>");
    xml
}

fn write_dm_code(xml: &mut String, dmc: &str) {
    let clean = dmc.strip_prefix("DMC-").unwrap_or(dmc);
    let parts: Vec<&str> = clean.split('-').collect();
    if parts.len() == 8 {
        let sub_sys = parts[3].chars().next().unwrap_or('0');
        let sub_sub = parts[3].chars().nth(1).unwrap_or('0');
        let disassy_code = parts[5].get(..2).unwrap_or("00");
        let disassy_var = parts[5].get(2..).unwrap_or("A");
        let info_code = parts[6].get(..3).unwrap_or("420");
        let info_var = parts[6].get(3..).unwrap_or("A");
        let _ = writeln!(
            xml,
            "        <dmCode modelIdentCode=\"{}\" systemDiffCode=\"{}\" systemCode=\"{}\" subSystemCode=\"{}\" subSubSystemCode=\"{}\" assyCode=\"{}\" disassyCode=\"{}\" disassyCodeVariant=\"{}\" infoCode=\"{}\" infoCodeVariant=\"{}\" itemLocationCode=\"{}\"/>",
            escape_xml(parts[0]),
            escape_xml(parts[1]),
            escape_xml(parts[2]),
            sub_sys,
            sub_sub,
            escape_xml(parts[4]),
            escape_xml(disassy_code),
            escape_xml(disassy_var),
            escape_xml(info_code),
            escape_xml(info_var),
            escape_xml(parts[7])
        );
        return;
    }
    let _ = writeln!(
        xml,
        "        <dmCode modelIdentCode=\"AEGIS\" systemDiffCode=\"A\" systemCode=\"24\" subSystemCode=\"3\" subSubSystemCode=\"0\" assyCode=\"01\" disassyCode=\"00\" disassyCodeVariant=\"A\" infoCode=\"420\" infoCodeVariant=\"A\" itemLocationCode=\"A\"/>"
    );
}

fn write_ident_and_status_section(xml: &mut String, srs: &SrsDocument) {
    let _ = writeln!(xml, "  <identAndStatusSection>");
    let _ = writeln!(xml, "    <dmAddress>");
    let _ = writeln!(xml, "      <dmIdent>");
    write_dm_code(xml, &srs.s1000d_dmc);
    let _ = writeln!(
        xml,
        "        <language languageIsoCode=\"en\" countryIsoCode=\"US\"/>"
    );
    let _ = writeln!(xml, "        <issueInfo issueNumber=\"001\" inWork=\"00\"/>");
    let _ = writeln!(xml, "      </dmIdent>");
    let _ = writeln!(xml, "      <dmAddressItems>");
    let _ = writeln!(
        xml,
        "        <issueDate year=\"2026\" month=\"11\" day=\"16\"/>"
    );
    let _ = writeln!(xml, "        <dmTitle>");
    let _ = writeln!(
        xml,
        "          <techName>{}</techName>",
        escape_xml(&srs.subsystem)
    );
    let _ = writeln!(
        xml,
        "          <infoName>Fault Isolation and DO-178C Traceability ({})</infoName>",
        escape_xml(&srs.document_id)
    );
    let _ = writeln!(xml, "        </dmTitle>");
    let _ = writeln!(xml, "      </dmAddressItems>");
    let _ = writeln!(xml, "    </dmAddress>");
    let _ = writeln!(xml, "    <dmStatus issueType=\"new\">");
    let _ = writeln!(xml, "      <security securityClassification=\"01\"/>");
    let _ = writeln!(
        xml,
        "      <brexDmRef><dmRef><dmRefIdent><dmCode modelIdentCode=\"AEGIS\" systemDiffCode=\"A\" systemCode=\"00\" subSystemCode=\"0\" subSubSystemCode=\"0\" assyCode=\"00\" disassyCode=\"00\" disassyCodeVariant=\"A\" infoCode=\"022\" infoCodeVariant=\"A\" itemLocationCode=\"D\"/></dmRefIdent></dmRef></brexDmRef>"
    );
    let _ = writeln!(
        xml,
        "      <qualityAssurance><firstVerification verificationType=\"tabtop\"/></qualityAssurance>"
    );
    let _ = writeln!(xml, "    </dmStatus>");
    let _ = writeln!(xml, "  </identAndStatusSection>");
}

fn escape_xml(input: &str) -> String {
    let mut out = String::with_capacity(input.len());
    for ch in input.chars() {
        match ch {
            '&' => out.push_str("&amp;"),
            '<' => out.push_str("&lt;"),
            '>' => out.push_str("&gt;"),
            '"' => out.push_str("&quot;"),
            '\'' => out.push_str("&apos;"),
            _ => out.push(ch),
        }
    }
    out
}
