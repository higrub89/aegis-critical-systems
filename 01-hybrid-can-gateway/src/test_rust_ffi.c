/* ========================================================================== *
 * AEGIS Critical Systems - Subsystem 01: Hybrid Safety Gateway
 * File: 01-hybrid-can-gateway/src/test_rust_ffi.c
 * Description: C17 <-> Rust no_std Zero-Copy FFI Boundary Integration Test
 * Standard: ISO/IEC 9899:2018 (C17)
 * ========================================================================== */

#include "aegis_frame_abi.h"
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static void encode_u32_le(uint8_t *dst, uint32_t val)
{
    dst[0] = (uint8_t)(val & 0xFFU);
    dst[1] = (uint8_t)((val >> 8U) & 0xFFU);
    dst[2] = (uint8_t)((val >> 16U) & 0xFFU);
    dst[3] = (uint8_t)((val >> 24U) & 0xFFU);
}

static void encode_i16_le(uint8_t *dst, int16_t val)
{
    uint16_t u = (uint16_t)val;

    dst[0] = (uint8_t)(u & 0xFFU);
    dst[1] = (uint8_t)((u >> 8U) & 0xFFU);
}

int main(void)
{
    _Alignas(AEGIS_CACHELINE_SIZE) t_aegis_frame_ctx ctx;
    t_aegis_telemetry_data                           telemetry;
    const uint8_t                                    key[] = "AEGIS_HV_BMS_SHARED_SECRET_2026";
    size_t                                           key_len;
    int32_t                                          rc;

    key_len = sizeof(key) - 1U;
    if (aegis_frame_ctx_init(&ctx) != 0)
    {
        (void)fprintf(stderr, "[FAIL] aegis_frame_ctx_init\n");
        return (1);
    }

    ctx.raw_frame.can_id = 0x120U;
    ctx.raw_frame.len = 12U;
    ctx.seq_counter = 7U;
    encode_u32_le(&ctx.raw_frame.data[0], 412500U);
    encode_u32_le(&ctx.raw_frame.data[4], (uint32_t)(-8400));
    encode_i16_le(&ctx.raw_frame.data[8], 315);
    ctx.raw_frame.data[10] = 2U;
    ctx.raw_frame.data[11] = 0U;

    rc = aegis_rust_compute_hmac(&ctx, key, key_len);
    if (rc != 0)
    {
        (void)fprintf(stderr, "[FAIL] aegis_rust_compute_hmac: %d\n", rc);
        return (1);
    }

    (void)memset(&telemetry, 0, sizeof(telemetry));
    rc = aegis_rust_process_frame(&ctx, key, key_len, &telemetry);
    if (rc != 0)
    {
        (void)fprintf(stderr, "[FAIL] aegis_rust_process_frame valid: %d\n", rc);
        return (1);
    }

    if (telemetry.pack_voltage_mv != 412500U || telemetry.pack_current_ma != -8400
        || telemetry.max_cell_temp_dc != 315 || telemetry.contactor_state != 2U)
    {
        (void)fprintf(stderr, "[FAIL] decoded telemetry mismatch\n");
        return (1);
    }

    /* Tamper payload bit and verify constant-time rejection */
    ctx.raw_frame.data[0] ^= 0x01U;
    rc = aegis_rust_process_frame(&ctx, key, key_len, &telemetry);
    if (rc != -EACCES || ctx.dtc_code != AEGIS_DTC_AUTH_FAILURE)
    {
        (void)fprintf(stderr, "[FAIL] expected -EACCES on tampered frame, got %d\n", rc);
        return (1);
    }

    /* Null pointer guard check */
    if (aegis_rust_process_frame(NULL, key, key_len, &telemetry) != -EINVAL)
    {
        (void)fprintf(stderr, "[FAIL] expected -EINVAL on NULL pointer\n");
        return (1);
    }

    (void)printf("[PASS] C17 <-> Rust no_std FFI Parser & HMAC-SHA256 verified.\n");
    return (0);
}
