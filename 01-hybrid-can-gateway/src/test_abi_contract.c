/* ========================================================================== *
 * AEGIS Critical Systems - Subsystem 01: Hybrid Safety Gateway
 * File: 01-hybrid-can-gateway/src/test_abi_contract.c
 * Description: Deterministic verification harness for 128B FFI ABI contract
 * Standard: ISO/IEC 9899:2018 (C17)
 * ========================================================================== */

#include "aegis_frame_abi.h"
#include <errno.h>
#include <stdio.h>
#include <string.h>

static int test_init_and_size(void)
{
    _Alignas(AEGIS_CACHELINE_SIZE) t_aegis_frame_ctx ctx;
    int rc;

    if (sizeof(t_aegis_frame_ctx) != 128U)
        return (1);
    rc = aegis_frame_ctx_init(&ctx);
    if (rc != 0)
        return (2);
    if (ctx.status_flags != AEGIS_STATUS_INIT || ctx.dtc_code != AEGIS_DTC_NONE)
        return (3);
    if (aegis_frame_ctx_init(NULL) != EINVAL)
        return (4);
    return (0);
}

static int test_bounds_validation(void)
{
    _Alignas(AEGIS_CACHELINE_SIZE) t_aegis_frame_ctx ctx;

    (void)aegis_frame_ctx_init(&ctx);
    ctx.raw_frame.len = 64U;
    if (aegis_frame_validate_bounds(&ctx) != 0)
        return (1);
    if ((ctx.status_flags & AEGIS_STATUS_VALID_LEN) == 0U)
        return (2);

    ctx.raw_frame.len = 65U;
    if (aegis_frame_validate_bounds(&ctx) != EMSGSIZE)
        return (3);
    if (ctx.dtc_code != AEGIS_DTC_PAYLOAD_BOUNDS)
        return (4);
    return (0);
}

static int test_hmac_verification(void)
{
    _Alignas(AEGIS_CACHELINE_SIZE) t_aegis_frame_ctx ctx;
    uint8_t expected[AEGIS_HMAC_TAG_LEN];
    size_t  i;

    (void)aegis_frame_ctx_init(&ctx);
    for (i = 0U; i < AEGIS_HMAC_TAG_LEN; ++i)
    {
        ctx.hmac_tag[i] = (uint8_t)(i + 1U);
        expected[i] = (uint8_t)(i + 1U);
    }
    if (aegis_frame_verify_hmac_consttime(&ctx, expected) != 0)
        return (1);
    if ((ctx.status_flags & AEGIS_STATUS_AUTH_OK) == 0U)
        return (2);

    expected[31] ^= 0xFFU;
    if (aegis_frame_verify_hmac_consttime(&ctx, expected) != EACCES)
        return (3);
    if (ctx.dtc_code != AEGIS_DTC_AUTH_FAILURE)
        return (4);
    return (0);
}

int main(void)
{
    int rc;

    rc = test_init_and_size();
    if (rc != 0)
    {
        (void)fprintf(stderr, "[FAIL] test_init_and_size: %d\n", rc);
        return (1);
    }
    rc = test_bounds_validation();
    if (rc != 0)
    {
        (void)fprintf(stderr, "[FAIL] test_bounds_validation: %d\n", rc);
        return (1);
    }
    rc = test_hmac_verification();
    if (rc != 0)
    {
        (void)fprintf(stderr, "[FAIL] test_hmac_verification: %d\n", rc);
        return (1);
    }
    (void)printf("[PASS] AEGIS 128-Byte ABI Contract & Safety Predicates verified.\n");
    return (0);
}
