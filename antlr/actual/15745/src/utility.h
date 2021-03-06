#ifndef __UTILITY_INCLUDED__
#define __UTILITY_INCLUDED__

#ifdef __cplusplus
extern "C" {
#endif

#include <sys/time.h>
#include <stdint.h>

uint64_t
time_elapsed(struct timeval *start, struct timeval *end);

static inline uint16_t
fastrand(uint32_t *seed)
{
    *seed = (214013 * (*seed) + 2531011);
    return (*seed >> 16) & 0xFFFF;
}

static inline void
lfence()
{
    __asm__ __volatile("lfence" ::: "memory");
}

static inline void
sfence()
{
    __asm__ __volatile("sfence" ::: "memory");
}

static inline void
mfence()
{
    __asm__ __volatile("mfence" ::: "memory");
}

static inline void
compiler_fence()
{
    __asm__ __volatile("" ::: "memory");
}

#define expect_true(expr) __builtin_expect((expr), 1)
#define expect_false(expr) __builtin_expect((expr), 0)

#ifdef __cplusplus
}
#endif

#endif /* __UTILITY_H_INCLUDED__ */
