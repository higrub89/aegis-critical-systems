/* ========================================================================== *
 * AEGIS Critical Systems - Subsystem 01: Hybrid Safety Gateway
 * File: 01-hybrid-can-gateway/src/main_gateway.c
 * Description: Standalone PREEMPT_RT Cyclic Executive Node over SocketCAN
 *              and POSIX Shared Memory Telemetry Bus.
 * Standard: ISO/IEC 9899:2018 (C17) | POSIX.1-2008
 * ========================================================================== */

#define _POSIX_C_SOURCE 200809L
#include "aegis_gateway.h"
#include "aegis_shm_bus.h"
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

static volatile sig_atomic_t g_stop_requested = 0;

static void handle_sigint(int signum)
{
    (void)signum;
    g_stop_requested = 1;
}

static int parse_u32_arg(const char *arg, uint32_t *out)
{
    char         *endptr;
    unsigned long val;

    if (arg == NULL || out == NULL || arg[0] == '\0')
        return (-1);
    val = strtoul(arg, &endptr, 10);
    if (*endptr != '\0')
        return (-1);
    *out = (uint32_t)val;
    return (0);
}

static t_aegis_tmtc_shm *open_shm_bus(const char *shm_name)
{
    int               fd;
    t_aegis_tmtc_shm *ptr;

    if (shm_name == NULL || shm_name[0] != '/')
        return (NULL);
    fd = shm_open(shm_name, O_CREAT | O_RDWR, S_IRUSR | S_IWUSR);
    if (fd < 0)
        return (NULL);
    if (ftruncate(fd, (off_t)sizeof(t_aegis_tmtc_shm)) != 0)
    {
        (void)close(fd);
        return (NULL);
    }
    ptr = (t_aegis_tmtc_shm *)mmap(
        NULL, sizeof(t_aegis_tmtc_shm), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    (void)close(fd);
    if (ptr == MAP_FAILED)
        return (NULL);
    return (ptr);
}

int main(int argc, char **argv)
{
    _Alignas(AEGIS_CACHELINE_SIZE) static t_aegis_gw_state g_state;
    const uint8_t     default_key[] = "AEGIS_HV_BMS_SHARED_SECRET_2026";
    t_aegis_gw_config cfg;
    struct sigaction  sa;
    const char       *iface;
    const char       *shm_name;
    t_aegis_tmtc_shm *shm_bus;
    uint32_t          max_cycles;
    int32_t           rt_active;
    int               can_fd;
    int               stop_local;
    int               i;

    iface = "vcan0";
    shm_name = AEGIS_DEFAULT_SHM_NAME;
    max_cycles = 0U;
    (void)memset(&cfg, 0, sizeof(cfg));
    cfg.period_ns = AEGIS_DEFAULT_PERIOD_NS;
    cfg.cpu_core = 0;
    cfg.rt_priority = 80;
    cfg.hmac_key_len = sizeof(default_key) - 1U;
    (void)memcpy(cfg.hmac_key, default_key, cfg.hmac_key_len);

    for (i = 1; i < argc; ++i)
    {
        if (strcmp(argv[i], "--iface") == 0 && (i + 1) < argc)
        {
            iface = argv[++i];
        }
        else if (strcmp(argv[i], "--shm") == 0 && (i + 1) < argc)
        {
            shm_name = argv[++i];
        }
        else if (strcmp(argv[i], "--cycles") == 0 && (i + 1) < argc)
        {
            if (parse_u32_arg(argv[++i], &max_cycles) != 0)
            {
                (void)fprintf(stderr, "[AEGIS-GW] Invalid --cycles value\n");
                return (1);
            }
        }
        else if (strcmp(argv[i], "--cpu") == 0 && (i + 1) < argc)
        {
            uint32_t tmp_cpu = 0U;
            if (parse_u32_arg(argv[++i], &tmp_cpu) != 0)
                return (1);
            cfg.cpu_core = (int32_t)tmp_cpu;
        }
    }

    (void)memset(&sa, 0, sizeof(sa));
    sa.sa_handler = handle_sigint;
    (void)sigaction(SIGINT, &sa, NULL);
    (void)sigaction(SIGTERM, &sa, NULL);

    if (aegis_gw_init(&g_state, &cfg) != 0)
    {
        (void)fprintf(stderr, "[AEGIS-GW] Failed to initialize gateway state\n");
        return (1);
    }

    rt_active = 0;
    (void)aegis_rt_lock_and_pin(cfg.cpu_core, cfg.rt_priority, &rt_active);
    g_state.rt_locked = rt_active;

    can_fd = aegis_can_open_fd(iface, 1);
    if (can_fd < 0)
    {
        (void)fprintf(stderr,
                      "[AEGIS-GW] Cannot open SocketCAN '%s' (err=%d). Run scripts/setup_vcan.sh first.\n",
                      iface,
                      -can_fd);
        return (1);
    }

    shm_bus = open_shm_bus(shm_name);
    (void)printf("[AEGIS-GW] Cyclic Executive STARTED on '%s' (1 kHz, rt_locked=%d, shm=%s)\n",
                 iface,
                 g_state.rt_locked,
                 shm_bus != NULL ? shm_name : "disabled");

    stop_local = 0;
    while (g_stop_requested == 0 && g_state.mode != AEGIS_GW_SAFE_STATE)
    {
        uint32_t step_cycles = (max_cycles == 0U || max_cycles > 10U) ? 10U : max_cycles;
        (void)aegis_gw_run_loop(&g_state, can_fd, step_cycles, &stop_local);

        if (shm_bus != NULL)
        {
            if (__atomic_load_n(&shm_bus->tc_safe_state_req, __ATOMIC_ACQUIRE) != 0U)
            {
                g_state.mode = AEGIS_GW_SAFE_STATE;
                g_state.last_dtc = AEGIS_DTC_AUTH_FAILURE;
            }
            shm_bus->telemetry = g_state.last_telemetry;
            shm_bus->gw_mode = (uint16_t)g_state.mode;
            shm_bus->last_dtc = g_state.last_dtc;
            shm_bus->timestamp_ns = aegis_monotonic_ns();
            __atomic_fetch_add(&shm_bus->seq_lock, 1U, __ATOMIC_RELEASE);
        }

        if (max_cycles != 0U)
        {
            if (g_state.cycles_executed >= max_cycles)
                break;
        }
        if (g_stop_requested != 0)
            stop_local = 1;
    }

    if (shm_bus != NULL)
        (void)munmap(shm_bus, sizeof(t_aegis_tmtc_shm));
    (void)close(can_fd);
    (void)printf("[AEGIS-GW] Shutdown: mode=%d cycles=%llu ok=%llu rejected=%llu max_lat_ns=%llu\n",
                 (int)g_state.mode,
                 (unsigned long long)g_state.cycles_executed,
                 (unsigned long long)g_state.frames_ok,
                 (unsigned long long)g_state.frames_rejected,
                 (unsigned long long)g_state.max_step_latency_ns);
    return (0);
}
