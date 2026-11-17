/* ========================================================================== *
 * AEGIS Critical Systems - Subsystem 03: Lock-Free HUMS & Edge Inference
 * File: 03-lockfree-hums/src/test_malloc_trap.cpp
 * Description: Empirical Verification that Post-Init Heap Allocation Raises
 *              SIGABRT via Linker Wrapping (--wrap=malloc/calloc/realloc).
 * Standard: ISO C++23 | POSIX.1-2008 | -fno-exceptions -fno-rtti
 * ========================================================================== */

#include <csignal>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>

extern "C" {
void aegis_hums_arm_alloc_trap(void);
void aegis_hums_disarm_alloc_trap(void);
}

namespace
{

enum class AllocKind
{
    Malloc,
    Calloc,
    Realloc
};

int expect_sigabrt_on_alloc(AllocKind kind) noexcept
{
    volatile std::size_t alloc_sz = 64U;
    void                *seed = nullptr;

    if (kind == AllocKind::Realloc)
    {
        seed = std::malloc(alloc_sz);
        if (seed == nullptr)
        {
            return 1;
        }
    }

    (void)std::fflush(nullptr);
    const pid_t pid = fork();
    if (pid < 0)
    {
        std::free(seed);
        return 2;
    }

    if (pid == 0)
    {
        const struct rlimit zero_core {0U, 0U};
        (void)setrlimit(RLIMIT_CORE, &zero_core);

        aegis_hums_arm_alloc_trap();
        void *volatile ptr = nullptr;
        switch (kind)
        {
        case AllocKind::Malloc:
            ptr = std::malloc(alloc_sz);
            break;
        case AllocKind::Calloc:
            ptr = std::calloc(1U, alloc_sz);
            break;
        case AllocKind::Realloc:
            ptr = std::realloc(seed, alloc_sz * 2U);
            break;
        }
        (void)ptr;
        aegis_hums_disarm_alloc_trap();
        _exit(0);
    }

    std::free(seed);
    int status = 0;
    if (waitpid(pid, &status, 0) != pid)
    {
        return 3;
    }
    if (!WIFSIGNALED(status) || WTERMSIG(status) != SIGABRT)
    {
        return 4;
    }
    return 0;
}

int test_disarmed_alloc_succeeds() noexcept
{
    volatile std::size_t alloc_sz = 64U;
    aegis_hums_disarm_alloc_trap();
    void *raw = std::malloc(alloc_sz);
    void *volatile sink = raw;
    (void)sink;
    if (raw == nullptr)
    {
        return 1;
    }
    std::free(raw);
    return 0;
}

} // namespace

int main()
{
    if (test_disarmed_alloc_succeeds() != 0)
    {
        std::fprintf(stderr, "[FAIL] test_disarmed_alloc_succeeds\n");
        return 1;
    }
    if (expect_sigabrt_on_alloc(AllocKind::Malloc) != 0)
    {
        std::fprintf(stderr, "[FAIL] expect_sigabrt_on_alloc(malloc)\n");
        return 1;
    }
    if (expect_sigabrt_on_alloc(AllocKind::Calloc) != 0)
    {
        std::fprintf(stderr, "[FAIL] expect_sigabrt_on_alloc(calloc)\n");
        return 1;
    }
    if (expect_sigabrt_on_alloc(AllocKind::Realloc) != 0)
    {
        std::fprintf(stderr, "[FAIL] expect_sigabrt_on_alloc(realloc)\n");
        return 1;
    }
    std::printf("[PASS] Post-Init --wrap={malloc,calloc,realloc} SIGABRT trap verified.\n");
    return 0;
}
