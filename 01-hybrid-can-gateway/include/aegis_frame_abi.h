/* ========================================================================== *
 * AEGIS Critical Systems - Subsystem 01: Hybrid Safety Gateway
 * File: 01-hybrid-can-gateway/include/aegis_frame_abi.h
 * Description: 128-Byte Cache-Aligned C17 / Rust no_std FFI ABI Contract
 * Standard: ISO/IEC 9899:2018 (C17) | MISRA C:2012 | DO-178C DAL-A
 * ========================================================================== */

#ifndef AEGIS_FRAME_ABI_H
#define AEGIS_FRAME_ABI_H

#include <linux/can.h>
#include <stddef.h>
#include <stdint.h>

#define AEGIS_ABI_CTX_SIZE        128U
#define AEGIS_CACHELINE_SIZE      64U
#define AEGIS_CANFD_MAX_DLEN      64U
#define AEGIS_HMAC_TAG_LEN        32U

/* Status Flags */
#define AEGIS_STATUS_INIT         0x0000U
#define AEGIS_STATUS_VALID_LEN    0x0001U
#define AEGIS_STATUS_AUTH_OK      0x0002U
#define AEGIS_STATUS_PARSED_OK    0x0004U
#define AEGIS_STATUS_FAULT_ACTIVE 0x8000U

/* Diagnostic Trouble Codes (DTC) mapped to S1000D fault.xsd */
#define AEGIS_DTC_NONE            0x0000U
#define AEGIS_DTC_ABI_ALIGNMENT   0x0101U
#define AEGIS_DTC_PAYLOAD_BOUNDS  0x0102U
#define AEGIS_DTC_AUTH_FAILURE    0x0104U

/*
 * @req SRS-GW-001
 * @fault DTC-0x101
 * @hazard MIL-882E-CAT-I
 *
 * Memory Layout (128 Bytes = 2 x 64B L1d Cache Lines):
 *   [0..71]   : struct canfd_frame raw_frame (72 bytes)
 *   [72..79]  : uint64_t rx_timestamp_ns     (8 bytes)
 *   [80..87]  : uint64_t auth_timestamp_ns   (8 bytes)
 *   [88..91]  : uint32_t seq_counter         (4 bytes)
 *   [92..93]  : uint16_t status_flags        (2 bytes)
 *   [94..95]  : uint16_t dtc_code            (2 bytes)
 *   [96..127] : uint8_t  hmac_tag[32]        (32 bytes)
 */
typedef struct s_aegis_frame_ctx
{
    _Alignas(AEGIS_CACHELINE_SIZE) struct canfd_frame raw_frame;
    uint64_t                                          rx_timestamp_ns;
    uint64_t                                          auth_timestamp_ns;
    uint32_t                                          seq_counter;
    uint16_t                                          status_flags;
    uint16_t                                          dtc_code;
    uint8_t                                           hmac_tag[AEGIS_HMAC_TAG_LEN];
} t_aegis_frame_ctx;

_Static_assert(sizeof(struct canfd_frame) == 72U,
               "Linux canfd_frame must be exactly 72 bytes");
_Static_assert(sizeof(t_aegis_frame_ctx) == AEGIS_ABI_CTX_SIZE,
               "ABI size mismatch: t_aegis_frame_ctx must be 128 bytes");
_Static_assert(_Alignof(t_aegis_frame_ctx) == AEGIS_CACHELINE_SIZE,
               "ABI alignment mismatch: t_aegis_frame_ctx must be 64B aligned");
_Static_assert(offsetof(t_aegis_frame_ctx, rx_timestamp_ns) == 72U,
               "ABI offset mismatch: rx_timestamp_ns must start at byte 72");
_Static_assert(offsetof(t_aegis_frame_ctx, hmac_tag) == 96U,
               "ABI offset mismatch: hmac_tag must start at byte 96");

/* Decoded propulsion & HV-BMS telemetry (12 bytes, C17 / Rust #[repr(C)]) */
typedef struct s_aegis_telemetry_data
{
    uint32_t pack_voltage_mv;
    int32_t  pack_current_ma;
    int16_t  max_cell_temp_dc;
    uint8_t  contactor_state;
    uint8_t  fault_mask;
} t_aegis_telemetry_data;

_Static_assert(sizeof(t_aegis_telemetry_data) == 12U,
               "ABI size mismatch: t_aegis_telemetry_data must be 12 bytes");

int aegis_frame_ctx_init(t_aegis_frame_ctx *ctx);
int aegis_frame_validate_bounds(t_aegis_frame_ctx *ctx);
int aegis_frame_verify_hmac_consttime(t_aegis_frame_ctx *ctx,
                                      const uint8_t expected_tag[AEGIS_HMAC_TAG_LEN]);

/* Rust no_std FFI entry points (libaegis_parser.a) */
int32_t aegis_rust_process_frame(t_aegis_frame_ctx *ctx,
                                 const uint8_t *key,
                                 size_t key_len,
                                 t_aegis_telemetry_data *out_telemetry);
int32_t aegis_rust_compute_hmac(t_aegis_frame_ctx *ctx,
                                const uint8_t *key,
                                size_t key_len);

#endif /* AEGIS_FRAME_ABI_H */
