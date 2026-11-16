//! Integration and unit verification suite for `aegis-trace`.

use aegis_trace::s1000d::{generate_s1000d_fault_xml, verify_traceability};
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
