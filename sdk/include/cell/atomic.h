/* cell/atomic.h -- PPU atomic operations over PowerPC lwarx/stwcx reservation.
 *
 * All operations are static-inline wrappers over the GCC builtins
 * __lwarx / __stwcx (32-bit) and __ldarx / __stdcx (64-bit) provided
 * by <ppu_intrinsics.h>.  The lwarx ... modify ... stwcx retry loop is
 * the standard PowerPC atomic idiom (ISA Book II, Section 4.4.2).
 *
 * Memory ordering:
 *   - Publishing stores / RMW (Add/Sub/And/Or/Incr/Decr/Store/
 *     TestAndDecr): lwsync (release barrier) before the lwarx loop
 *     ensures prior stores are visible before the atomic update.
 *   - CompareAndSwap: sync (full barrier) before lwarx + isync
 *     (acquire) on the success path only.
 *   - LockLine / StoreConditional: bare primitives, no barriers;
 *     callers compose their own fencing.
 *
 * SPU side is deferred -- this header is PPU-only for now.
 */
#ifndef __CELL_ATOMIC_H__
#define __CELL_ATOMIC_H__

#if defined(__SPU__)
#error "cell/atomic.h not yet shipped for SPU -- use cell/sync/ primitives or SPU MFC atomics"
#endif

#if !defined(__SPU__)

#include <stdint.h>
#include <ppu_intrinsics.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---------- 32-bit operations (lwarx / stwcx) ---------- */

static inline uint32_t
cellAtomicLockLine32(const volatile uint32_t *ea)
{
    return __lwarx(ea);
}

static inline int
cellAtomicStoreConditional32(volatile uint32_t *ea, uint32_t value)
{
    return __stwcx(ea, value) == 0;
}

static inline uint32_t
cellAtomicAdd32(volatile uint32_t *ea, uint32_t value)
{
    uint32_t old;
    __asm__ volatile("lwsync" ::: "memory");
    do {
        old = __lwarx(ea);
    } while (!__stwcx(ea, old + value));
    return old;
}

static inline uint32_t
cellAtomicSub32(volatile uint32_t *ea, uint32_t value)
{
    uint32_t old;
    __asm__ volatile("lwsync" ::: "memory");
    do {
        old = __lwarx(ea);
    } while (!__stwcx(ea, old - value));
    return old;
}

static inline uint32_t
cellAtomicAnd32(volatile uint32_t *ea, uint32_t value)
{
    uint32_t old;
    __asm__ volatile("lwsync" ::: "memory");
    do {
        old = __lwarx(ea);
    } while (!__stwcx(ea, old & value));
    return old;
}

static inline uint32_t
cellAtomicOr32(volatile uint32_t *ea, uint32_t value)
{
    uint32_t old;
    __asm__ volatile("lwsync" ::: "memory");
    do {
        old = __lwarx(ea);
    } while (!__stwcx(ea, old | value));
    return old;
}

static inline uint32_t
cellAtomicStore32(volatile uint32_t *ea, uint32_t value)
{
    uint32_t old;
    __asm__ volatile("lwsync" ::: "memory");
    do {
        old = __lwarx(ea);
    } while (!__stwcx(ea, value));
    return old;
}

static inline uint32_t
cellAtomicIncr32(volatile uint32_t *ea)
{
    uint32_t old;
    __asm__ volatile("lwsync" ::: "memory");
    do {
        old = __lwarx(ea);
    } while (!__stwcx(ea, old + 1));
    return old;
}

static inline uint32_t
cellAtomicDecr32(volatile uint32_t *ea)
{
    uint32_t old;
    __asm__ volatile("lwsync" ::: "memory");
    do {
        old = __lwarx(ea);
    } while (!__stwcx(ea, old - 1));
    return old;
}

static inline uint32_t
cellAtomicNop32(volatile uint32_t *ea)
{
    return cellAtomicLockLine32(ea);
}

static inline uint32_t
cellAtomicTestAndDecr32(volatile uint32_t *ea)
{
    uint32_t old;
    __asm__ volatile("lwsync" ::: "memory");
    do {
        old = __lwarx(ea);
        if (old == 0)
            break;
    } while (!__stwcx(ea, old - 1));
    return old;
}

static inline uint32_t
cellAtomicCompareAndSwap32(volatile uint32_t *ea,
                           uint32_t compare, uint32_t swap)
{
    uint32_t old;
    __asm__ volatile("sync" ::: "memory");
    do {
        old = __lwarx(ea);
        if (old != compare)
            break;
    } while (!__stwcx(ea, swap));
    if (old == compare)
        __asm__ volatile("isync" ::: "memory");
    return old;
}

/* ---------- 64-bit operations (ldarx / stdcx, PowerPC 64) ---------- */

#ifdef __PPU__

static inline uint64_t
cellAtomicLockLine64(const volatile uint64_t *ea)
{
    return __ldarx(ea);
}

static inline int
cellAtomicStoreConditional64(volatile uint64_t *ea, uint64_t value)
{
    return __stdcx(ea, value) == 0;
}

static inline uint64_t
cellAtomicAdd64(volatile uint64_t *ea, uint64_t value)
{
    uint64_t old;
    __asm__ volatile("lwsync" ::: "memory");
    do {
        old = __ldarx(ea);
    } while (!__stdcx(ea, old + value));
    return old;
}

static inline uint64_t
cellAtomicSub64(volatile uint64_t *ea, uint64_t value)
{
    uint64_t old;
    __asm__ volatile("lwsync" ::: "memory");
    do {
        old = __ldarx(ea);
    } while (!__stdcx(ea, old - value));
    return old;
}

static inline uint64_t
cellAtomicAnd64(volatile uint64_t *ea, uint64_t value)
{
    uint64_t old;
    __asm__ volatile("lwsync" ::: "memory");
    do {
        old = __ldarx(ea);
    } while (!__stdcx(ea, old & value));
    return old;
}

static inline uint64_t
cellAtomicOr64(volatile uint64_t *ea, uint64_t value)
{
    uint64_t old;
    __asm__ volatile("lwsync" ::: "memory");
    do {
        old = __ldarx(ea);
    } while (!__stdcx(ea, old | value));
    return old;
}

static inline uint64_t
cellAtomicStore64(volatile uint64_t *ea, uint64_t value)
{
    uint64_t old;
    __asm__ volatile("lwsync" ::: "memory");
    do {
        old = __ldarx(ea);
    } while (!__stdcx(ea, value));
    return old;
}

static inline uint64_t
cellAtomicIncr64(volatile uint64_t *ea)
{
    uint64_t old;
    __asm__ volatile("lwsync" ::: "memory");
    do {
        old = __ldarx(ea);
    } while (!__stdcx(ea, old + 1));
    return old;
}

static inline uint64_t
cellAtomicDecr64(volatile uint64_t *ea)
{
    uint64_t old;
    __asm__ volatile("lwsync" ::: "memory");
    do {
        old = __ldarx(ea);
    } while (!__stdcx(ea, old - 1));
    return old;
}

static inline uint64_t
cellAtomicNop64(volatile uint64_t *ea)
{
    return cellAtomicLockLine64(ea);
}

static inline uint64_t
cellAtomicTestAndDecr64(volatile uint64_t *ea)
{
    uint64_t old;
    __asm__ volatile("lwsync" ::: "memory");
    do {
        old = __ldarx(ea);
        if (old == 0)
            break;
    } while (!__stdcx(ea, old - 1));
    return old;
}

static inline uint64_t
cellAtomicCompareAndSwap64(volatile uint64_t *ea,
                           uint64_t compare, uint64_t swap)
{
    uint64_t old;
    __asm__ volatile("sync" ::: "memory");
    do {
        old = __ldarx(ea);
        if (old != compare)
            break;
    } while (!__stdcx(ea, swap));
    if (old == compare)
        __asm__ volatile("isync" ::: "memory");
    return old;
}

#endif /* __PPU__ */

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* !__SPU__ */

#endif /* __CELL_ATOMIC_H__ */
