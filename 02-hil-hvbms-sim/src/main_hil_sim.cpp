/* ========================================================================== *
 * AEGIS Critical Systems - Subsystem 02: HV-BMS & Propulsion HIL Simulator
 * File: 02-hil-hvbms-sim/src/main_hil_sim.cpp
 * Description: Standalone 1 kHz SocketCAN HIL Plant & MIL-STD-882E Injector
 * Standard: ISO C++23 | POSIX.1-2008 | -fno-exceptions -fno-rtti
 * ========================================================================== */

#include "hvbms_plant.hpp"
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <time.h>
#include <unistd.h>

namespace
{

volatile std::sig_atomic_t g_hil_stop = 0;

void handle_sig(int /*signum*/) noexcept
{
    g_hil_stop = 1;
}

void add_ns(struct timespec &ts, std::uint64_t add) noexcept
{
    const std::uint64_t total_ns = static_cast<std::uint64_t>(ts.tv_nsec) + add;
    ts.tv_sec += static_cast<time_t>(total_ns / 1000000000ULL);
    ts.tv_nsec = static_cast<long>(total_ns % 1000000000ULL);
}

aegis::hil::FaultMode parse_fault_mode(const char *arg) noexcept
{
    if (std::strcmp(arg, "thermal") == 0)
        return aegis::hil::FaultMode::ThermalRunaway;
    if (std::strcmp(arg, "surge") == 0)
        return aegis::hil::FaultMode::OverCurrentSurge;
    if (std::strcmp(arg, "emi") == 0)
        return aegis::hil::FaultMode::EmiBitFlip;
    if (std::strcmp(arg, "auth") == 0)
        return aegis::hil::FaultMode::AuthTagCorrupt;
    return aegis::hil::FaultMode::None;
}

} // namespace

int main(int argc, char **argv)
{
    const char             *iface = "vcan0";
    std::uint32_t           max_cycles = 100U;
    aegis::hil::FaultMode   injected_fault = aegis::hil::FaultMode::None;
    std::uint32_t           fault_at_cycle = 50U;
    const std::uint8_t      hmac_key[] = "AEGIS_HV_BMS_SHARED_SECRET_2026";

    for (int i = 1; i < argc; ++i)
    {
        if (std::strcmp(argv[i], "--iface") == 0 && (i + 1) < argc)
        {
            iface = argv[++i];
        }
        else if (std::strcmp(argv[i], "--cycles") == 0 && (i + 1) < argc)
        {
            char *endptr = nullptr;
            const unsigned long val = std::strtoul(argv[++i], &endptr, 10);
            if (*endptr != '\0')
            {
                std::fprintf(stderr, "[AEGIS-HIL] Invalid --cycles value\n");
                return 1;
            }
            max_cycles = static_cast<std::uint32_t>(val);
        }
        else if (std::strcmp(argv[i], "--fault") == 0 && (i + 1) < argc)
        {
            const char *mode_arg = argv[++i];
            if (std::strcmp(mode_arg, "none") != 0 && parse_fault_mode(mode_arg) == aegis::hil::FaultMode::None)
            {
                std::fprintf(stderr, "[AEGIS-HIL] Unknown --fault mode: %s\n", mode_arg);
                return 1;
            }
            injected_fault = parse_fault_mode(mode_arg);
        }
        else if (std::strcmp(argv[i], "--fault-at") == 0 && (i + 1) < argc)
        {
            char *endptr = nullptr;
            const unsigned long val = std::strtoul(argv[++i], &endptr, 10);
            if (*endptr != '\0')
            {
                std::fprintf(stderr, "[AEGIS-HIL] Invalid --fault-at value\n");
                return 1;
            }
            fault_at_cycle = static_cast<std::uint32_t>(val);
        }
        else if (std::strcmp(argv[i], "--help") == 0 || std::strcmp(argv[i], "-h") == 0)
        {
            std::printf(
                "Usage: %s [--iface IFACE] [--cycles N] [--fault none|thermal|surge|emi|auth] [--fault-at CYCLE]\n",
                argv[0]);
            return 0;
        }
        else
        {
            std::fprintf(stderr, "[AEGIS-HIL] Unknown or incomplete argument: %s\n", argv[i]);
            return 1;
        }
    }

    struct sigaction sa {};
    sa.sa_handler = handle_sig;
    (void)sigaction(SIGINT, &sa, nullptr);
    (void)sigaction(SIGTERM, &sa, nullptr);

    const int can_fd = aegis_can_open_fd(iface, 0);
    if (can_fd < 0)
    {
        std::fprintf(stderr,
                     "[AEGIS-HIL] Cannot open SocketCAN '%s' (err=%d). Run scripts/setup_vcan.sh first.\n",
                     iface,
                     -can_fd);
        return 1;
    }

    const aegis::hil::PlantConfig cfg {
        420000U, /* 420.0 V OCV */
        45U,     /* 45 mOhm internal resistance */
        50000,   /* 50.0 A nominal load */
        250,     /* 25.0 C ambient */
        500,     /* 50.0 C warn */
        600,     /* 60.0 C critical */
        {0U, 0U}
    };
    aegis::hil::HvBmsPlant plant(cfg);

    struct timespec wakeup {};
    (void)clock_gettime(CLOCK_MONOTONIC, &wakeup);

    std::printf("[AEGIS-HIL] HV-BMS Plant STARTED on '%s' (1 kHz, cycles=%u)\n",
                iface,
                max_cycles);
    (void)std::fflush(stdout);

    for (std::uint32_t c = 0U; (max_cycles == 0U || c < max_cycles) && g_hil_stop == 0; ++c)
    {
        if (injected_fault != aegis::hil::FaultMode::None && c >= fault_at_cycle)
        {
            plant.set_fault_mode(injected_fault);
        }
        plant.tick_1khz();

        struct canfd_frame frame {};
        if (plant.build_canfd_wire_frame(frame, hmac_key, sizeof(hmac_key) - 1U) == 0)
        {
            const ssize_t wr = write(can_fd, &frame, sizeof(frame));
            if (wr != static_cast<ssize_t>(sizeof(frame)))
            {
                std::fprintf(stderr, "[AEGIS-HIL] SocketCAN write warning (wr=%zd)\n", wr);
            }
        }

        add_ns(wakeup, AEGIS_DEFAULT_PERIOD_NS);
        (void)clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &wakeup, nullptr);
    }

    (void)close(can_fd);
    std::printf("[AEGIS-HIL] Shutdown: state=%u V=%u mV I=%d mA T=%d dC seq=%u\n",
                static_cast<unsigned>(plant.state()),
                plant.pack_voltage_mv(),
                plant.pack_current_ma(),
                static_cast<int>(plant.cell_temp_dc()),
                plant.seq_counter());
    return 0;
}
