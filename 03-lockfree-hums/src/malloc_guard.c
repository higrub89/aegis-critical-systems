/* ========================================================================== *
 * AEGIS Critical Systems - Subsystem 03: Lock-Free HUMS & Edge Inference
 * File: 03-lockfree-hums/src/malloc_guard.c
 * Description: Linker-Wrapped Post-Init Dynamic Allocation Trap (--wrap=malloc)
 *              Enforces SIGABRT if heap allocation occurs on critical path.
 * Standard: ISO/IEC 9899:2018 (C17)
 * ========================================================================== */

#include <stddef.h>
#include <stdlib.h>

void *__real_malloc(size_t size);
void *__real_calloc(size_t nmemb, size_t size);
void *__real_realloc(void *ptr, size_t size);

void *__wrap_malloc(size_t size);
void *__wrap_calloc(size_t nmemb, size_t size);
void *__wrap_realloc(void *ptr, size_t size);

void aegis_hums_arm_alloc_trap(void);
void aegis_hums_disarm_alloc_trap(void);

static int g_alloc_trap_armed = 0;

void aegis_hums_arm_alloc_trap(void)
{
    __atomic_store_n(&g_alloc_trap_armed, 1, __ATOMIC_SEQ_CST);
}

void aegis_hums_disarm_alloc_trap(void)
{
    __atomic_store_n(&g_alloc_trap_armed, 0, __ATOMIC_SEQ_CST);
}

void *__wrap_malloc(size_t size)
{
    if (__atomic_load_n(&g_alloc_trap_armed, __ATOMIC_SEQ_CST) != 0)
        abort();
    return (__real_malloc(size));
}

void *__wrap_calloc(size_t nmemb, size_t size)
{
    if (__atomic_load_n(&g_alloc_trap_armed, __ATOMIC_SEQ_CST) != 0)
        abort();
    return (__real_calloc(nmemb, size));
}

void *__wrap_realloc(void *ptr, size_t size)
{
    if (__atomic_load_n(&g_alloc_trap_armed, __ATOMIC_SEQ_CST) != 0)
        abort();
    return (__real_realloc(ptr, size));
}
