/* ========================================================================== *
 * AEGIS Critical Systems - Subsystem 01: Hybrid Safety Gateway
 * File: 01-hybrid-can-gateway/src/aegis_rt_exec.c
 * Description: PREEMPT_RT Cyclic Executive, SocketCAN CAN-FD Ingestion,
 *              and Deterministic Safe-State Latch.
 * Standard: ISO/IEC 9899:2018 (C17) | POSIX.1-2008 | MISRA C:2012
 * ========================================================================== */

#define _GNU_SOURCE
#include "aegis_gateway.h"
#include <errno.h>
#include <fcntl.h>
#include <linux/can/raw.h>
#include <net/if.h>
#include <sched.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

static uint32_t decode_u32_le(const uint8_t *src)
{
    return ((uint32_t)src[0]) | ((uint32_t)src[1] << 8U)
           | ((uint32_t)src[2] << 16U) | ((uint32_t)src[3] << 24U);
}

static void add_ns_to_timespec(struct timespec *ts, uint64_t add_ns)
{
    uint64_t total_ns;

    total_ns = (uint64_t)ts->tv_nsec + add_ns;
    ts->tv_sec += (time_t)(total_ns / 1000000000ULL);
    ts->tv_nsec = (long)(total_ns % 1000000000ULL);
}

uint64_t aegis_monotonic_ns(void)
{
    struct timespec ts;

    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
        return (0ULL);
    return ((uint64_t)ts.tv_sec * 1000000000ULL) + (uint64_t)ts.tv_nsec;
}

/*
 * @req SRS-GW-005
 * @fault DTC-0x105
 * @hazard MIL-882E-CAT-I
 */
int aegis_rt_lock_and_pin(int32_t cpu_core, int32_t rt_prio, int32_t *out_rt_active)
{
    struct sched_param sp;
    cpu_set_t          cpuset;
    int32_t            rt_ok;

    if (out_rt_active == NULL)
        return (EINVAL);
    rt_ok = 1;
    if (mlockall(MCL_CURRENT | MCL_FUTURE) != 0)
        rt_ok = 0;
    if (cpu_core >= 0 && cpu_core < CPU_SETSIZE)
    {
        CPU_ZERO(&cpuset);
        CPU_SET((size_t)cpu_core, &cpuset);
        if (sched_setaffinity(0, sizeof(cpuset), &cpuset) != 0)
            rt_ok = 0;
    }
    if (rt_prio > 0)
    {
        (void)memset(&sp, 0, sizeof(sp));
        sp.sched_priority = rt_prio;
        if (sched_setscheduler(0, SCHED_FIFO, &sp) != 0)
            rt_ok = 0;
    }
    *out_rt_active = rt_ok;
    return (0);
}

int aegis_can_open_fd(const char *iface_name, int nonblocking)
{
    struct sockaddr_can addr;
    struct ifreq        ifr;
    int                 fd;
    int                 enable_canfd;
    int                 flags;
    size_t              name_len;

    if (iface_name == NULL)
        return (-EINVAL);
    name_len = strlen(iface_name);
    if (name_len == 0U || name_len >= IFNAMSIZ)
        return (-EINVAL);

    fd = socket(PF_CAN, SOCK_RAW, CAN_RAW);
    if (fd < 0)
        return (-errno);

    enable_canfd = 1;
    if (setsockopt(fd, SOL_CAN_RAW, CAN_RAW_FD_FRAMES, &enable_canfd, sizeof(enable_canfd)) < 0)
    {
        int err = errno;
        (void)close(fd);
        return (-err);
    }

    (void)memset(&ifr, 0, sizeof(ifr));
    (void)memcpy(ifr.ifr_name, iface_name, name_len);
    if (ioctl(fd, SIOCGIFINDEX, &ifr) < 0)
    {
        int err = errno;
        (void)close(fd);
        return (-err);
    }

    (void)memset(&addr, 0, sizeof(addr));
    addr.can_family = AF_CAN;
    addr.can_ifindex = ifr.ifr_ifindex;
    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0)
    {
        int err = errno;
        (void)close(fd);
        return (-err);
    }

    if (nonblocking != 0)
    {
        flags = fcntl(fd, F_GETFL, 0);
        if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0)
        {
            int err = errno;
            (void)close(fd);
            return (-err);
        }
    }

    return (fd);
}

int aegis_can_set_filter(int fd, uint32_t can_id, uint32_t can_mask)
{
    struct can_filter rfilter[1];

    if (fd < 0)
        return (-EINVAL);
    rfilter[0].can_id = can_id;
    rfilter[0].can_mask = can_mask;
    if (setsockopt(fd, SOL_CAN_RAW, CAN_RAW_FILTER, &rfilter, sizeof(rfilter)) < 0)
    {
        int err = errno;
        return (-err);
    }
    return (0);
}

int aegis_gw_init(t_aegis_gw_state *state, const t_aegis_gw_config *cfg)
{
    int rc;

    if (state == NULL || cfg == NULL)
        return (EINVAL);
    if (cfg->hmac_key_len == 0U || cfg->hmac_key_len > AEGIS_KEY_MAX_LEN)
        return (EINVAL);
    if (cfg->period_ns == 0ULL)
        return (EINVAL);

    (void)memset(state, 0, sizeof(t_aegis_gw_state));
    rc = aegis_frame_ctx_init(&state->frame_ctx);
    if (rc != 0)
        return (rc);

    state->config = *cfg;
    state->mode = AEGIS_GW_OPERATIONAL;
    state->last_dtc = AEGIS_DTC_NONE;
    return (0);
}

/*
 * @req SRS-GW-007
 * @fault DTC-0x106
 * @hazard MIL-882E-CAT-I
 */
int aegis_gw_check_antireplay(t_aegis_gw_state *state, uint32_t rx_seq)
{
    if (state == NULL)
        return (EINVAL);
    if (state->has_rx_seq != 0U && rx_seq <= state->last_valid_seq)
    {
        state->frames_rejected += 1ULL;
        state->last_dtc = AEGIS_DTC_REPLAY_ATTACK;
        state->consec_auth_failures += 1U;
        if (state->consec_auth_failures >= AEGIS_MAX_CONSEC_AUTH_FAIL)
            state->mode = AEGIS_GW_SAFE_STATE;
        return (EALREADY);
    }
    state->last_valid_seq = rx_seq;
    state->has_rx_seq = 1U;
    return (0);
}

/*
 * @req SRS-GW-006
 * @fault DTC-0x104
 * @hazard MIL-882E-CAT-I
 */
int aegis_gw_step_frame(t_aegis_gw_state         *state,
                        const struct canfd_frame *rx_frame,
                        uint64_t                  now_ns)
{
    t_aegis_telemetry_data decoded;
    int32_t                ffi_rc;

    if (state == NULL || rx_frame == NULL)
        return (EINVAL);
    if (state->mode == AEGIS_GW_SAFE_STATE)
        return (EOWNERDEAD);

    if (aegis_frame_ctx_init(&state->frame_ctx) != 0)
        return (EFAULT);

    state->frame_ctx.raw_frame = *rx_frame;
    state->frame_ctx.rx_timestamp_ns = now_ns;

    if (rx_frame->len == AEGIS_WIRE_FRAME_LEN)
    {
        state->frame_ctx.seq_counter = decode_u32_le(&rx_frame->data[12]);
        (void)memcpy(state->frame_ctx.hmac_tag, &rx_frame->data[16], AEGIS_HMAC_TAG_LEN);
        state->frame_ctx.raw_frame.len = AEGIS_TELEMETRY_LEN;
    }

    (void)memset(&decoded, 0, sizeof(decoded));
    ffi_rc = aegis_rust_process_frame(&state->frame_ctx,
                                      state->config.hmac_key,
                                      state->config.hmac_key_len,
                                      &decoded);
    state->frame_ctx.auth_timestamp_ns = aegis_monotonic_ns();

    if (ffi_rc == 0)
    {
        int replay_rc = aegis_gw_check_antireplay(state, state->frame_ctx.seq_counter);
        if (replay_rc != 0)
            return (replay_rc);
        state->last_telemetry = decoded;
        state->frames_ok += 1ULL;
        state->consec_auth_failures = 0U;
        state->last_dtc = AEGIS_DTC_NONE;
        return (0);
    }

    state->frames_rejected += 1ULL;
    state->last_dtc = state->frame_ctx.dtc_code;
    if (ffi_rc == -EACCES)
    {
        state->consec_auth_failures += 1U;
        if (state->consec_auth_failures >= AEGIS_MAX_CONSEC_AUTH_FAIL)
        {
            state->mode = AEGIS_GW_SAFE_STATE;
        }
    }
    return (-ffi_rc);
}

int aegis_gw_run_loop(t_aegis_gw_state *state,
                      int               can_fd,
                      uint32_t          max_cycles,
                      volatile int     *stop_flag)
{
    struct timespec    next_wakeup;
    struct canfd_frame rx_frame;
    ssize_t            nbytes;
    uint64_t           t_start;
    uint64_t           t_elapsed;
    uint32_t           cycle;

    if (state == NULL || can_fd < 0)
        return (EINVAL);
    if (clock_gettime(CLOCK_MONOTONIC, &next_wakeup) != 0)
        return (errno);

    cycle = 0U;
    while (max_cycles == 0U || cycle < max_cycles)
    {
        if (stop_flag != NULL && *stop_flag != 0)
            break;
        if (state->mode == AEGIS_GW_SAFE_STATE)
            break;

        t_start = aegis_monotonic_ns();
        for (;;)
        {
            nbytes = read(can_fd, &rx_frame, sizeof(rx_frame));
            if (nbytes == (ssize_t)sizeof(rx_frame))
            {
                (void)aegis_gw_step_frame(state, &rx_frame, t_start);
                if (state->mode == AEGIS_GW_SAFE_STATE)
                    break;
            }
            else
            {
                if (nbytes < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR))
                    break;
                break;
            }
        }

        t_elapsed = aegis_monotonic_ns() - t_start;
        if (t_elapsed > state->max_step_latency_ns)
            state->max_step_latency_ns = t_elapsed;
        state->cycles_executed += 1ULL;
        cycle += 1U;

        if (max_cycles != 0U && cycle >= max_cycles)
            break;
        add_ns_to_timespec(&next_wakeup, state->config.period_ns);
        (void)clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &next_wakeup, NULL);
    }

    return (0);
}
