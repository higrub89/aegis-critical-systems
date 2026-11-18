//! Unit and RFC 4231 test suite for `aegis-parser`.

use crate::hmac::{constant_time_eq_32, hmac_sha256, Sha256};
use crate::parser::{
    compute_frame_hmac, parse_and_authenticate_frame, AegisFrameCtx, CanFdFrame,
    AEGIS_DTC_AUTH_FAILURE, AEGIS_DTC_NONE, AEGIS_DTC_PAYLOAD_BOUNDS, AEGIS_STATUS_AUTH_OK,
    AEGIS_STATUS_PARSED_OK,
};

fn make_ctx() -> AegisFrameCtx {
    AegisFrameCtx {
        raw_frame: CanFdFrame {
            can_id: 0x120,
            len: 12,
            flags: 0x01,
            res0: 0,
            res1: 0,
            data: [0u8; 64],
        },
        rx_timestamp_ns: 1_000_000,
        auth_timestamp_ns: 0,
        seq_counter: 42,
        status_flags: 0,
        dtc_code: 0,
        hmac_tag: [0u8; 32],
    }
}

#[test]
fn rfc_4231_hmac_sha256_test_case_1() {
    let key = [0x0bu8; 20];
    let msg = b"Hi There";
    let expected: [u8; 32] = [
        0xb0, 0x34, 0x4c, 0x61, 0xd8, 0xdb, 0x38, 0x53, 0x5c, 0xa8, 0xaf, 0xce, 0xaf, 0x0b, 0xf1,
        0x2b, 0x88, 0x1d, 0xc2, 0x00, 0xc9, 0x83, 0x3d, 0xa7, 0x26, 0xe9, 0x37, 0x6c, 0x2e, 0x32,
        0xcf, 0xf7,
    ];

    let mut sha = Sha256::new();
    sha.update(b"abc");
    let abc = sha.finalize();
    assert_eq!(abc[0], 0xba);
    assert_eq!(abc[31], 0xad);

    let actual = hmac_sha256(&key, msg);
    assert!(constant_time_eq_32(&actual, &expected));
}

#[test]
fn parser_accepts_authenticated_telemetry_and_rejects_tampering() {
    let key = b"AEGIS_HV_BMS_SHARED_SECRET_2026";
    let mut ctx = make_ctx();

    // Pack: 400_000 mV (0x0006_1A80), -12_500 mA (0xFFFF_CF2C), 32.5 C (325), contactor=2, fault=0
    ctx.raw_frame.data[..4].copy_from_slice(&400_000u32.to_le_bytes());
    ctx.raw_frame.data[4..8].copy_from_slice(&(-12_500i32).to_le_bytes());
    ctx.raw_frame.data[8..10].copy_from_slice(&325i16.to_le_bytes());
    ctx.raw_frame.data[10] = 2;
    ctx.raw_frame.data[11] = 0;

    let tag_res = compute_frame_hmac(&ctx, key);
    assert!(tag_res.is_ok());
    if let Ok(tag) = tag_res {
        ctx.hmac_tag = tag;
    }

    let parsed = parse_and_authenticate_frame(&mut ctx, key);
    assert!(parsed.is_ok());
    if let Ok(tel) = parsed {
        assert_eq!(tel.pack_voltage_mv, 400_000);
        assert_eq!(tel.pack_current_ma, -12_500);
        assert_eq!(tel.max_cell_temp_dc, 325);
        assert_eq!(tel.contactor_state, 2);
        assert_eq!(tel.fault_mask, 0);
    }
    assert_eq!(ctx.dtc_code, AEGIS_DTC_NONE);
    assert_ne!(ctx.status_flags & AEGIS_STATUS_AUTH_OK, 0);
    assert_ne!(ctx.status_flags & AEGIS_STATUS_PARSED_OK, 0);

    // Tamper 1 bit in payload -> must reject with AEGIS_DTC_AUTH_FAILURE
    ctx.raw_frame.data[0] ^= 0x01;
    let tampered = parse_and_authenticate_frame(&mut ctx, key);
    assert_eq!(tampered, Err(AEGIS_DTC_AUTH_FAILURE));
    assert_eq!(ctx.dtc_code, AEGIS_DTC_AUTH_FAILURE);
}

#[test]
fn parser_rejects_invalid_payload_length() {
    let key = b"AEGIS_KEY";
    let mut ctx = make_ctx();
    ctx.raw_frame.len = 65;

    let res = parse_and_authenticate_frame(&mut ctx, key);
    assert_eq!(res, Err(AEGIS_DTC_PAYLOAD_BOUNDS));
    assert_eq!(ctx.dtc_code, AEGIS_DTC_PAYLOAD_BOUNDS);
}
