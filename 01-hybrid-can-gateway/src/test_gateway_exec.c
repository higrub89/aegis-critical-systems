/* ========================================================================== *
 * AEGIS Critical Systems - Subsystem 01: Hybrid Safety Gateway
 * File: 01-hybrid-can-gateway/src/test_gateway_exec.c
 * Description: Deterministic Verification for Cyclic Executive, 48B CAN-FD Wire
 *              Frames, Socket Ingestion, and 3-Strike Safe-State Latch.
 * Standard: ISO/IEC 9899:2018 (C17)
 * ========================================================================== */

#include "aegis_gateway.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

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

static int build_wire_frame(struct canfd_frame *out,
                            const uint8_t      *key,
                            size_t              key_len,
                            uint32_t            seq,
                            uint32_t            voltage_mv)
{
    _Alignas(AEGIS_CACHELINE_SIZE) t_aegis_frame_ctx tmp_ctx;

    if (aegis_frame_ctx_init(&tmp_ctx) != 0)
        return (-1);
    tmp_ctx.raw_frame.can_id = 0x120U;
    tmp_ctx.raw_frame.len = AEGIS_TELEMETRY_LEN;
    tmp_ctx.seq_counter = seq;
    encode_u32_le(&tmp_ctx.raw_frame.data[0], voltage_mv);
    encode_u32_le(&tmp_ctx.raw_frame.data[4], (uint32_t)(-5000));
    encode_i16_le(&tmp_ctx.raw_frame.data[8], 295);
    tmp_ctx.raw_frame.data[10] = 2U;
    tmp_ctx.raw_frame.data[11] = 0U;

    if (aegis_rust_compute_hmac(&tmp_ctx, key, key_len) != 0)
        return (-1);

    (void)memset(out, 0, sizeof(*out));
    out->can_id = tmp_ctx.raw_frame.can_id;
    out->len = AEGIS_WIRE_FRAME_LEN;
    (void)memcpy(&out->data[0], &tmp_ctx.raw_frame.data[0], AEGIS_TELEMETRY_LEN);
    encode_u32_le(&out->data[12], seq);
    (void)memcpy(&out->data[16], tmp_ctx.hmac_tag, AEGIS_HMAC_TAG_LEN);
    return (0);
}

static int test_socket_loop_and_safe_state(void)
{
    _Alignas(AEGIS_CACHELINE_SIZE) static t_aegis_gw_state state;
    const uint8_t      key[] = "AEGIS_HV_BMS_SHARED_SECRET_2026";
    t_aegis_gw_config  cfg;
    struct canfd_frame frame;
    int                sv[2];
    int                flags;
    int                stop_flag;
    int32_t            rt_active;

    (void)memset(&cfg, 0, sizeof(cfg));
    cfg.period_ns = 100000ULL; /* 100 us fast deterministic test period */
    cfg.cpu_core = 0;
    cfg.rt_priority = 0;
    cfg.hmac_key_len = sizeof(key) - 1U;
    (void)memcpy(cfg.hmac_key, key, cfg.hmac_key_len);

    if (aegis_gw_init(&state, &cfg) != 0)
        return (1);

    rt_active = 0;
    if (aegis_rt_lock_and_pin(0, 0, &rt_active) != 0)
        return (2);

    if (socketpair(AF_UNIX, SOCK_DGRAM, 0, sv) != 0)
        return (3);
    flags = fcntl(sv[0], F_GETFL, 0);
    if (flags < 0 || fcntl(sv[0], F_SETFL, flags | O_NONBLOCK) < 0)
    {
        (void)close(sv[0]);
        (void)close(sv[1]);
        return (4);
    }

    /* Send 2 valid frames over datagram socket */
    if (build_wire_frame(&frame, key, cfg.hmac_key_len, 1U, 405000U) != 0)
        return (5);
    if (write(sv[1], &frame, sizeof(frame)) != (ssize_t)sizeof(frame))
        return (6);

    if (build_wire_frame(&frame, key, cfg.hmac_key_len, 2U, 406200U) != 0)
        return (7);
    if (write(sv[1], &frame, sizeof(frame)) != (ssize_t)sizeof(frame))
        return (8);

    stop_flag = 0;
    if (aegis_gw_run_loop(&state, sv[0], 3U, &stop_flag) != 0)
        return (9);

    if (state.mode != AEGIS_GW_OPERATIONAL || state.frames_ok != 2ULL
        || state.last_telemetry.pack_voltage_mv != 406200U)
    {
        (void)close(sv[0]);
        (void)close(sv[1]);
        return (10);
    }

    /* Inject 3 consecutive tampered frames -> must latch AEGIS_GW_SAFE_STATE */
    frame.data[0] ^= 0x01U;
    for (int i = 0; i < 3; ++i)
    {
        if (write(sv[1], &frame, sizeof(frame)) != (ssize_t)sizeof(frame))
            return (11);
    }
    if (aegis_gw_run_loop(&state, sv[0], 3U, &stop_flag) != 0)
        return (12);

    (void)close(sv[0]);
    (void)close(sv[1]);

    if (state.mode != AEGIS_GW_SAFE_STATE || state.frames_rejected != 3ULL
        || state.last_dtc != AEGIS_DTC_AUTH_FAILURE)
    {
        return (13);
    }

    return (0);
}

static int test_antireplay_protection(void)
{
    _Alignas(AEGIS_CACHELINE_SIZE) static t_aegis_gw_state state;
    const uint8_t      key[] = "AEGIS_HV_BMS_SHARED_SECRET_2026";
    t_aegis_gw_config  cfg;
    struct canfd_frame frame;

    (void)memset(&cfg, 0, sizeof(cfg));
    cfg.period_ns = 100000ULL;
    cfg.hmac_key_len = sizeof(key) - 1U;
    (void)memcpy(cfg.hmac_key, key, cfg.hmac_key_len);

    if (aegis_gw_init(&state, &cfg) != 0)
        return (1);

    if (build_wire_frame(&frame, key, cfg.hmac_key_len, 10U, 401000U) != 0)
        return (2);
    if (aegis_gw_step_frame(&state, &frame, 1000ULL) != 0)
        return (3);

    if (build_wire_frame(&frame, key, cfg.hmac_key_len, 11U, 402000U) != 0)
        return (4);
    if (aegis_gw_step_frame(&state, &frame, 2000ULL) != 0)
        return (5);

    /* Replay seq=11 three times -> must reject with EALREADY + DTC-0x106 and latch SAFE_STATE */
    for (uint64_t i = 0ULL; i < 3ULL; ++i)
    {
        if (aegis_gw_step_frame(&state, &frame, 3000ULL + i) != EALREADY)
            return (6);
    }
    if (state.mode != AEGIS_GW_SAFE_STATE || state.last_dtc != AEGIS_DTC_REPLAY_ATTACK)
        return (7);
    return (0);
}

int main(void)
{
    int rc;

    rc = test_socket_loop_and_safe_state();
    if (rc != 0)
    {
        (void)fprintf(stderr, "[FAIL] test_socket_loop_and_safe_state: %d\n", rc);
        return (1);
    }
    rc = test_antireplay_protection();
    if (rc != 0)
    {
        (void)fprintf(stderr, "[FAIL] test_antireplay_protection: %d\n", rc);
        return (1);
    }
    (void)printf("[PASS] C17 Cyclic Executive, Anti-Replay (DO-326A) & Safe-State Latch verified.\n");
    return (0);
}
