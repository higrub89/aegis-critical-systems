/* ========================================================================== *
 * AEGIS Critical Systems - Subsystem 02: HV-BMS & Propulsion HIL Simulator
 * File: 02-hil-hvbms-sim/src/test_hil_fsm.cpp
 * Description: Closed-Loop HIL <-> Gateway Verification, Thermal Runaway
 *              Safe-State Transition (< 500 us) & MIL-STD-882E Fault Test.
 * Standard: ISO C++23 | -fno-exceptions -fno-rtti
 * ========================================================================== */

#include "hvbms_plant.hpp"
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <sys/socket.h>
#include <unistd.h>

namespace
{

int test_fsm_wcet_latency() noexcept
{
    const std::uint64_t t0 = aegis_monotonic_ns();
    aegis::hil::BmsState s = aegis::hil::BmsState::Operational;
    s = aegis::hil::step_safety_fsm(s, aegis::hil::BmsEvent::FaultCritical);
    const std::uint8_t  contactor = aegis::hil::contactor_for_state(s);
    const std::uint64_t t1 = aegis_monotonic_ns();

    if (s != aegis::hil::BmsState::SafeState || contactor != 0U)
    {
        return 1;
    }
    /* Requirement: Safe-State transition strictly < 500 us (500,000 ns) */
    if ((t1 - t0) >= 500000ULL)
    {
        return 2;
    }
    return 0;
}

int test_thermal_runaway_physics() noexcept
{
    const aegis::hil::PlantConfig cfg {
        420000U, /* 420.0 V OCV */
        50U,     /* 50 mOhm internal resistance */
        80000,   /* 80.0 A load -> 4000 mV sag -> 416,000 mV */
        250,     /* 25.0 C */
        500,     /* 50.0 C warn */
        600,     /* 60.0 C critical */
        {0U, 0U}
    };
    aegis::hil::HvBmsPlant plant(cfg);

    /* Ramp Init -> Precharge -> Operational */
    for (int i = 0; i < 4; ++i)
    {
        plant.tick_1khz();
    }
    if (plant.state() != aegis::hil::BmsState::Operational)
    {
        return 1;
    }
    if (plant.pack_voltage_mv() != 416000U)
    {
        return 2;
    }

    /* Inject MIL-STD-882E Thermal Runaway */
    plant.set_fault_mode(aegis::hil::FaultMode::ThermalRunaway);
    for (int i = 0; i < 200; ++i)
    {
        plant.tick_1khz();
        if (plant.state() == aegis::hil::BmsState::SafeState)
        {
            break;
        }
    }
    if (plant.state() != aegis::hil::BmsState::SafeState)
    {
        return 3;
    }
    if (aegis::hil::contactor_for_state(plant.state()) != 0U)
    {
        return 4;
    }
    return 0;
}

int test_closed_loop_hil_to_gateway() noexcept
{
    AEGIS_ALIGNAS(AEGIS_CACHELINE_SIZE) static t_aegis_gw_state gw_state;
    const std::uint8_t key[] = "AEGIS_HV_BMS_SHARED_SECRET_2026";
    t_aegis_gw_config  gw_cfg {};

    gw_cfg.period_ns = 100000ULL;
    gw_cfg.cpu_core = 0;
    gw_cfg.rt_priority = 0;
    gw_cfg.hmac_key_len = sizeof(key) - 1U;
    std::memcpy(gw_cfg.hmac_key, key, gw_cfg.hmac_key_len);

    if (aegis_gw_init(&gw_state, &gw_cfg) != 0)
    {
        return 1;
    }

    const aegis::hil::PlantConfig plant_cfg {
        400000U, 40U, 50000, 250, 500, 600, {0U, 0U}
    };
    aegis::hil::HvBmsPlant plant(plant_cfg);

    int sv[2] = {-1, -1};
    if (socketpair(AF_UNIX, SOCK_DGRAM, 0, sv) != 0)
    {
        return 2;
    }
    const int flags = fcntl(sv[0], F_GETFL, 0);
    if (flags < 0 || fcntl(sv[0], F_SETFL, flags | O_NONBLOCK) < 0)
    {
        (void)close(sv[0]);
        (void)close(sv[1]);
        return 3;
    }

    /* Emit 4 nominal authenticated CAN-FD frames from HIL plant */
    for (int i = 0; i < 4; ++i)
    {
        plant.tick_1khz();
        struct canfd_frame frame {};
        if (plant.build_canfd_wire_frame(frame, key, gw_cfg.hmac_key_len) != 0)
        {
            (void)close(sv[0]);
            (void)close(sv[1]);
            return 4;
        }
        if (write(sv[1], &frame, sizeof(frame)) != static_cast<ssize_t>(sizeof(frame)))
        {
            (void)close(sv[0]);
            (void)close(sv[1]);
            return 5;
        }
    }

    int stop_flag = 0;
    (void)aegis_gw_run_loop(&gw_state, sv[0], 2U, &stop_flag);
    if (gw_state.mode != AEGIS_GW_OPERATIONAL || gw_state.frames_ok != 4ULL
        || gw_state.last_telemetry.pack_voltage_mv != 398000U)
    {
        (void)close(sv[0]);
        (void)close(sv[1]);
        return 6;
    }

    /* Inject MIL-STD-882E EMI Bit-Flip over the wire (3 frames -> Gateway Safe-State) */
    plant.set_fault_mode(aegis::hil::FaultMode::EmiBitFlip);
    for (int i = 0; i < 3; ++i)
    {
        plant.tick_1khz();
        struct canfd_frame frame {};
        if (plant.build_canfd_wire_frame(frame, key, gw_cfg.hmac_key_len) != 0)
        {
            (void)close(sv[0]);
            (void)close(sv[1]);
            return 7;
        }
        if (write(sv[1], &frame, sizeof(frame)) != static_cast<ssize_t>(sizeof(frame)))
        {
            (void)close(sv[0]);
            (void)close(sv[1]);
            return 8;
        }
    }

    (void)aegis_gw_run_loop(&gw_state, sv[0], 2U, &stop_flag);
    (void)close(sv[0]);
    (void)close(sv[1]);

    if (gw_state.mode != AEGIS_GW_SAFE_STATE || gw_state.frames_rejected != 3ULL
        || gw_state.last_dtc != AEGIS_DTC_AUTH_FAILURE)
    {
        return 9;
    }
    return 0;
}

} // namespace

int main()
{
    int rc = test_fsm_wcet_latency();
    if (rc != 0)
    {
        std::fprintf(stderr, "[FAIL] test_fsm_wcet_latency: %d\n", rc);
        return 1;
    }
    rc = test_thermal_runaway_physics();
    if (rc != 0)
    {
        std::fprintf(stderr, "[FAIL] test_thermal_runaway_physics: %d\n", rc);
        return 1;
    }
    rc = test_closed_loop_hil_to_gateway();
    if (rc != 0)
    {
        std::fprintf(stderr, "[FAIL] test_closed_loop_hil_to_gateway: %d\n", rc);
        return 1;
    }
    std::printf("[PASS] C++23 HIL HV-BMS, 2D constexpr FSM (<500us) & Closed-Loop Gateway verified.\n");
    return 0;
}
