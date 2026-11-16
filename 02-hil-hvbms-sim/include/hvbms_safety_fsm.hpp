/* ========================================================================== *
 * AEGIS Critical Systems - Subsystem 02: HV-BMS & Propulsion HIL Simulator
 * File: 02-hil-hvbms-sim/include/hvbms_safety_fsm.hpp
 * Description: Zero-Indirection O(1) 2D constexpr Safety State Machine
 * Standard: ISO C++23 | MISRA C++:2023 | DO-178C DAL-A
 * ========================================================================== */

#ifndef AEGIS_HVBMS_SAFETY_FSM_HPP
#define AEGIS_HVBMS_SAFETY_FSM_HPP

#include <array>
#include <cstddef>
#include <cstdint>

namespace aegis::hil
{

enum class BmsState : std::uint8_t
{
    Init = 0U,
    Precharge = 1U,
    Operational = 2U,
    Degraded = 3U,
    SafeState = 4U
};

enum class BmsEvent : std::uint8_t
{
    SelfTestOk = 0U,
    PrechargeDone = 1U,
    NominalTick = 2U,
    OverTempWarn = 3U,
    FaultCritical = 4U,
    AuthViolation = 5U,
    ResetReq = 6U
};

inline constexpr std::size_t kBmsStateCount = 5U;
inline constexpr std::size_t kBmsEventCount = 7U;

/*
 * Compile-time O(1) 2D transition matrix indexed by [State][Event].
 * Eliminates std::visit / jump-table indirection per MISRA C++:2023 WCET rules.
 */
inline constexpr std::array<std::array<BmsState, kBmsEventCount>, kBmsStateCount>
    kBmsTransitionTable = {{
        /* Init */
        {BmsState::Precharge, BmsState::Init, BmsState::Init, BmsState::Degraded,
         BmsState::SafeState, BmsState::SafeState, BmsState::Init},
        /* Precharge */
        {BmsState::Precharge, BmsState::Operational, BmsState::Precharge,
         BmsState::Degraded, BmsState::SafeState, BmsState::SafeState, BmsState::Init},
        /* Operational */
        {BmsState::Operational, BmsState::Operational, BmsState::Operational,
         BmsState::Degraded, BmsState::SafeState, BmsState::SafeState,
         BmsState::Operational},
        /* Degraded */
        {BmsState::Degraded, BmsState::Degraded, BmsState::Degraded,
         BmsState::Degraded, BmsState::SafeState, BmsState::SafeState,
         BmsState::Init},
        /* SafeState (absorbing until explicit ResetReq) */
        {BmsState::SafeState, BmsState::SafeState, BmsState::SafeState,
         BmsState::SafeState, BmsState::SafeState, BmsState::SafeState,
         BmsState::Init},
    }};

[[nodiscard]] constexpr std::uint8_t contactor_for_state(BmsState state) noexcept
{
    switch (state)
    {
    case BmsState::Init:
    case BmsState::SafeState:
        return 0U; /* Contactor OPEN */
    case BmsState::Precharge:
        return 1U; /* PRECHARGE */
    case BmsState::Operational:
    case BmsState::Degraded:
        return 2U; /* Contactor CLOSED */
    }
    return 0U;
}

/*
 * @req SRS-HIL-001
 * @fault DTC-0x201
 * @hazard MIL-882E-CAT-I
 */
[[nodiscard]] constexpr BmsState step_safety_fsm(BmsState current,
                                                 BmsEvent event) noexcept
{
    const auto s_idx = static_cast<std::size_t>(current);
    const auto e_idx = static_cast<std::size_t>(event);
    if (s_idx >= kBmsStateCount || e_idx >= kBmsEventCount)
    {
        return BmsState::SafeState;
    }
    return kBmsTransitionTable[s_idx][e_idx];
}

/* Compile-time DAL-A invariants */
static_assert(step_safety_fsm(BmsState::Operational, BmsEvent::FaultCritical)
                  == BmsState::SafeState,
              "FaultCritical must transition immediately to SafeState");
static_assert(step_safety_fsm(BmsState::Operational, BmsEvent::AuthViolation)
                  == BmsState::SafeState,
              "AuthViolation must transition immediately to SafeState");
static_assert(step_safety_fsm(BmsState::SafeState, BmsEvent::NominalTick)
                  == BmsState::SafeState,
              "SafeState must remain latched on NominalTick");
static_assert(contactor_for_state(BmsState::SafeState) == 0U,
              "SafeState must command HV contactor OPEN");

} // namespace aegis::hil

#endif /* AEGIS_HVBMS_SAFETY_FSM_HPP */
