/* ========================================================================== *
 * AEGIS Critical Systems - Subsystem 03: Lock-Free HUMS & Edge Inference
 * File: 03-lockfree-hums/src/q15_ekf.cpp
 * Description: Deterministic Zero-Heap Q15 Fixed-Point Kalman Anomaly Engine
 * Standard: ISO C++23 | MISRA C++:2023 | -fno-exceptions -fno-rtti
 * ========================================================================== */

#include "q15_ekf.hpp"

namespace aegis::hums
{

std::int16_t q15_clamp(std::int32_t val) noexcept
{
    if (val > 32767)
    {
        return 32767;
    }
    if (val < -32768)
    {
        return -32768;
    }
    return static_cast<std::int16_t>(val);
}

std::int16_t q15_mul(std::int16_t a, std::int16_t b) noexcept
{
    const std::int32_t prod =
        (static_cast<std::int32_t>(a) * static_cast<std::int32_t>(b)) + 16384;
    return q15_clamp(prod >> 15);
}

namespace
{

std::int16_t normalize_to_q15(std::int32_t raw,
                              std::int32_t nominal,
                              std::int32_t scale) noexcept
{
    if (scale <= 0)
    {
        return 0;
    }
    const std::int64_t diff = static_cast<std::int64_t>(raw - nominal) * 32767LL;
    return q15_clamp(static_cast<std::int32_t>(diff / scale));
}

std::int16_t abs_q15(std::int16_t v) noexcept
{
    if (v == -32768)
    {
        return 32767;
    }
    return (v < 0) ? static_cast<std::int16_t>(-v) : v;
}

} // namespace

Q15AnomalyFilter::Q15AnomalyFilter(const Q15FilterConfig &cfg) noexcept
    : cfg_(cfg), est_voltage_q15_(0), est_temp_q15_(0), initialized_(0U), reserved_{}
{
}

void Q15AnomalyFilter::reset() noexcept
{
    est_voltage_q15_ = 0;
    est_temp_q15_ = 0;
    initialized_ = 0U;
}

/*
 * @req SRS-HUMS-002
 * @fault DTC-0x302
 * @hazard MIL-882E-CAT-II
 */
Q15InferenceOutput Q15AnomalyFilter::step(const t_aegis_telemetry_data &sample) noexcept
{
    Q15InferenceOutput out {};

    const std::int16_t z_volt_q15 =
        normalize_to_q15(static_cast<std::int32_t>(sample.pack_voltage_mv),
                         cfg_.nominal_voltage_mv,
                         cfg_.voltage_scale_mv);
    const std::int16_t z_temp_q15 =
        normalize_to_q15(static_cast<std::int32_t>(sample.max_cell_temp_dc),
                         static_cast<std::int32_t>(cfg_.nominal_temp_dc),
                         static_cast<std::int32_t>(cfg_.temp_scale_dc));

    if (initialized_ == 0U)
    {
        est_voltage_q15_ = z_volt_q15;
        est_temp_q15_ = z_temp_q15;
        initialized_ = 1U;
    }

    const std::int16_t innov_v =
        q15_clamp(static_cast<std::int32_t>(z_volt_q15) - est_voltage_q15_);
    const std::int16_t innov_t =
        q15_clamp(static_cast<std::int32_t>(z_temp_q15) - est_temp_q15_);

    const std::int16_t corr_v = q15_mul(cfg_.kalman_gain_q15, innov_v);
    const std::int16_t corr_t = q15_mul(cfg_.kalman_gain_q15, innov_t);

    est_voltage_q15_ = q15_clamp(static_cast<std::int32_t>(est_voltage_q15_) + corr_v);
    est_temp_q15_ = q15_clamp(static_cast<std::int32_t>(est_temp_q15_) + corr_t);

    out.filtered_voltage_q15 = est_voltage_q15_;
    out.filtered_temp_q15 = est_temp_q15_;
    out.voltage_innovation_q15 = innov_v;
    out.temp_innovation_q15 = innov_t;

    if (abs_q15(innov_v) > cfg_.innovation_limit_q15
        || abs_q15(innov_t) > cfg_.innovation_limit_q15)
    {
        out.anomaly_detected = 1U;
        out.dtc_code = kDtcHumsAnomaly;
    }
    else
    {
        out.anomaly_detected = 0U;
        out.dtc_code = 0U;
    }

    return out;
}

} // namespace aegis::hums
