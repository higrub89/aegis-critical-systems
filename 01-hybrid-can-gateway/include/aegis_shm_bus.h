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
#include <errno.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define AEGIS_DEFAULT_SHM_NAME "/aegis_tmtc_shm"
#define AEGIS_SHM_READ_RETRIES 64U

typedef struct s_aegis_tmtc_shm
{
    AEGIS_ALIGNAS(AEGIS_CACHELINE_SIZE) uint32_t seq_lock;
    uint16_t               gw_mode;
    uint16_t               last_dtc;
    uint32_t               tc_safe_state_req;
    uint32_t               tc_ack;
    uint64_t               timestamp_ns;
    t_aegis_telemetry_data telemetry;
} t_aegis_tmtc_shm;

AEGIS_STATIC_ASSERT(sizeof(t_aegis_tmtc_shm) == 64U,
                    "POSIX SHM snapshot must fit cleanly in one 64B cache line");

/**
 * Publishes a consistent telemetry snapshot to POSIX shared memory using an
 * even/odd seqlock protocol safe for cross-process readers and TSan.
 */
static inline void aegis_shm_publish(t_aegis_tmtc_shm             *shm,
                                     const t_aegis_telemetry_data *telemetry,
                                     uint16_t                      gw_mode,
                                     uint16_t                      last_dtc,
                                     uint64_t                      timestamp_ns)
{
    uint32_t seq;

    if (shm == NULL || telemetry == NULL)
        return;

    seq = __atomic_load_n(&shm->seq_lock, __ATOMIC_RELAXED);
    if ((seq & 1U) != 0U)
        seq += 1U;

    __atomic_store_n(&shm->seq_lock, seq + 1U, __ATOMIC_RELEASE);
    __atomic_store_n(&shm->gw_mode, gw_mode, __ATOMIC_RELAXED);
    __atomic_store_n(&shm->last_dtc, last_dtc, __ATOMIC_RELAXED);
    __atomic_store_n(&shm->timestamp_ns, timestamp_ns, __ATOMIC_RELAXED);
    __atomic_store_n(&shm->telemetry.pack_voltage_mv, telemetry->pack_voltage_mv, __ATOMIC_RELAXED);
    __atomic_store_n(&shm->telemetry.pack_current_ma, telemetry->pack_current_ma, __ATOMIC_RELAXED);
    __atomic_store_n(&shm->telemetry.max_cell_temp_dc, telemetry->max_cell_temp_dc, __ATOMIC_RELAXED);
    __atomic_store_n(&shm->telemetry.contactor_state, telemetry->contactor_state, __ATOMIC_RELAXED);
    __atomic_store_n(&shm->telemetry.fault_mask, telemetry->fault_mask, __ATOMIC_RELAXED);
    __atomic_store_n(&shm->seq_lock, seq + 2U, __ATOMIC_RELEASE);
}

/**
 * Reads a tear-free telemetry snapshot from POSIX shared memory with bounded retries.
 * Returns 0 on success, EINVAL on null args, or EAGAIN if writer kept mutating.
 */
static inline int aegis_shm_read_snapshot(const t_aegis_tmtc_shm *shm,
                                          t_aegis_telemetry_data *out_telemetry,
                                          uint16_t               *out_gw_mode,
                                          uint16_t               *out_last_dtc,
                                          uint64_t               *out_timestamp_ns)
{
    uint32_t attempt;

    if (shm == NULL || out_telemetry == NULL)
        return (EINVAL);

    for (attempt = 0U; attempt < AEGIS_SHM_READ_RETRIES; ++attempt)
    {
        uint32_t               seq_before;
        uint32_t               seq_after;
        uint16_t               mode_snap;
        uint16_t               dtc_snap;
        uint64_t               ts_snap;
        t_aegis_telemetry_data tel_snap;

        seq_before = __atomic_load_n(&shm->seq_lock, __ATOMIC_ACQUIRE);
        if ((seq_before & 1U) != 0U)
            continue;

        mode_snap = __atomic_load_n(&shm->gw_mode, __ATOMIC_RELAXED);
        dtc_snap = __atomic_load_n(&shm->last_dtc, __ATOMIC_RELAXED);
        ts_snap = __atomic_load_n(&shm->timestamp_ns, __ATOMIC_RELAXED);
        tel_snap.pack_voltage_mv = __atomic_load_n(&shm->telemetry.pack_voltage_mv, __ATOMIC_RELAXED);
        tel_snap.pack_current_ma = __atomic_load_n(&shm->telemetry.pack_current_ma, __ATOMIC_RELAXED);
        tel_snap.max_cell_temp_dc =
            __atomic_load_n(&shm->telemetry.max_cell_temp_dc, __ATOMIC_RELAXED);
        tel_snap.contactor_state = __atomic_load_n(&shm->telemetry.contactor_state, __ATOMIC_RELAXED);
        tel_snap.fault_mask = __atomic_load_n(&shm->telemetry.fault_mask, __ATOMIC_RELAXED);

#if defined(__SANITIZE_THREAD__)
        seq_after = __atomic_load_n(&shm->seq_lock, __ATOMIC_SEQ_CST);
#else
        __atomic_thread_fence(__ATOMIC_ACQUIRE);
        seq_after = __atomic_load_n(&shm->seq_lock, __ATOMIC_ACQUIRE);
#endif
        if (seq_before == seq_after)
        {
            *out_telemetry = tel_snap;
            if (out_gw_mode != NULL)
                *out_gw_mode = mode_snap;
            if (out_last_dtc != NULL)
                *out_last_dtc = dtc_snap;
            if (out_timestamp_ns != NULL)
                *out_timestamp_ns = ts_snap;
            return (0);
        }
    }

    return (EAGAIN);
}

#ifdef __cplusplus
}
#endif

#endif /* AEGIS_SHM_BUS_H */
