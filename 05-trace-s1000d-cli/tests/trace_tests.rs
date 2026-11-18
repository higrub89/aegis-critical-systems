//! Integration and unit verification suite for `aegis-trace`.

use aegis_trace::s1000d::{
    check_orphan_requirements, generate_s1000d_fault_xml, render_trace_lock, verify_s1000d_brex,
    verify_trace_lock, verify_traceability,
};
use aegis_trace::scanner::scan_source_str;
use aegis_trace::sha256::sha256_hex;
use aegis_trace::srs::SrsDocument;

#[test]
fn fips_180_4_sha256_test_vectors() {
    assert_eq!(
        sha256_hex(b""),
        "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"
    );
    assert_eq!(
        sha256_hex(b"abc"),
        "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"
    );
}

#[test]
fn scanner_fingerprint_invariant_to_whitespace_sensitive_to_logic() {
    let src_v1 = r"
/*
 * @req SRS-GW-002
 * @fault DTC-0x102
 * @hazard MIL-882E-CAT-I
 */
int check_len(unsigned int len)
{
    if (len > 64U)
        return (1);
    return (0);
}
";

    let src_v2_reformatted = r"
// @req SRS-GW-002
// @fault DTC-0x102
// @hazard MIL-882E-CAT-I
int check_len( unsigned int len ) {
    if ( len > 64U ) return (1);
    return (0);
}
";

    let src_v3_mutated = r"
// @req SRS-GW-002
// @fault DTC-0x102
// @hazard MIL-882E-CAT-I
int check_len(unsigned int len) {
    if (len >= 64U)
        return (1);
    return (0);
}
";

    let rec1 = scan_source_str(src_v1, "test.c");
    let rec2 = scan_source_str(src_v2_reformatted, "test.c");
    let rec3 = scan_source_str(src_v3_mutated, "test.c");

    assert_eq!(rec1.len(), 1);
    assert_eq!(rec2.len(), 1);
    assert_eq!(rec3.len(), 1);

    assert_eq!(rec1[0].symbol_name, "check_len");
    assert_eq!(rec1[0].body_sha256, rec2[0].body_sha256);
    assert_ne!(rec1[0].body_sha256, rec3[0].body_sha256);
}

#[test]
fn end_to_end_srs_verification_and_s1000d_emission() {
    let yaml = r"
document_id: SRS-TEST-001
subsystem: 01-hybrid-can-gateway
dal_level: DAL-A
s1000d_dmc: DMC-AEGIS-A-24-30-01-00A-420A-A

requirements:
  - id: SRS-GW-001
    title: 128-Byte ABI Context
    dal: DAL-A
    hazard: MIL-882E-CAT-I
    fault: DTC-0x101
    description: >
      Shared context must be 128 bytes.
    isolation_action: >
      Halt initialization on alignment error.
";

    let srs_res = SrsDocument::parse_yaml(yaml);
    assert!(srs_res.is_ok());
    let Ok(srs) = srs_res else {
        return;
    };

    let code = r"
// @req SRS-GW-001
// @fault DTC-0x101
// @hazard MIL-882E-CAT-I
int init_abi(void) { return 0; }
";
    let records = scan_source_str(code, "abi.c");
    let cov_res = verify_traceability(&srs, &records);
    assert!(cov_res.is_ok());
    let Ok(coverage) = cov_res else {
        return;
    };

    let xml = generate_s1000d_fault_xml(&srs, &coverage);
    assert!(xml.contains("<faultIsolationProcedure id=\"FIP-SRS-GW-001\" faultCode=\"DTC-0x101\">"));
    assert!(xml.contains("Verified Symbol: init_abi"));
}

#[test]
fn dynamic_dmc_orphan_detection_and_lockfile_drift_gate() {
    let yaml = r"
document_id: SRS-HIL-TEST
subsystem: 02-hil-hvbms-sim
dal_level: DAL-A
s1000d_dmc: DMC-AEGIS-A-24-30-02-00A-420A-A

requirements:
  - id: SRS-HIL-001
    title: 2D FSM
    dal: DAL-A
    hazard: MIL-882E-CAT-I
    fault: DTC-0x201
    description: >
      O(1) FSM
    isolation_action: >
      Open HV contactor
";
    let srs_res = SrsDocument::parse_yaml(yaml);
    assert!(srs_res.is_ok());
    let Ok(srs) = srs_res else {
        return;
    };

    let code = r"
// @req SRS-HIL-001
// @fault DTC-0x201
// @hazard MIL-882E-CAT-I
int step_fsm(void) { return 0; }
";
    let records = scan_source_str(code, "fsm.c");
    assert!(check_orphan_requirements(std::slice::from_ref(&srs), &records).is_ok());

    let cov_res = verify_traceability(&srs, &records);
    assert!(cov_res.is_ok());
    let Ok(cov) = cov_res else {
        return;
    };
    let xml = generate_s1000d_fault_xml(&srs, &cov);
    assert!(xml.contains("assyCode=\"02\""));

    let lock = render_trace_lock(&records);
    assert!(verify_trace_lock(&lock, &records).is_ok());

    let drifted_code = r"
// @req SRS-HIL-001
// @fault DTC-0x201
// @hazard MIL-882E-CAT-I
int step_fsm(void) { return 1; }
";
    let drifted_records = scan_source_str(drifted_code, "fsm.c");
    assert!(verify_trace_lock(&lock, &drifted_records).is_err());

    let orphan_code = r"
// @req SRS-UNKNOWN-999
// @fault DTC-0x999
// @hazard MIL-882E-CAT-I
int unknown_fn(void) { return 0; }
";
    let orphan_records = scan_source_str(orphan_code, "orphan.c");
    assert!(check_orphan_requirements(&[srs], &orphan_records).is_err());
}

#[test]
fn s1000d_brex_rule_gate_accepts_valid_and_rejects_tampered() {
    let yaml = r"
document_id: SRS-BREX-TEST
subsystem: 04-tactical-gcs
dal_level: DAL-C
s1000d_dmc: DMC-AEGIS-A-24-30-04-00A-420A-A

requirements:
  - id: SRS-GCS-001
    title: Castagnoli CRC-32C
    dal: DAL-C
    hazard: MIL-882E-CAT-III
    fault: DTC-0x401
    description: >
      Verify CRC-32C checksum.
    isolation_action: >
      Discard corrupted packet.
";
    let srs_res = SrsDocument::parse_yaml(yaml);
    assert!(srs_res.is_ok());
    let Ok(srs) = srs_res else {
        return;
    };
    let code = r"
// @req SRS-GCS-001
// @fault DTC-0x401
// @hazard MIL-882E-CAT-III
unsigned int calc_crc(void) { return 0xE3069283U; }
";
    let records = scan_source_str(code, "crc.c");
    let cov_res = verify_traceability(&srs, &records);
    assert!(cov_res.is_ok());
    let Ok(cov) = cov_res else {
        return;
    };
    let xml = generate_s1000d_fault_xml(&srs, &cov);
    assert!(verify_s1000d_brex(&xml, &srs).is_ok());

    let bad_sec = xml.replace(
        "securityClassification=\"01\"",
        "securityClassification=\"09\"",
    );
    assert!(verify_s1000d_brex(&bad_sec, &srs).is_err());

    let bad_brex = xml.replace("infoCode=\"022\"", "infoCode=\"999\"");
    assert!(verify_s1000d_brex(&bad_brex, &srs).is_err());
}

#[test]
fn scanner_ignores_at_req_inside_non_comment_code() {
    let code = r#"
fn log_status() {
    println!("Zero orphan @req tags verified");
}
"#;
    let records = scan_source_str(code, "main.rs");
    assert!(records.is_empty());
}
