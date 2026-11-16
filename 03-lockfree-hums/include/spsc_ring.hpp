/* ========================================================================== *
 * AEGIS Critical Systems - Subsystem 03: Lock-Free HUMS & Edge Inference
 * File: 03-lockfree-hums/include/spsc_ring.hpp
 * Description: Wait-Free SPSC Ring Buffer (Acquire/Release + alignas(64))
 *              and RAII POSIX Shared Memory (shm_open / mmap) Transport.
 * Standard: ISO C++23 | POSIX.1b | -fno-exceptions -fno-rtti
 * ========================================================================== */

#ifndef AEGIS_SPSC_RING_HPP
#define AEGIS_SPSC_RING_HPP

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <fcntl.h>
#include <new>
#include <sys/mman.h>
#include <sys/stat.h>
#include <type_traits>
#include <unistd.h>

namespace aegis::hums
{

inline constexpr std::size_t kCacheLineBytes = 64U;

template <typename T>
struct alignas(kCacheLineBytes) CacheAlignedSlot
{
    T payload;
};

/*
 * Wait-free Single-Producer Single-Consumer (SPSC) ring buffer.
 * Capacity must be a power of 2 >= 2. Zero dynamic allocation.
 */
template <typename T, std::size_t Capacity>
class SpscRingBuffer
{
    static_assert(Capacity >= 2U && (Capacity & (Capacity - 1U)) == 0U,
                  "SPSC capacity must be a power of 2 >= 2");
    static_assert(std::is_trivially_copyable_v<T>,
                  "SPSC payload must be trivially copyable for SHM safety");

public:
    SpscRingBuffer() noexcept : head_(0U), tail_(0U), dropped_count_(0U), slots_() {}

    /*
     * @req SRS-HUMS-001
     * @fault DTC-0x301
     * @hazard MIL-882E-CAT-II
     */
    [[nodiscard]] bool try_push(const T &item) noexcept
    {
        const std::size_t cur_head = head_.load(std::memory_order_relaxed);
        const std::size_t next_head = (cur_head + 1U) & kIndexMask;
        const std::size_t cur_tail = tail_.load(std::memory_order_acquire);

        if (next_head == cur_tail)
        {
            dropped_count_.fetch_add(1U, std::memory_order_relaxed);
            return false;
        }
        slots_[cur_head].payload = item;
        head_.store(next_head, std::memory_order_release);
        return true;
    }

    [[nodiscard]] bool try_pop(T &out_item) noexcept
    {
        const std::size_t cur_tail = tail_.load(std::memory_order_relaxed);
        const std::size_t cur_head = head_.load(std::memory_order_acquire);

        if (cur_tail == cur_head)
        {
            return false;
        }
        out_item = slots_[cur_tail].payload;
        tail_.store((cur_tail + 1U) & kIndexMask, std::memory_order_release);
        return true;
    }

    [[nodiscard]] std::uint64_t dropped_count() const noexcept
    {
        return dropped_count_.load(std::memory_order_relaxed);
    }

    [[nodiscard]] bool is_empty() const noexcept
    {
        return head_.load(std::memory_order_acquire)
               == tail_.load(std::memory_order_acquire);
    }

private:
    static constexpr std::size_t kIndexMask = Capacity - 1U;

    alignas(kCacheLineBytes) std::atomic<std::size_t> head_;
    alignas(kCacheLineBytes) std::atomic<std::size_t> tail_;
    alignas(kCacheLineBytes) std::atomic<std::uint64_t> dropped_count_;
    alignas(kCacheLineBytes) std::array<CacheAlignedSlot<T>, Capacity> slots_;
};

/*
 * Deterministic RAII owner for POSIX shared memory ring buffers (shm_open/mmap).
 */
template <typename RingType>
class ShmRingRegion
{
public:
    ShmRingRegion() noexcept : fd_(-1), ptr_(nullptr), unlink_on_close_(false)
    {
        name_[0] = '\0';
    }

    ShmRingRegion(const ShmRingRegion &) = delete;
    ShmRingRegion &operator=(const ShmRingRegion &) = delete;

    ~ShmRingRegion() noexcept { close_region(); }

    [[nodiscard]] bool open_or_create(const char *shm_name, bool create_owner) noexcept
    {
        close_region();
        if (shm_name == nullptr || shm_name[0] != '/')
        {
            return false;
        }

        std::size_t len = 0U;
        while (shm_name[len] != '\0' && len < (sizeof(name_) - 1U))
        {
            name_[len] = shm_name[len];
            ++len;
        }
        name_[len] = '\0';

        const int oflags = create_owner ? (O_CREAT | O_RDWR) : O_RDWR;
        fd_ = shm_open(name_, oflags, S_IRUSR | S_IWUSR);
        if (fd_ < 0)
        {
            return false;
        }

        if (create_owner && ftruncate(fd_, static_cast<off_t>(sizeof(RingType))) != 0)
        {
            (void)close(fd_);
            fd_ = -1;
            return false;
        }

        void *mapped = mmap(nullptr,
                            sizeof(RingType),
                            PROT_READ | PROT_WRITE,
                            MAP_SHARED,
                            fd_,
                            0);
        if (mapped == MAP_FAILED)
        {
            (void)close(fd_);
            fd_ = -1;
            return false;
        }

        if (create_owner)
        {
            ptr_ = new (mapped) RingType();
            unlink_on_close_ = true;
        }
        else
        {
            ptr_ = static_cast<RingType *>(mapped);
            unlink_on_close_ = false;
        }
        return true;
    }

    void close_region() noexcept
    {
        if (ptr_ != nullptr)
        {
            (void)munmap(ptr_, sizeof(RingType));
            ptr_ = nullptr;
        }
        if (fd_ >= 0)
        {
            (void)close(fd_);
            fd_ = -1;
        }
        if (unlink_on_close_ && name_[0] != '\0')
        {
            (void)shm_unlink(name_);
            unlink_on_close_ = false;
        }
    }

    [[nodiscard]] RingType *ring() noexcept { return ptr_; }

private:
    int      fd_;
    RingType *ptr_;
    bool     unlink_on_close_;
    char     name_[64];
};

} // namespace aegis::hums

#endif /* AEGIS_SPSC_RING_HPP */
