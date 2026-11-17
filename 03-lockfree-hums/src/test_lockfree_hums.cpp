/* ========================================================================== *
 * AEGIS Critical Systems - Subsystem 03: Lock-Free HUMS & Edge Inference
 * File: 03-lockfree-hums/src/test_lockfree_hums.cpp
 * Description: Multi-Threaded SPSC Stress, POSIX SHM Transport & Q15 EKF Test
 * Standard: ISO C++23 | POSIX.1b | -fno-exceptions -fno-rtti
 * ========================================================================== */

#include "aegis_shm_bus.h"
#include "q15_ekf.hpp"
#include "spsc_ring.hpp"
#include <atomic>
#include <cstdio>
#include <thread>
#include <unistd.h>

extern "C" {
void aegis_hums_arm_alloc_trap(void);
void aegis_hums_disarm_alloc_trap(void);
}

namespace
{

using TelemetryRing = aegis::hums::SpscRingBuffer<t_aegis_telemetry_data, 256U>;

alignas(64) TelemetryRing g_thread_ring;

int test_multithreaded_spsc_fifo() noexcept
{
    constexpr std::uint32_t kTotalFrames = 50000U;
    std::uint32_t           rx_count = 0U;
    int                     order_error = 0;

    std::thread producer([]() noexcept {
        for (std::uint32_t seq = 0U; seq < kTotalFrames; ++seq)
        {
            const t_aegis_telemetry_data frame {
                400000U + seq,
                static_cast<std::int32_t>(seq),
                300,
                2U,
                0U
            };
            while (!g_thread_ring.try_push(frame))
            {
                std::this_thread::yield();
            }
        }
    });

    while (rx_count < kTotalFrames)
    {
        t_aegis_telemetry_data frame {};
        if (g_thread_ring.try_pop(frame))
        {
            if (frame.pack_voltage_mv != (400000U + rx_count)
                || frame.pack_current_ma != static_cast<std::int32_t>(rx_count))
            {
                order_error = 1;
            }
            ++rx_count;
        }
        else
        {
            std::this_thread::yield();
        }
    }

    producer.join();
    if (order_error != 0 || !g_thread_ring.is_empty())
    {
        return 1;
    }
    return 0;
}

int test_posix_shm_ring_roundtrip() noexcept
{
    char shm_name[64] {};
    (void)std::snprintf(shm_name,
                        sizeof(shm_name),
                        "/aegis_hums_test_%ld",
                        static_cast<long>(getpid()));

    aegis::hums::ShmRingRegion<TelemetryRing> writer_region;
    aegis::hums::ShmRingRegion<TelemetryRing> reader_region;

    if (!writer_region.open_or_create(shm_name, true))
    {
        return 1;
    }
    if (!reader_region.open_or_create(shm_name, false))
    {
        return 2;
    }

    const t_aegis_telemetry_data tx {395500U, 42000, 345, 2U, 0x01U};
    if (!writer_region.ring()->try_push(tx))
    {
        return 3;
    }

    t_aegis_telemetry_data rx {};
    if (!reader_region.ring()->try_pop(rx))
    {
        return 4;
    }
    if (rx.pack_voltage_mv != 395500U || rx.max_cell_temp_dc != 345
        || rx.fault_mask != 0x01U)
    {
        return 5;
    }
    return 0;
}

int test_q15_anomaly_inference() noexcept
{
    const aegis::hums::Q15FilterConfig cfg {
        6554,   /* K = 0.20 in Q15 */
        3277,   /* ~10% innovation limit */
        400000, /* 400.0 V nominal */
        50000,  /* +/-50.0 V full scale */
        250,    /* 25.0 C nominal */
        400     /* +/-40.0 C full scale */
    };
    aegis::hums::Q15AnomalyFilter filter(cfg);

    aegis_hums_arm_alloc_trap();
    /* Steady nominal samples -> zero anomalies */
    for (int i = 0; i < 10; ++i)
    {
        const t_aegis_telemetry_data nom {400000U, 30000, 255, 2U, 0U};
        const auto out = filter.step(nom);
        if (out.anomaly_detected != 0U)
        {
            aegis_hums_disarm_alloc_trap();
            return 1;
        }
    }

    /* Sudden thermal runaway spike (25.5 C -> 55.0 C) -> triggers DTC-0x302 */
    const t_aegis_telemetry_data spike {400000U, 30000, 550, 2U, 0x01U};
    const auto out = filter.step(spike);
    aegis_hums_disarm_alloc_trap();
    if (out.anomaly_detected != 1U || out.dtc_code != aegis::hums::kDtcHumsAnomaly)
    {
        return 2;
    }
    return 0;
}

int test_spsc_ring_overrun_saturation() noexcept
{
    using SmallRing = aegis::hums::SpscRingBuffer<t_aegis_telemetry_data, 4U>;
    SmallRing ring;

    if (!ring.is_empty())
    {
        return 1;
    }
    if (ring.dropped_count() != 0U)
    {
        return 2;
    }

    const t_aegis_telemetry_data item1 {400000U, 1000, 250, 2U, 0U};
    const t_aegis_telemetry_data item2 {400010U, 1010, 251, 2U, 0U};
    const t_aegis_telemetry_data item3 {400020U, 1020, 252, 2U, 0U};
    const t_aegis_telemetry_data item4 {400030U, 1030, 253, 2U, 0U};

    if (!ring.try_push(item1))
        return 3;
    if (!ring.try_push(item2))
        return 4;
    if (!ring.try_push(item3))
        return 5;

    /* Buffer capacity=4 has 3 usable slots. Push item4 must fail & increment dropped_count */
    if (ring.try_push(item4))
        return 6;
    if (ring.dropped_count() != 1U)
        return 7;

    t_aegis_telemetry_data out {};
    if (!ring.try_pop(out) || out.pack_voltage_mv != 400000U)
        return 8;
    if (!ring.try_pop(out) || out.pack_voltage_mv != 400010U)
        return 9;
    if (!ring.try_pop(out) || out.pack_voltage_mv != 400020U)
        return 10;
    if (ring.try_pop(out))
        return 11;
    if (!ring.is_empty())
        return 12;

    if (!ring.try_push(item4))
        return 13;
    if (!ring.try_pop(out) || out.pack_voltage_mv != 400030U)
        return 14;

    return 0;
}

int test_seqlock_shm_concurrent_reader_writer() noexcept
{
    alignas(64) static t_aegis_tmtc_shm shared_bus {};
    std::atomic<bool>                   done {false};
    std::atomic<int>                    torn_error {0};

    constexpr std::uint32_t kIterations = 50000U;
    std::thread             reader([&]() noexcept {
        while (!done.load(std::memory_order_acquire))
        {
            t_aegis_telemetry_data tel {};
            std::uint16_t          mode = 0U;
            std::uint16_t          dtc = 0U;
            std::uint64_t          ts = 0ULL;

            const int rc = aegis_shm_read_snapshot(&shared_bus, &tel, &mode, &dtc, &ts);
            if (rc == 0 && ts != 0ULL)
            {
                const std::uint32_t tick = static_cast<std::uint32_t>(ts / 1000ULL);
                if (tel.pack_voltage_mv != (400000U + tick)
                    || tel.pack_current_ma != static_cast<std::int32_t>(tick)
                    || tel.max_cell_temp_dc != static_cast<std::int16_t>(250U + (tick % 200U))
                    || mode != static_cast<std::uint16_t>(tick % 3U)
                    || dtc != static_cast<std::uint16_t>(tick & 0xFFFFU))
                {
                    torn_error.store(1, std::memory_order_release);
                    break;
                }
            }
        }
    });

    for (std::uint32_t tick = 1U; tick <= kIterations; ++tick)
    {
        const t_aegis_telemetry_data tel {
            400000U + tick,
            static_cast<std::int32_t>(tick),
            static_cast<std::int16_t>(250U + (tick % 200U)),
            2U,
            0U
        };
        aegis_shm_publish(&shared_bus,
                          &tel,
                          static_cast<std::uint16_t>(tick % 3U),
                          static_cast<std::uint16_t>(tick & 0xFFFFU),
                          static_cast<std::uint64_t>(tick) * 1000ULL);
    }

    done.store(true, std::memory_order_release);
    reader.join();
    return torn_error.load(std::memory_order_acquire);
}

} // namespace

int main()
{
    int rc = test_spsc_ring_overrun_saturation();
    if (rc != 0)
    {
        std::fprintf(stderr, "[FAIL] test_spsc_ring_overrun_saturation: %d\n", rc);
        return 1;
    }
    rc = test_multithreaded_spsc_fifo();
    if (rc != 0)
    {
        std::fprintf(stderr, "[FAIL] test_multithreaded_spsc_fifo: %d\n", rc);
        return 1;
    }
    rc = test_posix_shm_ring_roundtrip();
    if (rc != 0)
    {
        std::fprintf(stderr, "[FAIL] test_posix_shm_ring_roundtrip: %d\n", rc);
        return 1;
    }
    rc = test_q15_anomaly_inference();
    if (rc != 0)
    {
        std::fprintf(stderr, "[FAIL] test_q15_anomaly_inference: %d\n", rc);
        return 1;
    }
    rc = test_seqlock_shm_concurrent_reader_writer();
    if (rc != 0)
    {
        std::fprintf(stderr, "[FAIL] test_seqlock_shm_concurrent_reader_writer: %d\n", rc);
        return 1;
    }
    std::printf("[PASS] Wait-Free SPSC Ring, POSIX SHM Seqlock & Q15 Fixed-Point EKF verified.\n");
    return 0;
}
