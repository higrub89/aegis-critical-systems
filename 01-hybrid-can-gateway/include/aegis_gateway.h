/* ========================================================================== *
 * AEGIS Critical Systems - Subsystem 01: Hybrid Safety Gateway
 * File: 01-hybrid-can-gateway/include/aegis_gateway.h
 * Description: C17 PREEMPT_RT Cyclic Executive & SocketCAN CAN-FD Node API
 * Standard: ISO/IEC 9899:2018 (C17) | POSIX.1-2008 | MISRA C:2012
 * ========================================================================== */

#ifndef AEGIS_GATEWAY_H
#define AEGIS_GATEWAY_H

#include "aegis_frame_abi.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define AEGIS_WIRE_FRAME_LEN       48U
#define AEGIS_TELEMETRY_LEN        12U
#define AEGIS_DEFAULT_PERIOD_NS    1000000ULL /* 1 kHz cyclic executive */
#define AEGIS_MAX_CONSEC_AUTH_FAIL 3U
#define AEGIS_KEY_MAX_LEN          64U
#define AEGIS_DTC_RT_LOCK_FAIL     0x0105U

typedef enum e_aegis_gw_mode
{
    AEGIS_GW_INIT = 0,
    AEGIS_GW_OPERATIONAL = 1,
    AEGIS_GW_SAFE_STATE = 2
} t_aegis_gw_mode;

typedef struct s_aegis_gw_config
{
    uint64_t period_ns;
    int32_t  cpu_core;
    int32_t  rt_priority;
    size_t   hmac_key_len;
    uint8_t  hmac_key[AEGIS_KEY_MAX_LEN];
} t_aegis_gw_config;

typedef struct s_aegis_gw_state
{
    AEGIS_ALIGNAS(AEGIS_CACHELINE_SIZE) t_aegis_frame_ctx frame_ctx;
    t_aegis_telemetry_data                                last_telemetry;
    t_aegis_gw_config                                     config;
    uint64_t                                              cycles_executed;
    uint64_t                                              frames_ok;
    uint64_t                                              frames_rejected;
    uint64_t                                              max_step_latency_ns;
    uint32_t                                              consec_auth_failures;
    t_aegis_gw_mode                                       mode;
    int32_t                                               rt_locked;
    uint16_t                                              last_dtc;
} t_aegis_gw_state;

AEGIS_STATIC_ASSERT((sizeof(t_aegis_gw_state) % AEGIS_CACHELINE_SIZE) == 0U
                        || sizeof(t_aegis_gw_state) >= AEGIS_ABI_CTX_SIZE,
                    "Gateway state must embed aligned 128-byte ABI context");

uint64_t aegis_monotonic_ns(void);
int      aegis_rt_lock_and_pin(int32_t cpu_core, int32_t rt_prio, int32_t *out_rt_active);
int      aegis_can_open_fd(const char *iface_name, int nonblocking);
int      aegis_gw_init(t_aegis_gw_state *state, const t_aegis_gw_config *cfg);
int      aegis_gw_step_frame(t_aegis_gw_state         *state,
                             const struct canfd_frame *rx_frame,
                             uint64_t                  now_ns);
int      aegis_gw_run_loop(t_aegis_gw_state *state,
                           int               can_fd,
                           uint32_t          max_cycles,
                           volatile int     *stop_flag);

#ifdef __cplusplus
}
#endif

#endif /* AEGIS_GATEWAY_H */
