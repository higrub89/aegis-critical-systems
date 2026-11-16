/* ========================================================================== *
 * AEGIS Critical Systems - Subsystem 04: Tactical Ground Control Station
 * File: 04-tactical-gcs/tmtc-daemon/src/aegis_crc32c.c
 * Description: Hardware SSE4.2 Castagnoli CRC-32C with Deterministic Fallback
 * Standard: ISO/IEC 9899:2018 (C17) | MISRA C:2012
 * ========================================================================== */

#include "aegis_tmtc.h"
#include <stddef.h>
#include <stdint.h>

#if defined(__x86_64__) && (defined(__GNUC__) || defined(__clang__))
__attribute__((target("sse4.2"))) static uint32_t crc32c_hw_sse42(const uint8_t *buf,
                                                                    size_t         len)
{
    uint32_t crc;
    size_t   i;

    crc = 0xFFFFFFFFU;
    i = 0U;
    while ((i + 8U) <= len)
    {
        uint64_t val64 = ((uint64_t)buf[i]) | ((uint64_t)buf[i + 1U] << 8U)
                         | ((uint64_t)buf[i + 2U] << 16U)
                         | ((uint64_t)buf[i + 3U] << 24U)
                         | ((uint64_t)buf[i + 4U] << 32U)
                         | ((uint64_t)buf[i + 5U] << 40U)
                         | ((uint64_t)buf[i + 6U] << 48U)
                         | ((uint64_t)buf[i + 7U] << 56U);
        crc = (uint32_t)__builtin_ia32_crc32di((uint64_t)crc, val64);
        i += 8U;
    }
    while (i < len)
    {
        crc = __builtin_ia32_crc32qi(crc, buf[i]);
        i += 1U;
    }
    return (~crc);
}
#endif

static uint32_t crc32c_sw(const uint8_t *buf, size_t len)
{
    uint32_t crc;
    size_t   i;
    uint32_t bit;

    crc = 0xFFFFFFFFU;
    for (i = 0U; i < len; ++i)
    {
        crc ^= (uint32_t)buf[i];
        for (bit = 0U; bit < 8U; ++bit)
        {
            if ((crc & 1U) != 0U)
                crc = (crc >> 1U) ^ 0x82F63B78U;
            else
                crc >>= 1U;
        }
    }
    return (~crc);
}

/*
 * @req SRS-GCS-001
 * @fault DTC-0x401
 * @hazard MIL-882E-CAT-III
 */
uint32_t aegis_crc32c(const void *data, size_t len)
{
    const uint8_t *bytes;

    if (data == NULL || len == 0U)
        return (0U);
    bytes = (const uint8_t *)data;
#if defined(__x86_64__) && (defined(__GNUC__) || defined(__clang__))
    if (__builtin_cpu_supports("sse4.2"))
        return (crc32c_hw_sse42(bytes, len));
#endif
    return (crc32c_sw(bytes, len));
}
