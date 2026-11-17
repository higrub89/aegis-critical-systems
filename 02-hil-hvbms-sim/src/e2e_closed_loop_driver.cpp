/* ========================================================================== *
 * AEGIS Critical Systems - Integrated Multi-Subsystem E2E Closed-Loop Driver
 * File: 02-hil-hvbms-sim/src/e2e_closed_loop_driver.cpp
 * Description: Exercises HIL Plant (C++23) -> CAN-FD Wire -> Gateway (C17 +
 *              Rust no_std HMAC) -> POSIX SHM Seqlock -> HUMS Q15 EKF ->
 *              TM/TC Daemon -> Java 21 GCS -> TC_SAFE_STATE Return Ack.
 * Standard: ISO C++23 | POSIX.1-2008 | -fno-exceptions -fno-rtti
 * ========================================================================== */

#include "aegis_shm_bus.h"
#include "hvbms_plant.hpp"
#include "q15_ekf.hpp"
#include "spsc_ring.hpp"
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

namespace
{

t_aegis_tmtc_shm *open_or_attach_shm(const char *shm_name, int *out_created) noexcept
{
    int created = 1;
    int fd = shm_open(shm_name, O_CREAT | O_EXCL | O_RDWR, S_IRUSR | S_IWUSR);
    if (fd < 0)
    {
        created = 0;
        fd = shm_open(shm_name, O_RDWR, S_IRUSR | S_IWUSR);
    }
    if (fd < 0)
    {
        return nullptr;
    }
    if (created != 0 && ftruncate(fd, static_cast<off_t>(sizeof(t_aegis_tmtc_shm))) != 0)
    {
        (void)close(fd);
        (void)shm_unlink(shm_name);
        return nullptr;
    }

    void *mapped =
        mmap(nullptr, sizeof(t_aegis_tmtc_shm), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    (void)close(fd);
    if (mapped == MAP_FAILED)
    {
        if (created != 0)
        {
            (void)shm_unlink(shm_name);
        }
        return nullptr;
    }

    auto *shm = static_cast<t_aegis_tmtc_shm *>(mapped);
    if (created != 0)
    {
        std::memset(shm, 0, sizeof(t_aegis_tmtc_shm));
    }
    *out_created = created;
    return shm;
}

void sleep_ms(std::uint32_t ms) noexcept
{
    struct timespec req {};
    req.tv_sec = static_cast<time_t>(ms / 1000U);
    req.tv_nsec = static_cast<long>((ms % 1000U) * 1000000ULL);
    while (nanosleep(&req, &req) != 0 && errno == EINTR)
    {
    }
}

alignas(64) aegis::hums::SpscRingBuffer<t_aegis_telemetry_data, 128U> g_hums_ring;
alignas(64) t_aegis_gw_state g_gw_state;

} // namespace

int main(int argc, char **argv)
{
    const char   *shm_name = AEGIS_DEFAULT_SHM_NAME;
    std::uint32_t max_ticks = 200U;
    std::uint32_t fault_at_tick = 10U;
    std::uint32_t period_ms = 20U;

    for (int i = 1; i < argc; ++i)
    {
        if (std::strcmp(argv[i], "--shm") == 0 && (i + 1) < argc)
        {
            shm_name = argv[++i];
        }
        else if (std::strcmp(argv[i], "--ticks") == 0 && (i + 1) < argc)
        {
            max_ticks = static_cast<std::uint32_t>(std::strtoul(argv[++i], nullptr, 10));
        }
        else if (std::strcmp(argv[i], "--fault-at") == 0 && (i + 1) < argc)
        {
            fault_at_tick = static_cast<std::uint32_t>(std::strtoul(argv[++i], nullptr, 10));
        }
        else if (std::strcmp(argv[i], "--period-ms") == 0 && (i + 1) < argc)
        {
            period_ms = static_cast<std::uint32_t>(std::strtoul(argv[++i], nullptr, 10));
        }
    }

    int               shm_created = 0;
    t_aegis_tmtc_shm *shm = open_or_attach_shm(shm_name, &shm_created);
    if (shm == nullptr)
    {
        std::fprintf(stderr, "[AEGIS-E2E-DRV] Failed to open POSIX SHM '%s'\n", shm_name);
        return 1;
    }

    const std::uint8_t key[] = "AEGIS_HV_BMS_SHARED_SECRET_2026";
    t_aegis_gw_config  gw_cfg {};
    gw_cfg.period_ns = AEGIS_DEFAULT_PERIOD_NS;
    gw_cfg.cpu_core = 0;
    gw_cfg.rt_priority = 0;
    gw_cfg.hmac_key_len = sizeof(key) - 1U;
    std::memcpy(gw_cfg.hmac_key, key, gw_cfg.hmac_key_len);

    if (aegis_gw_init(&g_gw_state, &gw_cfg) != 0)
    {
        (void)munmap(shm, sizeof(t_aegis_tmtc_shm));
        if (shm_created != 0)
        {
            (void)shm_unlink(shm_name);
        }
        return 2;
    }

    int sv[2] = {-1, -1};
    if (socketpair(AF_UNIX, SOCK_DGRAM, 0, sv) != 0)
    {
        (void)munmap(shm, sizeof(t_aegis_tmtc_shm));
        if (shm_created != 0)
        {
            (void)shm_unlink(shm_name);
        }
        return 3;
    }
    const int fl = fcntl(sv[0], F_GETFL, 0);
    if (fl < 0 || fcntl(sv[0], F_SETFL, fl | O_NONBLOCK) < 0)
    {
        (void)close(sv[0]);
        (void)close(sv[1]);
        (void)munmap(shm, sizeof(t_aegis_tmtc_shm));
        if (shm_created != 0)
        {
            (void)shm_unlink(shm_name);
        }
        return 4;
    }

    const aegis::hil::PlantConfig plant_cfg {
        420000U, 45U, 50000, 250, 500, 600, {0U, 0U}
    };
    aegis::hil::HvBmsPlant plant(plant_cfg);

    const aegis::hums::Q15FilterConfig ekf_cfg {
        3277,   /* K = 0.10 in Q15 (slow thermal plant tracking) */
        350,    /* ~1.07% innovation anomaly threshold (~0.43 C rapid drift) */
        417750, /* nominal loaded voltage */
        50000,
        250,
        400
    };
    aegis::hums::Q15AnomalyFilter ekf(ekf_cfg);

    std::uint64_t hums_anomalies = 0ULL;
    std::uint32_t tc_acked = 0U;

    /* Complete Init -> Precharge -> Operational ramp and publish nominal baseline */
    for (int ramp = 0; ramp < 4; ++ramp)
    {
        plant.tick_1khz();
    }
    {
        struct canfd_frame init_wire {};
        if (plant.build_canfd_wire_frame(init_wire, key, gw_cfg.hmac_key_len) == 0)
        {
            const std::uint64_t now_ns = aegis_monotonic_ns();
            if (aegis_gw_step_frame(&g_gw_state, &init_wire, now_ns) == 0)
            {
                aegis_shm_publish(shm,
                                  &g_gw_state.last_telemetry,
                                  static_cast<std::uint16_t>(g_gw_state.mode),
                                  g_gw_state.last_dtc,
                                  now_ns);
                if (g_hums_ring.try_push(g_gw_state.last_telemetry))
                {
                    t_aegis_telemetry_data hums_item {};
                    if (g_hums_ring.try_pop(hums_item))
                    {
                        (void)ekf.step(hums_item);
                    }
                }
            }
        }
    }

    for (std::uint32_t tick = 0U; tick < max_ticks; ++tick)
    {
        if (tick == fault_at_tick)
        {
            plant.set_fault_mode(aegis::hil::FaultMode::ThermalRunaway);
        }
        plant.tick_1khz();

        struct canfd_frame wire_frame {};
        if (plant.build_canfd_wire_frame(wire_frame, key, gw_cfg.hmac_key_len) != 0)
        {
            break;
        }
        if (write(sv[1], &wire_frame, sizeof(wire_frame))
            != static_cast<ssize_t>(sizeof(wire_frame)))
        {
            break;
        }

        struct canfd_frame rx_frame {};
        if (read(sv[0], &rx_frame, sizeof(rx_frame)) == static_cast<ssize_t>(sizeof(rx_frame)))
        {
            const std::uint64_t now_ns = aegis_monotonic_ns();
            if (aegis_gw_step_frame(&g_gw_state, &rx_frame, now_ns) == 0)
            {
                aegis_shm_publish(shm,
                                  &g_gw_state.last_telemetry,
                                  static_cast<std::uint16_t>(g_gw_state.mode),
                                  g_gw_state.last_dtc,
                                  now_ns);
                if (g_hums_ring.try_push(g_gw_state.last_telemetry))
                {
                    t_aegis_telemetry_data hums_item {};
                    if (g_hums_ring.try_pop(hums_item))
                    {
                        const auto ekf_out = ekf.step(hums_item);
                        hums_anomalies += ekf_out.anomaly_detected;
                    }
                }
            }
        }

        if (__atomic_load_n(&shm->tc_safe_state_req, __ATOMIC_ACQUIRE) != 0U)
        {
            g_gw_state.mode = AEGIS_GW_SAFE_STATE;
            g_gw_state.last_dtc = AEGIS_DTC_AUTH_FAILURE;
            aegis_shm_publish(shm,
                              &g_gw_state.last_telemetry,
                              static_cast<std::uint16_t>(g_gw_state.mode),
                              g_gw_state.last_dtc,
                              aegis_monotonic_ns());
            tc_acked = __atomic_add_fetch(&shm->tc_ack, 1U, __ATOMIC_RELEASE);
            break;
        }

        sleep_ms(period_ms);
    }

    (void)close(sv[0]);
    (void)close(sv[1]);
    (void)munmap(shm, sizeof(t_aegis_tmtc_shm));
    if (shm_created != 0)
    {
        (void)shm_unlink(shm_name);
    }

    std::printf(
        "[AEGIS-E2E-DRV] frames_ok=%llu hums_anomalies=%llu gw_mode=%d tc_ack=%u\n",
        static_cast<unsigned long long>(g_gw_state.frames_ok),
        static_cast<unsigned long long>(hums_anomalies),
        static_cast<int>(g_gw_state.mode),
        tc_acked);

    if (g_gw_state.frames_ok < 10ULL || g_gw_state.mode != AEGIS_GW_SAFE_STATE
        || tc_acked < 1U || hums_anomalies == 0ULL)
    {
        return 5;
    }
    return 0;
}
