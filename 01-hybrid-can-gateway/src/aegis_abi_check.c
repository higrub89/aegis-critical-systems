/* ========================================================================== *
 * AEGIS Critical Systems - Subsystem 01: Hybrid Safety Gateway
 * File: 01-hybrid-can-gateway/src/aegis_abi_check.c
 * Description: Deterministic initialization, flat-predicate bounds check,
 *              and constant-time HMAC comparator for C17/Rust FFI Context.
 * Standard: ISO/IEC 9899:2018 (C17) | MISRA C:2012 | DO-178C DAL-A
 * ========================================================================== */

#include "aegis_frame_abi.h"
#include <errno.h>
#include <stdint.h>
#include <string.h>

/*
 * @req SRS-GW-001
 * @fault DTC-0x101
 * @hazard MIL-882E-CAT-I
 */
int aegis_frame_ctx_init(t_aegis_frame_ctx *ctx)
{
    uintptr_t addr;

    if (ctx == NULL)
        return (EINVAL);
    addr = (uintptr_t)ctx;
    if ((addr & (AEGIS_CACHELINE_SIZE - 1U)) != 0U)
    {
        ctx->status_flags = AEGIS_STATUS_FAULT_ACTIVE;
        ctx->dtc_code = AEGIS_DTC_ABI_ALIGNMENT;
        return (EFAULT);
    }
    (void)memset(ctx, 0, sizeof(t_aegis_frame_ctx));
    ctx->status_flags = AEGIS_STATUS_INIT;
    ctx->dtc_code = AEGIS_DTC_NONE;
    return (0);
}

/*
 * @req SRS-GW-002
 * @fault DTC-0x102
 * @hazard MIL-882E-CAT-I
 */
int aegis_frame_validate_bounds(t_aegis_frame_ctx *ctx)
{
    if (ctx == NULL)
        return (EINVAL);
    if (ctx->raw_frame.len > AEGIS_CANFD_MAX_DLEN)
    {
        ctx->status_flags |= AEGIS_STATUS_FAULT_ACTIVE;
        ctx->dtc_code = AEGIS_DTC_PAYLOAD_BOUNDS;
        return (EMSGSIZE);
    }
    ctx->status_flags |= AEGIS_STATUS_VALID_LEN;
    return (0);
}

/*
 * @req SRS-GW-003
 * @fault DTC-0x104
 * @hazard MIL-882E-CAT-I
 */
int aegis_frame_verify_hmac_consttime(
    t_aegis_frame_ctx *ctx,
    const uint8_t expected_tag[AEGIS_HMAC_TAG_LEN])
{
    uint8_t diff;
    size_t  i;

    if (ctx == NULL)
        return (EINVAL);
    if (expected_tag == NULL)
        return (EINVAL);
    diff = 0U;
    for (i = 0U; i < AEGIS_HMAC_TAG_LEN; ++i)
    {
        diff |= (uint8_t)(ctx->hmac_tag[i] ^ expected_tag[i]);
    }
    if (diff != 0U)
    {
        ctx->status_flags |= AEGIS_STATUS_FAULT_ACTIVE;
        ctx->dtc_code = AEGIS_DTC_AUTH_FAILURE;
        return (EACCES);
    }
    ctx->status_flags |= AEGIS_STATUS_AUTH_OK;
    return (0);
}
