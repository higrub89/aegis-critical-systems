/* ========================================================================== *
 * AEGIS Critical Systems - Subsystem 03: Lock-Free HUMS & Edge Inference
 * File: 03-lockfree-hums/src/main_hums_bench.cpp
 * Description: Hardware Latency Harness (CLOCK_MONOTONIC_RAW) over Static
 *              .bss SPSC Ring + Q15 EKF (Zero Post-Init Heap Allocation).
 * Standard: ISO C++23 | POSIX.1b | -fno-exceptions -fno-rtti
 * ========================================================================== */

#include "q15_ekf.hpp"
#include "spsc_ring.hpp"
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <time.h>

namespace
{

constexpr std::size_t kSamples = 10000U;

std::uint64_t raw_monotonic_ns() noexcept
{
    struct timespec ts {};
    if (clock_gettime(CLOCK_MONOTONIC_RAW, &ts) != 0)
    {
        return 0ULL;
    }
    return (static_cast<std::uint64_t>(ts.tv_sec) * 1000000000ULL)
           + static_cast<std::uint64_t>(ts.tv_nsec);
}

alignas(64) aegis::hums::SpscRingBuffer<t_aegis_telemetry_data, 1024U> g_ring;
alignas(64) std::array<std::uint64_t, kSamples> g_latencies_ns {};

} // namespace

int main()
{
    const aegis::hums::Q15FilterConfig cfg {
        6554,   /* K = 0.20 in Q15 */
        4096,   /* ~12.5% innovation anomaly limit */
        400000, /* 400 V nominal */
        50000,  /* +/-50 V full scale */
        300,    /* 30.0 C nominal */
        400     /* +/-40.0 C full scale */
    };
    aegis::hums::Q15AnomalyFilter filter(cfg);

    std::uint64_t sum_ns = 0ULL;
    std::uint64_t anomalies = 0ULL;

    for (std::size_t i = 0U; i < kSamples; ++i)
    {
        const t_aegis_telemetry_data sample {
            static_cast<std::uint32_t>(400000U - ((i & 3U) * 100U)),
            50000,
            305,
            2U,
            0U
        };
        t_aegis_telemetry_data popped {};

        const std::uint64_t t0 = raw_monotonic_ns();
        (void)g_ring.try_push(sample);
        if (g_ring.try_pop(popped))
        {
            const auto out = filter.step(popped);
            anomalies += out.anomaly_detected;
        }
        const std::uint64_t dt = raw_monotonic_ns() - t0;
        g_latencies_ns[i] = dt;
        sum_ns += dt;
    }

    std::sort(g_latencies_ns.begin(), g_latencies_ns.end());
    const std::uint64_t p50 = g_latencies_ns[(kSamples * 50U) / 100U];
    const std::uint64_t p99 = g_latencies_ns[(kSamples * 99U) / 100U];
    const std::uint64_t pmax = g_latencies_ns[kSamples - 1U];

    std::printf(
        "[AEGIS-HUMS-BENCH] N=%zu | avg=%llu ns | p50=%llu ns | p99=%llu ns | max=%llu ns | anomalies=%llu\n",
        kSamples,
        static_cast<unsigned long long>(sum_ns / kSamples),
        static_cast<unsigned long long>(p50),
        static_cast<unsigned long long>(p99),
        static_cast<unsigned long long>(pmax),
        static_cast<unsigned long long>(anomalies));
    return 0;
}
