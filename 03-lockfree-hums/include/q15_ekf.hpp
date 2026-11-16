/* ========================================================================== *
 * AEGIS Critical Systems - Subsystem 03: Lock-Free HUMS & Edge Inference
 * File: 03-lockfree-hums/include/q15_ekf.hpp
 * Description: Zero-Heap Q15 (int16_t) Fixed-Point Kalman Anomaly Filter
 * Standard: ISO C++23 | MISRA C++:2023 | -fno-exceptions -fno-rtti
 * ========================================================================== */

#ifndef AEGIS_Q15_EKF_HPP
#define AEGIS_Q15_EKF_HPP

#include "aegis_frame_abi.h"
#include <cstdint>

namespace aegis::hums
{

inline constexpr std::uint16_t kDtcHumsRingOverrun = 0x0301U;
inline constexpr std::uint16_t kDtcHumsAnomaly = 0x0302U;

struct Q15FilterConfig
{
    std::int16_t kalman_gain_q15;      /* e.g. 6554 (~0.20 in Q15) */
    std::int16_t innovation_limit_q15; /* anomaly trip threshold in Q15 */
    std::int32_t nominal_voltage_mv;
    std::int32_t voltage_scale_mv;
    std::int16_t nominal_temp_dc;
    std::int16_t temp_scale_dc;
};

struct Q15InferenceOutput
{
    std::int16_t  filtered_voltage_q15;
    std::int16_t  filtered_temp_q15;
    std::int16_t  voltage_innovation_q15;
    std::int16_t  temp_innovation_q15;
    std::uint16_t dtc_code;
    std::uint8_t  anomaly_detected;
    std::uint8_t  reserved;
};

class Q15AnomalyFilter
{
public:
    explicit Q15AnomalyFilter(const Q15FilterConfig &cfg) noexcept;

    void reset() noexcept;

    [[nodiscard]] Q15InferenceOutput step(const t_aegis_telemetry_data &sample) noexcept;

private:
    Q15FilterConfig cfg_;
    std::int16_t    est_voltage_q15_;
    std::int16_t    est_temp_q15_;
    std::uint8_t    initialized_;
    std::uint8_t    reserved_[3];
};

[[nodiscard]] std::int16_t q15_clamp(std::int32_t val) noexcept;
[[nodiscard]] std::int16_t q15_mul(std::int16_t a, std::int16_t b) noexcept;

} // namespace aegis::hums

#endif /* AEGIS_Q15_EKF_HPP */
