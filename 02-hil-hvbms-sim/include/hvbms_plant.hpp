/* ========================================================================== *
 * AEGIS Critical Systems - Subsystem 02: HV-BMS & Propulsion HIL Simulator
 * File: 02-hil-hvbms-sim/include/hvbms_plant.hpp
 * Description: Fixed-Point 1 kHz HV-BMS Plant Model & MIL-STD-882E Injector
 * Standard: ISO C++23 | MISRA C++:2023 | DO-178C DAL-A
 * ========================================================================== */

#ifndef AEGIS_HVBMS_PLANT_HPP
#define AEGIS_HVBMS_PLANT_HPP

#include "aegis_gateway.h"
#include "hvbms_safety_fsm.hpp"
#include <cstdint>

namespace aegis::hil
{

enum class FaultMode : std::uint8_t
{
    None = 0U,
    ThermalRunaway = 1U,
    OverCurrentSurge = 2U,
    EmiBitFlip = 3U,
    AuthTagCorrupt = 4U
};

struct PlantConfig
{
    std::uint32_t ocv_mv;
    std::uint32_t internal_res_mohm;
    std::int32_t  nominal_load_ma;
    std::int16_t  ambient_temp_dc;
    std::int16_t  warn_temp_dc;
    std::int16_t  critical_temp_dc;
    std::uint8_t  reserved[2];
};

class HvBmsPlant
{
public:
    explicit HvBmsPlant(const PlantConfig &cfg) noexcept;

    void set_fault_mode(FaultMode mode) noexcept;
    void tick_1khz() noexcept;

    [[nodiscard]] int build_canfd_wire_frame(struct canfd_frame &out_frame,
                                             const std::uint8_t *key,
                                             std::size_t         key_len) noexcept;

    [[nodiscard]] BmsState      state() const noexcept { return state_; }
    [[nodiscard]] std::uint32_t pack_voltage_mv() const noexcept { return pack_voltage_mv_; }
    [[nodiscard]] std::int32_t  pack_current_ma() const noexcept { return pack_current_ma_; }
    [[nodiscard]] std::int16_t  cell_temp_dc() const noexcept { return cell_temp_dc_; }
    [[nodiscard]] std::uint32_t seq_counter() const noexcept { return seq_counter_; }
    [[nodiscard]] std::uint8_t  fault_mask() const noexcept { return fault_mask_; }

private:
    PlantConfig   cfg_;
    BmsState      state_;
    FaultMode     fault_mode_;
    std::uint32_t pack_voltage_mv_;
    std::int32_t  pack_current_ma_;
    std::int32_t  thermal_accum_mdk_; /* milli-deci-Kelvin accumulator */
    std::int16_t  cell_temp_dc_;
    std::uint32_t seq_counter_;
    std::uint8_t  fault_mask_;
    std::uint8_t  tick_phase_;
};

} // namespace aegis::hil

#endif /* AEGIS_HVBMS_PLANT_HPP */
