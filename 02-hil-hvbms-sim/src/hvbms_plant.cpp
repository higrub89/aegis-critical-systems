/* ========================================================================== *
 * AEGIS Critical Systems - Subsystem 02: HV-BMS & Propulsion HIL Simulator
 * File: 02-hil-hvbms-sim/src/hvbms_plant.cpp
 * Description: Fixed-Point HV-BMS Physics Model & MIL-STD-882E Injector
 * Standard: ISO C++23 | MISRA C++:2023 | DO-178C DAL-A
 * ========================================================================== */

#include "hvbms_plant.hpp"
#include <cstring>

namespace aegis::hil
{

namespace
{

void encode_u32_le(std::uint8_t *dst, std::uint32_t val) noexcept
{
    dst[0] = static_cast<std::uint8_t>(val & 0xFFU);
    dst[1] = static_cast<std::uint8_t>((val >> 8U) & 0xFFU);
    dst[2] = static_cast<std::uint8_t>((val >> 16U) & 0xFFU);
    dst[3] = static_cast<std::uint8_t>((val >> 24U) & 0xFFU);
}

void encode_i16_le(std::uint8_t *dst, std::int16_t val) noexcept
{
    const auto u = static_cast<std::uint16_t>(val);
    dst[0] = static_cast<std::uint8_t>(u & 0xFFU);
    dst[1] = static_cast<std::uint8_t>((u >> 8U) & 0xFFU);
}

} // namespace

HvBmsPlant::HvBmsPlant(const PlantConfig &cfg) noexcept
    : cfg_(cfg),
      state_(BmsState::Init),
      fault_mode_(FaultMode::None),
      pack_voltage_mv_(cfg.ocv_mv),
      pack_current_ma_(0),
      thermal_accum_mdk_(static_cast<std::int32_t>(cfg.ambient_temp_dc) * 1000),
      cell_temp_dc_(cfg.ambient_temp_dc),
      seq_counter_(0U),
      fault_mask_(0U),
      tick_phase_(0U)
{
}

void HvBmsPlant::set_fault_mode(FaultMode mode) noexcept
{
    fault_mode_ = mode;
}

/*
 * @req SRS-HIL-002
 * @fault DTC-0x202
 * @hazard MIL-882E-CAT-I
 */
void HvBmsPlant::tick_1khz() noexcept
{
    BmsEvent ev = BmsEvent::NominalTick;

    if (state_ == BmsState::Init)
    {
        ev = BmsEvent::SelfTestOk;
    }
    else if (state_ == BmsState::Precharge)
    {
        tick_phase_ = static_cast<std::uint8_t>(tick_phase_ + 1U);
        if (tick_phase_ >= 2U)
        {
            ev = BmsEvent::PrechargeDone;
        }
    }

    if (state_ == BmsState::Operational || state_ == BmsState::Degraded)
    {
        pack_current_ma_ = cfg_.nominal_load_ma;
        if (fault_mode_ == FaultMode::OverCurrentSurge)
        {
            pack_current_ma_ = cfg_.nominal_load_ma * 6;
            fault_mask_ |= 0x02U;
        }
    }
    else
    {
        pack_current_ma_ = 0;
    }

    /* Fixed-point voltage sag: V_pack = V_ocv - (I_ma * R_mohm) / 1000 */
    const std::int64_t sag_mv =
        (static_cast<std::int64_t>(pack_current_ma_)
         * static_cast<std::int64_t>(cfg_.internal_res_mohm))
        / 1000LL;
    const std::int64_t v_calc = static_cast<std::int64_t>(cfg_.ocv_mv) - sag_mv;
    pack_voltage_mv_ = (v_calc > 0LL) ? static_cast<std::uint32_t>(v_calc) : 0U;

    /* Fixed-point thermal derivative (milli-deci-C per 1 ms tick) */
    std::int32_t dtemp_mdk = (pack_current_ma_ > 0) ? 2 : -1;
    if (fault_mode_ == FaultMode::ThermalRunaway)
    {
        dtemp_mdk = 2500; /* +2.5 dC/ms runaway slope */
        fault_mask_ |= 0x01U;
    }
    thermal_accum_mdk_ += dtemp_mdk;
    const std::int32_t ambient_mdk =
        static_cast<std::int32_t>(cfg_.ambient_temp_dc) * 1000;
    if (thermal_accum_mdk_ < ambient_mdk)
    {
        thermal_accum_mdk_ = ambient_mdk;
    }
    cell_temp_dc_ = static_cast<std::int16_t>(thermal_accum_mdk_ / 1000);

    if (cell_temp_dc_ >= cfg_.critical_temp_dc
        || fault_mode_ == FaultMode::OverCurrentSurge)
    {
        ev = BmsEvent::FaultCritical;
    }
    else if (cell_temp_dc_ >= cfg_.warn_temp_dc)
    {
        ev = BmsEvent::OverTempWarn;
    }

    state_ = step_safety_fsm(state_, ev);
    if (state_ == BmsState::SafeState)
    {
        pack_current_ma_ = 0;
        pack_voltage_mv_ = cfg_.ocv_mv;
    }
}

int HvBmsPlant::build_canfd_wire_frame(struct canfd_frame &out_frame,
                                       const std::uint8_t *key,
                                       std::size_t         key_len) noexcept
{
    AEGIS_ALIGNAS(AEGIS_CACHELINE_SIZE) t_aegis_frame_ctx tmp_ctx {};

    if (key == nullptr || key_len == 0U)
    {
        return -1;
    }
    if (aegis_frame_ctx_init(&tmp_ctx) != 0)
    {
        return -1;
    }

    seq_counter_ += 1U;
    tmp_ctx.raw_frame.can_id = 0x120U;
    tmp_ctx.raw_frame.len = AEGIS_TELEMETRY_LEN;
    tmp_ctx.seq_counter = seq_counter_;

    encode_u32_le(&tmp_ctx.raw_frame.data[0], pack_voltage_mv_);
    encode_u32_le(&tmp_ctx.raw_frame.data[4],
                  static_cast<std::uint32_t>(pack_current_ma_));
    encode_i16_le(&tmp_ctx.raw_frame.data[8], cell_temp_dc_);
    tmp_ctx.raw_frame.data[10] = contactor_for_state(state_);
    tmp_ctx.raw_frame.data[11] = fault_mask_;

    if (aegis_rust_compute_hmac(&tmp_ctx, key, key_len) != 0)
    {
        return -1;
    }

    std::memset(&out_frame, 0, sizeof(out_frame));
    out_frame.can_id = tmp_ctx.raw_frame.can_id;
    out_frame.len = AEGIS_WIRE_FRAME_LEN;
    std::memcpy(&out_frame.data[0], &tmp_ctx.raw_frame.data[0], AEGIS_TELEMETRY_LEN);
    encode_u32_le(&out_frame.data[12], seq_counter_);
    std::memcpy(&out_frame.data[16], tmp_ctx.hmac_tag, AEGIS_HMAC_TAG_LEN);

    /* MIL-STD-882E wire-level fault injection after HMAC signing */
    if (fault_mode_ == FaultMode::EmiBitFlip)
    {
        out_frame.data[0] ^= 0x04U;
    }
    else if (fault_mode_ == FaultMode::AuthTagCorrupt)
    {
        out_frame.data[47] ^= 0xFFU;
    }

    return 0;
}

} // namespace aegis::hil
