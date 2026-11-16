//! AEGIS DO-178C to S1000D Issue 5.0 BREX Traceability Compiler Library.

pub mod s1000d;
pub mod scanner;
pub mod sha256;
pub mod srs;

use std::fmt;

/// Domain error type for traceability scanning, verification, and S1000D emission.
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum TraceError {
    /// File or directory I/O error.
    Io(String),
    /// SRS YAML or command-line parsing error.
    Parse(String),
    /// DO-178C / MIL-STD-882E traceability verification failure.
    VerificationFailed(String),
}

impl fmt::Display for TraceError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            Self::Io(msg) => write!(f, "[IO_ERROR] {msg}"),
            Self::Parse(msg) => write!(f, "[PARSE_ERROR] {msg}"),
            Self::VerificationFailed(msg) => write!(f, "[TRACE_VERIFICATION_FAILED] {msg}"),
        }
    }
}

impl std::error::Error for TraceError {}
