//! Safe `no_std` CAN-FD Frame Parser and RTCA DO-326A Authentication Validator.
//! All code in this module is `#![forbid(unsafe_code)]` with zero heap allocation.

use crate::hmac::{constant_time_eq_32, hmac_sha256};

/// Status flag: context initialized.
pub const AEGIS_STATUS_INIT: u16 = 0x0000;
/// Status flag: CAN-FD payload length verified (`<= 64` bytes).
pub const AEGIS_STATUS_VALID_LEN: u16 = 0x0001;
/// Status flag: HMAC-SHA256 tag verified.
pub const AEGIS_STATUS_AUTH_OK: u16 = 0x0002;
/// Status flag: decoded telemetry payload valid.
pub const AEGIS_STATUS_PARSED_OK: u16 = 0x0004;
/// Status flag: active fault detected.
pub const AEGIS_STATUS_FAULT_ACTIVE: u16 = 0x8000;

/// DTC: No fault.
pub const AEGIS_DTC_NONE: u16 = 0x0000;
/// DTC: ABI alignment violation.
pub const AEGIS_DTC_ABI_ALIGNMENT: u16 = 0x0101;
/// DTC: CAN-FD payload bounds violation (`len > 64` or `< 12` telemetry bytes).
pub const AEGIS_DTC_PAYLOAD_BOUNDS: u16 = 0x0102;
/// DTC: Constant-time HMAC-SHA256 tag mismatch.
pub const AEGIS_DTC_AUTH_FAILURE: u16 = 0x0104;

/// C-ABI `struct canfd_frame` from `<linux/can.h>` (72 bytes).
#[repr(C)]
#[derive(Debug, Clone, Copy)]
pub struct CanFdFrame {
    /// CAN identifier + flags (`EFF`/`RTR`/`ERR`).
    pub can_id: u32,
    /// Payload length (`0..=64`).
    pub len: u8,
    /// CAN-FD flags (`CANFD_BRS`, `CANFD_ESI`).
    pub flags: u8,
    /// Reserved byte 0.
    pub res0: u8,
    /// Reserved byte 1.
    pub res1: u8,
    /// CAN-FD data payload (up to 64 bytes).
    pub data: [u8; 64],
}

/// 128-Byte Cache-Aligned C17 / Rust `no_std` FFI Context (`t_aegis_frame_ctx`).
#[repr(C, align(64))]
#[derive(Debug, Clone, Copy)]
pub struct AegisFrameCtx {
    /// Raw `SocketCAN` CAN-FD frame (72 bytes).
    pub raw_frame: CanFdFrame,
    /// Monotonic RX timestamp in nanoseconds.
    pub rx_timestamp_ns: u64,
    /// Monotonic authentication completion timestamp in nanoseconds.
    pub auth_timestamp_ns: u64,
    /// Monotonically increasing frame sequence counter.
    pub seq_counter: u32,
    /// Bitmask status flags (`AEGIS_STATUS_*`).
    pub status_flags: u16,
    /// Active Diagnostic Trouble Code (`AEGIS_DTC_*`).
    pub dtc_code: u16,
    /// 256-bit HMAC authentication tag.
    pub hmac_tag: [u8; 32],
}

/// Decoded propulsion & HV-BMS telemetry extracted deterministically from CAN-FD payload.
#[repr(C)]
#[derive(Debug, Clone, Copy, PartialEq, Eq, Default)]
pub struct AegisTelemetryData {
    /// HV pack voltage in millivolts.
    pub pack_voltage_mv: u32,
    /// HV pack current in milliamperes (signed: positive = discharge).
    pub pack_current_ma: i32,
    /// Maximum cell temperature in deci-Celsius (`250` = `25.0 C`).
    pub max_cell_temp_dc: i16,
    /// Contactor state (`0 = OPEN`, `1 = PRECHARGE`, `2 = CLOSED`).
    pub contactor_state: u8,
    /// Subsystem fault bitmask.
    pub fault_mask: u8,
}

const _: () = assert!(core::mem::size_of::<CanFdFrame>() == 72);
const _: () = assert!(core::mem::size_of::<AegisFrameCtx>() == 128);
const _: () = assert!(core::mem::align_of::<AegisFrameCtx>() == 64);
const _: () = assert!(core::mem::offset_of!(AegisFrameCtx, rx_timestamp_ns) == 72);
const _: () = assert!(core::mem::offset_of!(AegisFrameCtx, hmac_tag) == 96);
const _: () = assert!(core::mem::size_of::<AegisTelemetryData>() == 12);

/// Computes the canonical HMAC-SHA256 authentication tag over `(can_id, len, seq_counter, payload[..len])`.
///
/// # Errors
/// Returns `AEGIS_DTC_PAYLOAD_BOUNDS` if `ctx.raw_frame.len > 64`.
// @req SRS-GW-003
// @fault DTC-0x104
// @hazard MIL-882E-CAT-I
pub fn compute_frame_hmac(ctx: &AegisFrameCtx, key: &[u8]) -> Result<[u8; 32], u16> {
    let payload_len = usize::from(ctx.raw_frame.len);
    let Some(payload_slice) = ctx.raw_frame.data.get(..payload_len) else {
        return Err(AEGIS_DTC_PAYLOAD_BOUNDS);
    };

    let mut msg_buf = [0u8; 73];
    let id_bytes = ctx.raw_frame.can_id.to_le_bytes();
    let seq_bytes = ctx.seq_counter.to_le_bytes();

    msg_buf[0] = id_bytes[0];
    msg_buf[1] = id_bytes[1];
    msg_buf[2] = id_bytes[2];
    msg_buf[3] = id_bytes[3];
    msg_buf[4] = ctx.raw_frame.len;
    msg_buf[5] = seq_bytes[0];
    msg_buf[6] = seq_bytes[1];
    msg_buf[7] = seq_bytes[2];
    msg_buf[8] = seq_bytes[3];

    for (dst, &src) in msg_buf.iter_mut().skip(9).zip(payload_slice.iter()) {
        *dst = src;
    }

    let total_len = 9usize.wrapping_add(payload_len);
    let msg = msg_buf.get(..total_len).unwrap_or(&[]);
    Ok(hmac_sha256(key, msg))
}

/// Validates CAN-FD bounds, checks constant-time HMAC-SHA256, and decodes propulsion telemetry.
///
/// # Errors
/// Returns a `DTC-0x1xx` code on bounds or authentication failure and updates `ctx.status_flags` / `ctx.dtc_code`.
// @req SRS-GW-004
// @fault DTC-0x104
// @hazard MIL-882E-CAT-I
pub fn parse_and_authenticate_frame(
    ctx: &mut AegisFrameCtx,
    key: &[u8],
) -> Result<AegisTelemetryData, u16> {
    let payload_len = usize::from(ctx.raw_frame.len);
    if !(12..=64).contains(&payload_len) {
        ctx.status_flags |= AEGIS_STATUS_FAULT_ACTIVE;
        ctx.dtc_code = AEGIS_DTC_PAYLOAD_BOUNDS;
        return Err(AEGIS_DTC_PAYLOAD_BOUNDS);
    }
    ctx.status_flags |= AEGIS_STATUS_VALID_LEN;

    let expected_tag = match compute_frame_hmac(ctx, key) {
        Ok(tag) => tag,
        Err(dtc) => {
            ctx.status_flags |= AEGIS_STATUS_FAULT_ACTIVE;
            ctx.dtc_code = dtc;
            return Err(dtc);
        }
    };

    if !constant_time_eq_32(&ctx.hmac_tag, &expected_tag) {
        ctx.status_flags |= AEGIS_STATUS_FAULT_ACTIVE;
        ctx.dtc_code = AEGIS_DTC_AUTH_FAILURE;
        return Err(AEGIS_DTC_AUTH_FAILURE);
    }
    ctx.status_flags |= AEGIS_STATUS_AUTH_OK;

    let d = &ctx.raw_frame.data;
    let voltage = u32::from_le_bytes([d[0], d[1], d[2], d[3]]);
    let current = i32::from_le_bytes([d[4], d[5], d[6], d[7]]);
    let temp_dc = i16::from_le_bytes([d[8], d[9]]);
    let contactor = d[10];
    let fault_mask = d[11];

    ctx.status_flags |= AEGIS_STATUS_PARSED_OK;
    ctx.dtc_code = AEGIS_DTC_NONE;

    Ok(AegisTelemetryData {
        pack_voltage_mv: voltage,
        pack_current_ma: current,
        max_cell_temp_dc: temp_dc,
        contactor_state: contactor,
        fault_mask,
    })
}
