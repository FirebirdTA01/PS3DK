/* cell/atomic.h -- SPU-side atomic operations over MFC reservation.
 *
 * All operations are static-inline wrappers using the SPU MFC atomic
 * primitives mfc_getllar (get lock line and reserve) and mfc_putllc
 * (put lock line conditional).  The reservation-granule +
 * conditional-store retry pattern is the standard SPU MFC atomic
 * idiom (Cell BE Handbook, Chapter 19, MFC Atomic Update Commands).
 *
 * Memory ordering: mfc_getllar is an acquire barrier (the MFC
 * command serializes before the reservation read).  mfc_putllc is a
 * release barrier on success (the store is visible to other SPUs
 * only after the reservation token is released).
 */
#ifndef __CELL_ATOMIC_H_SPU__
#define __CELL_ATOMIC_H_SPU__

#include <stdint.h>
#include <spu_mfcio.h>
#include <spu_intrinsics.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---------- 32-bit atomic ops ---------- */

static inline uint32_t
cellAtomicLockLine32(uint32_t *ls, uint64_t ea)
{
    unsigned int i = ((uint32_t)ea & 0x7f) >> 2;
    spu_hcmpeq(((uintptr_t)ls & 0x7f) == 0, 0);
    ea &= ~0x7f;
    mfc_getllar(ls, ea, 0, 0);
    mfc_read_atomic_status();
    return ls[i];
}

static inline int
cellAtomicStoreConditional32(uint32_t *ls, uint64_t ea, uint32_t value)
{
    unsigned int i = ((uint32_t)ea & 0x7f) >> 2;
    spu_hcmpeq(((uintptr_t)ls & 0x7f) == 0, 0);
    ls[i] = value;
    ea &= ~0x7f;
    mfc_putllc(ls, ea, 0, 0);
    /* MFC_PUTLLC_STATUS == 0 on success, non-zero on reservation lost */
    return mfc_read_atomic_status() == 0;
}

static inline uint32_t
cellAtomicAdd32(uint32_t *ls, uint64_t ea, uint32_t value)
{
    uint32_t old;
    do {
        old = cellAtomicLockLine32(ls, ea);
    } while (!cellAtomicStoreConditional32(ls, ea, old + value));
    return old;
}

static inline uint32_t
cellAtomicSub32(uint32_t *ls, uint64_t ea, uint32_t value)
{
    uint32_t old;
    do {
        old = cellAtomicLockLine32(ls, ea);
    } while (!cellAtomicStoreConditional32(ls, ea, old - value));
    return old;
}

static inline uint32_t
cellAtomicAnd32(uint32_t *ls, uint64_t ea, uint32_t value)
{
    uint32_t old;
    do {
        old = cellAtomicLockLine32(ls, ea);
    } while (!cellAtomicStoreConditional32(ls, ea, old & value));
    return old;
}

static inline uint32_t
cellAtomicOr32(uint32_t *ls, uint64_t ea, uint32_t value)
{
    uint32_t old;
    do {
        old = cellAtomicLockLine32(ls, ea);
    } while (!cellAtomicStoreConditional32(ls, ea, old | value));
    return old;
}

static inline uint32_t
cellAtomicStore32(uint32_t *ls, uint64_t ea, uint32_t value)
{
    uint32_t old;
    do {
        old = cellAtomicLockLine32(ls, ea);
    } while (!cellAtomicStoreConditional32(ls, ea, value));
    return old;
}

static inline uint32_t
cellAtomicIncr32(uint32_t *ls, uint64_t ea)
{
    uint32_t old;
    do {
        old = cellAtomicLockLine32(ls, ea);
    } while (!cellAtomicStoreConditional32(ls, ea, old + 1));
    return old;
}

static inline uint32_t
cellAtomicDecr32(uint32_t *ls, uint64_t ea)
{
    uint32_t old;
    do {
        old = cellAtomicLockLine32(ls, ea);
    } while (!cellAtomicStoreConditional32(ls, ea, old - 1));
    return old;
}

static inline uint32_t
cellAtomicNop32(uint32_t *ls, uint64_t ea)
{
    return cellAtomicLockLine32(ls, ea);
}

static inline uint32_t
cellAtomicTestAndDecr32(uint32_t *ls, uint64_t ea)
{
    uint32_t old;
    do {
        old = cellAtomicLockLine32(ls, ea);
        if (old == 0)
            break;
    } while (!cellAtomicStoreConditional32(ls, ea, old - 1));
    return old;
}

static inline uint32_t
cellAtomicCompareAndSwap32(uint32_t *ls, uint64_t ea,
                           uint32_t compare, uint32_t swap)
{
    uint32_t old;
    do {
        old = cellAtomicLockLine32(ls, ea);
        if (old != compare)
            break;
    } while (!cellAtomicStoreConditional32(ls, ea, swap));
    return old;
}

/* ---------- 64-bit atomic ops ---------- */

static inline uint64_t
cellAtomicLockLine64(uint64_t *ls, uint64_t ea)
{
    unsigned int i = ((uint32_t)ea & 0x7f) >> 3;
    spu_hcmpeq(((uintptr_t)ls & 0x7f) == 0, 0);
    ea &= ~0x7f;
    mfc_getllar(ls, ea, 0, 0);
    mfc_read_atomic_status();
    return ls[i];
}

static inline int
cellAtomicStoreConditional64(uint64_t *ls, uint64_t ea, uint64_t value)
{
    unsigned int i = ((uint32_t)ea & 0x7f) >> 3;
    spu_hcmpeq(((uintptr_t)ls & 0x7f) == 0, 0);
    ls[i] = value;
    ea &= ~0x7f;
    mfc_putllc(ls, ea, 0, 0);
    /* MFC_PUTLLC_STATUS == 0 on success, non-zero on reservation lost */
    return mfc_read_atomic_status() == 0;
}

static inline uint64_t
cellAtomicAdd64(uint64_t *ls, uint64_t ea, uint64_t value)
{
    uint64_t old;
    do {
        old = cellAtomicLockLine64(ls, ea);
    } while (!cellAtomicStoreConditional64(ls, ea, old + value));
    return old;
}

static inline uint64_t
cellAtomicSub64(uint64_t *ls, uint64_t ea, uint64_t value)
{
    uint64_t old;
    do {
        old = cellAtomicLockLine64(ls, ea);
    } while (!cellAtomicStoreConditional64(ls, ea, old - value));
    return old;
}

static inline uint64_t
cellAtomicAnd64(uint64_t *ls, uint64_t ea, uint64_t value)
{
    uint64_t old;
    do {
        old = cellAtomicLockLine64(ls, ea);
    } while (!cellAtomicStoreConditional64(ls, ea, old & value));
    return old;
}

static inline uint64_t
cellAtomicOr64(uint64_t *ls, uint64_t ea, uint64_t value)
{
    uint64_t old;
    do {
        old = cellAtomicLockLine64(ls, ea);
    } while (!cellAtomicStoreConditional64(ls, ea, old | value));
    return old;
}

static inline uint64_t
cellAtomicStore64(uint64_t *ls, uint64_t ea, uint64_t value)
{
    uint64_t old;
    do {
        old = cellAtomicLockLine64(ls, ea);
    } while (!cellAtomicStoreConditional64(ls, ea, value));
    return old;
}

static inline uint64_t
cellAtomicIncr64(uint64_t *ls, uint64_t ea)
{
    uint64_t old;
    do {
        old = cellAtomicLockLine64(ls, ea);
    } while (!cellAtomicStoreConditional64(ls, ea, old + 1));
    return old;
}

static inline uint64_t
cellAtomicDecr64(uint64_t *ls, uint64_t ea)
{
    uint64_t old;
    do {
        old = cellAtomicLockLine64(ls, ea);
    } while (!cellAtomicStoreConditional64(ls, ea, old - 1));
    return old;
}

static inline uint64_t
cellAtomicNop64(uint64_t *ls, uint64_t ea)
{
    return cellAtomicLockLine64(ls, ea);
}

static inline uint64_t
cellAtomicTestAndDecr64(uint64_t *ls, uint64_t ea)
{
    uint64_t old;
    do {
        old = cellAtomicLockLine64(ls, ea);
        if (old == 0)
            break;
    } while (!cellAtomicStoreConditional64(ls, ea, old - 1));
    return old;
}

static inline uint64_t
cellAtomicCompareAndSwap64(uint64_t *ls, uint64_t ea,
                           uint64_t compare, uint64_t swap)
{
    uint64_t old;
    do {
        old = cellAtomicLockLine64(ls, ea);
        if (old != compare)
            break;
    } while (!cellAtomicStoreConditional64(ls, ea, swap));
    return old;
}

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* __CELL_ATOMIC_H_SPU__ */
