//! AEGIS Critical Systems - `no_std` Hybrid Safety Gateway Parser (`libaegis_parser.a`).
//! Core cryptographic and protocol parsing logic enforces `#[forbid(unsafe_code)]`.

#![cfg_attr(not(any(test, debug_assertions)), no_std)]

#[forbid(unsafe_code)]
pub mod hmac;

#[forbid(unsafe_code)]
pub mod parser;

#[cfg(test)]
mod tests;

use core::slice;
use parser::{
    compute_frame_hmac, parse_and_authenticate_frame, AegisFrameCtx, AegisTelemetryData,
    AEGIS_DTC_ABI_ALIGNMENT, AEGIS_STATUS_FAULT_ACTIVE,
};

#[cfg(not(any(test, debug_assertions)))]
#[panic_handler]
fn aegis_panic_handler(_info: &core::panic::PanicInfo<'_>) -> ! {
    loop {
        core::hint::spin_loop();
    }
}

/// Validates pointer non-nullness and 64-byte cache-line alignment, then processes the CAN-FD
/// frame through the `#[forbid(unsafe_code)]` parser and constant-time HMAC gate.
///
/// Returns `0` on success, or negative `-EINVAL` (`-22`) / `-EFAULT` (`-14`) / `-EMSGSIZE` (`-90`) /
/// `-EACCES` (`-13`).
///
/// # Safety
/// - `ctx_ptr` must be valid, non-null, 64-byte aligned, and exclusively accessible for the duration of the call.
/// - `key_ptr` must point to at least `key_len` readable bytes.
/// - `out_telemetry` must be valid, non-null, and properly aligned for `AegisTelemetryData`.
#[allow(unsafe_code)]
#[no_mangle]
pub unsafe extern "C" fn aegis_rust_process_frame(
    ctx_ptr: *mut AegisFrameCtx,
    key_ptr: *const u8,
    key_len: usize,
    out_telemetry: *mut AegisTelemetryData,
) -> i32 {
    if ctx_ptr.is_null() || key_ptr.is_null() || out_telemetry.is_null() || key_len == 0 {
        return -22; // -EINVAL
    }

    if !(ctx_ptr as usize).is_multiple_of(64) {
        // SAFETY: `ctx_ptr` was checked non-null above; alignment failed so we only set fault metadata if aligned to u64.
        if (ctx_ptr as usize).is_multiple_of(core::mem::align_of::<u64>()) {
            let unaligned_ctx = unsafe { &mut *ctx_ptr };
            unaligned_ctx.status_flags |= AEGIS_STATUS_FAULT_ACTIVE;
            unaligned_ctx.dtc_code = AEGIS_DTC_ABI_ALIGNMENT;
        }
        return -14; // -EFAULT
    }

    if !(out_telemetry as usize).is_multiple_of(core::mem::align_of::<AegisTelemetryData>()) {
        return -14; // -EFAULT
    }

    // SAFETY: `ctx_ptr` is non-null, 64-byte aligned, and points to a valid `t_aegis_frame_ctx`
    // for the duration of this synchronous FFI call.
    let ctx: &mut AegisFrameCtx = unsafe { &mut *ctx_ptr };

    // SAFETY: `key_ptr` is non-null and `key_len > 0` points to caller-owned immutable key bytes.
    let key: &[u8] = unsafe { slice::from_raw_parts(key_ptr, key_len) };

    match parse_and_authenticate_frame(ctx, key) {
        Ok(telemetry) => {
            // SAFETY: `out_telemetry` is non-null and properly aligned for `AegisTelemetryData`.
            unsafe {
                *out_telemetry = telemetry;
            }
            0
        }
        Err(parser::AEGIS_DTC_PAYLOAD_BOUNDS) => -90, // -EMSGSIZE
        Err(_) => -13,                                // -EACCES
    }
}

/// Computes the canonical 32-byte HMAC-SHA256 tag for `ctx_ptr` and writes it into `ctx_ptr->hmac_tag`.
///
/// # Safety
/// - `ctx_ptr` must be valid, non-null, 64-byte aligned, and exclusively accessible for the duration of the call.
/// - `key_ptr` must point to at least `key_len` readable bytes.
#[allow(unsafe_code)]
#[no_mangle]
pub unsafe extern "C" fn aegis_rust_compute_hmac(
    ctx_ptr: *mut AegisFrameCtx,
    key_ptr: *const u8,
    key_len: usize,
) -> i32 {
    if ctx_ptr.is_null() || key_ptr.is_null() || key_len == 0 {
        return -22; // -EINVAL
    }
    if !(ctx_ptr as usize).is_multiple_of(64) {
        return -14; // -EFAULT
    }

    // SAFETY: `ctx_ptr` is non-null, 64-byte aligned, and exclusively borrowed for this FFI call.
    let ctx: &mut AegisFrameCtx = unsafe { &mut *ctx_ptr };

    // SAFETY: `key_ptr` is non-null and `key_len > 0` points to valid immutable key bytes.
    let key: &[u8] = unsafe { slice::from_raw_parts(key_ptr, key_len) };

    match compute_frame_hmac(ctx, key) {
        Ok(tag) => {
            ctx.hmac_tag = tag;
            0
        }
        Err(_) => -90, // -EMSGSIZE
    }
}
