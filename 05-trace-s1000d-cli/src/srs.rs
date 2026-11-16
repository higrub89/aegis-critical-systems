//! Structured Low-Level Requirements Specification (SRS) YAML Loader.

use crate::TraceError;
use std::fs;
use std::path::Path;

/// Represents a single DO-178C Low-Level Requirement with MIL-STD-882E / S1000D links.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct SrsRequirement {
    /// Requirement identifier (e.g., `SRS-GW-001`).
    pub id: String,
    /// Short engineering title.
    pub title: String,
    /// DO-178C Design Assurance Level (e.g., `DAL-A`).
    pub dal: String,
    /// MIL-STD-882E Hazard Category (e.g., `MIL-882E-CAT-I`).
    pub hazard: String,
    /// Diagnostic Trouble Code (e.g., `DTC-0x101`).
    pub fault: String,
    /// Normative requirement text.
    pub description: String,
    /// S1000D fault isolation / corrective action text.
    pub isolation_action: String,
}

/// Represents a parsed SRS document.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct SrsDocument {
    /// Document identifier (e.g., `SRS-GW-001`).
    pub document_id: String,
    /// Target subsystem identifier.
    pub subsystem: String,
    /// Default DAL level.
    pub dal_level: String,
    /// S1000D Data Module Code.
    pub s1000d_dmc: String,
    /// List of low-level requirements.
    pub requirements: Vec<SrsRequirement>,
}

impl SrsDocument {
    /// Loads and parses an SRS YAML specification from `path`.
    ///
    /// # Errors
    /// Returns [`TraceError`] if the file cannot be read or is missing mandatory fields.
    pub fn from_file(path: &Path) -> Result<Self, TraceError> {
        let content = fs::read_to_string(path).map_err(|e| {
            TraceError::Io(format!("failed to read SRS file {}: {e}", path.display()))
        })?;
        Self::parse_yaml(&content)
    }

    /// Parses an SRS YAML specification string.
    ///
    /// # Errors
    /// Returns [`TraceError`] if required document or requirement attributes are missing.
    pub fn parse_yaml(content: &str) -> Result<Self, TraceError> {
        let mut document_id = String::new();
        let mut subsystem = String::new();
        let mut dal_level = String::new();
        let mut s1000d_dmc = String::new();
        let mut requirements: Vec<SrsRequirement> = Vec::new();

        let mut current_req: Option<SrsRequirement> = None;
        let mut multiline_field: Option<&str> = None;

        for raw_line in content.lines() {
            let trimmed = raw_line.trim();
            if trimmed.is_empty() || trimmed.starts_with('#') {
                continue;
            }

            let indent = raw_line.len() - raw_line.trim_start().len();

            if indent >= 6 {
                if let (Some(req), Some(field)) = (&mut current_req, multiline_field) {
                    let target = if field == "description" {
                        &mut req.description
                    } else {
                        &mut req.isolation_action
                    };
                    if !target.is_empty() {
                        target.push(' ');
                    }
                    target.push_str(trimmed);
                    continue;
                }
            } else {
                multiline_field = None;
            }

            if let Some(rest) = trimmed.strip_prefix("- id:") {
                if let Some(prev) = current_req.take() {
                    requirements.push(prev);
                }
                current_req = Some(SrsRequirement {
                    id: rest.trim().to_string(),
                    title: String::new(),
                    dal: String::new(),
                    hazard: String::new(),
                    fault: String::new(),
                    description: String::new(),
                    isolation_action: String::new(),
                });
                continue;
            }

            if let Some((key, val_raw)) = trimmed.split_once(':') {
                let key = key.trim();
                let val = val_raw.trim();

                if let Some(req) = &mut current_req {
                    multiline_field = apply_requirement_field(req, key, val);
                } else {
                    match key {
                        "document_id" => document_id = val.to_string(),
                        "subsystem" => subsystem = val.to_string(),
                        "dal_level" => dal_level = val.to_string(),
                        "s1000d_dmc" => s1000d_dmc = val.to_string(),
                        _ => {}
                    }
                }
            }
        }

        if let Some(last) = current_req.take() {
            requirements.push(last);
        }

        validate_srs_document(&document_id, &requirements)?;

        Ok(Self {
            document_id,
            subsystem,
            dal_level,
            s1000d_dmc,
            requirements,
        })
    }
}

fn apply_requirement_field<'a>(
    req: &mut SrsRequirement,
    key: &str,
    val: &str,
) -> Option<&'a str> {
    match key {
        "title" => req.title = val.to_string(),
        "dal" => req.dal = val.to_string(),
        "hazard" => req.hazard = val.to_string(),
        "fault" => req.fault = val.to_string(),
        "description" => {
            if val == ">" || val == "|" {
                return Some("description");
            }
            req.description = val.to_string();
        }
        "isolation_action" => {
            if val == ">" || val == "|" {
                return Some("isolation_action");
            }
            req.isolation_action = val.to_string();
        }
        _ => {}
    }
    None
}

fn validate_srs_document(
    document_id: &str,
    requirements: &[SrsRequirement],
) -> Result<(), TraceError> {
    if document_id.is_empty() || requirements.is_empty() {
        return Err(TraceError::Parse(
            "SRS YAML missing document_id or requirements list".to_string(),
        ));
    }
    for req in requirements {
        if req.id.is_empty() || req.fault.is_empty() || req.hazard.is_empty() {
            return Err(TraceError::Parse(format!(
                "requirement '{}' missing mandatory id, fault, or hazard tag",
                req.id
            )));
        }
    }
    Ok(())
}
