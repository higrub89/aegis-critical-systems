/* ========================================================================== *
 * AEGIS Critical Systems - Shared Memory Telemetry & Command Bus Contract
 * File: 01-hybrid-can-gateway/include/aegis_shm_bus.h
 * Description: POSIX Shared Memory Snapshot Contract bridging Gateway (01)
 *              and Tactical TM/TC Daemon (04).
 * Standard: ISO/IEC 9899:2018 (C17) | ISO C++23 | POSIX.1-2017
 * ========================================================================== */

#ifndef AEGIS_SHM_BUS_H
#define AEGIS_SHM_BUS_H

#include "aegis_frame_abi.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define AEGIS_DEFAULT_SHM_NAME "/aegis_tmtc_shm"

typedef struct s_aegis_tmtc_shm
{
    AEGIS_ALIGNAS(AEGIS_CACHELINE_SIZE) uint32_t seq_lock;
    uint16_t               gw_mode;
    uint16_t               last_dtc;
    uint32_t               tc_safe_state_req;
    uint32_t               reserved;
    uint64_t               timestamp_ns;
    t_aegis_telemetry_data telemetry;
} t_aegis_tmtc_shm;

AEGIS_STATIC_ASSERT(sizeof(t_aegis_tmtc_shm) == 64U,
                    "POSIX SHM snapshot must fit cleanly in one 64B cache line");

#ifdef __cplusplus
}
#endif

#endif /* AEGIS_SHM_BUS_H */
