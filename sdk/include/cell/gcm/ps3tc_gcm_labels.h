/* <cell/gcm/ps3tc_gcm_labels.h> -- RSX label and report index reservations, and a non-blocking label query.
 *
 * The rule (docs/gcm-labels.md, enforced by tests/sdk/gcm-label-reservation-test.py):
 *
 *   Labels (256, cellGcmGetLabelAddress):
 *     0..63     SYSTEM    never written or waited on by application code (CELL_GCM_INDEX_RANGE_LABEL_MIN is 64).
 *     65        PSGL      libPSGL prepared-buffer label.
 *     66..68    PSGL      libPSGL per-frame buffer-status labels (one per PSGL frame buffer, at most 3).
 *     240..247  PROFILER  the block the profiler samples lend to ps3dk_prof_gpu_attach and use for their fences.
 *                         The profiler library itself owns no fixed label: the application lends one.
 *     255       PSGL      libPSGL idle/finish token.
 *   Every other label (64, 69..239, 248..254) is the APPLICATION's.  The PSGL labels are only written when libPSGL
 *   is linked and initialised; a pure GCM application may treat them as its own, but should not, so that it never
 *   collides with a later PSGL or SDK tool.
 *
 *   Local reports (2048, cellGcmGetReportDataAddressLocation(i, CELL_GCM_LOCATION_LOCAL), cellGcmSetTimeStamp):
 *     0..511    PROFILER  the block the profiler samples lend to ps3dk_prof_gpu_attach and use for raw timestamps.
 *     512..2047 APPLICATION.
 *   No SDK library writes a local report today; the main-memory report area is entirely the application's.
 *
 * A1 query: cellGcmGetReservedLabelRanges() / cellGcmGetReservedReportRanges() return the ranges above;
 * cellGcmGetLabelOwner() / cellGcmGetReportOwner() test one index.
 *
 * A4 query: cellGcmIsLabelPassed(index, target) is a NON-BLOCKING completion test: it reads the label once, then
 * orders that read before every later load and store of the calling thread (a PowerPC `sync`), and compares the value
 * with `target` in 32-bit serial arithmetic, so a serial that wraps past 0xffffffff still compares correctly as long
 * as the label and the target are less than 2^31 apart.  See cellGcmIsSerialReached for the exact rule.
 *
 * Header only (static inline), no library, both PPU data models.  The serial and range helpers have no GCM
 * dependency and compile on any host; the label read needs <cell/gcm.h> (included unless
 * PS3TC_GCM_LABELS_NO_GCM is defined, which host tests use with their own cellGcmGetLabelAddress).
 */
#ifndef PS3TC_GCM_LABELS_H
#define PS3TC_GCM_LABELS_H

#include <stdint.h>

#ifndef PS3TC_GCM_LABELS_NO_GCM
#include <cell/gcm.h>
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* ---- owners ----------------------------------------------------------------------------------------------------- */
#define CELL_GCM_INDEX_OWNER_SYSTEM   1u /* firmware / GCM system use */
#define CELL_GCM_INDEX_OWNER_PSGL     2u /* libPSGL */
#define CELL_GCM_INDEX_OWNER_PROFILER 3u /* the profiler lending block */

/* ---- labels ----------------------------------------------------------------------------------------------------- */
#define CELL_GCM_LABEL_COUNT                256u
#define CELL_GCM_LABEL_SYSTEM_FIRST         0u
#define CELL_GCM_LABEL_SYSTEM_LAST          63u
#define CELL_GCM_LABEL_PSGL_PREPARED        65u
#define CELL_GCM_LABEL_PSGL_STATUS_FIRST    66u
#define CELL_GCM_LABEL_PSGL_STATUS_LAST     68u
#define CELL_GCM_LABEL_PSGL_IDLE            255u
#define CELL_GCM_LABEL_PROFILER_FIRST       240u
#define CELL_GCM_LABEL_PROFILER_LAST        247u
/* the largest contiguous application block: use it for per-slot frame-completion labels */
#define CELL_GCM_LABEL_APP_FIRST            69u
#define CELL_GCM_LABEL_APP_LAST             239u

/* ---- local reports ---------------------------------------------------------------------------------------------- */
#define CELL_GCM_REPORT_LOCAL_COUNT         2048u
#define CELL_GCM_REPORT_PROFILER_FIRST      0u
#define CELL_GCM_REPORT_PROFILER_LAST       511u
#define CELL_GCM_REPORT_APP_FIRST           512u
#define CELL_GCM_REPORT_APP_LAST            2047u

/* One reserved range: [first, last] inclusive, owned by `owner` (CELL_GCM_INDEX_OWNER_*). */
typedef struct CellGcmIndexRange {
    uint32_t first;
    uint32_t last;
    uint32_t owner;
    uint32_t reserved; /* 0 */
} CellGcmIndexRange;

/* The reserved label ranges, ascending and disjoint; *count receives their number (NULL allowed). */
static inline const CellGcmIndexRange *cellGcmGetReservedLabelRanges(uint32_t *count)
{
    static const CellGcmIndexRange ranges[] = {
        {CELL_GCM_LABEL_SYSTEM_FIRST, CELL_GCM_LABEL_SYSTEM_LAST, CELL_GCM_INDEX_OWNER_SYSTEM, 0u},
        {CELL_GCM_LABEL_PSGL_PREPARED, CELL_GCM_LABEL_PSGL_PREPARED, CELL_GCM_INDEX_OWNER_PSGL, 0u},
        {CELL_GCM_LABEL_PSGL_STATUS_FIRST, CELL_GCM_LABEL_PSGL_STATUS_LAST, CELL_GCM_INDEX_OWNER_PSGL, 0u},
        {CELL_GCM_LABEL_PROFILER_FIRST, CELL_GCM_LABEL_PROFILER_LAST, CELL_GCM_INDEX_OWNER_PROFILER, 0u},
        {CELL_GCM_LABEL_PSGL_IDLE, CELL_GCM_LABEL_PSGL_IDLE, CELL_GCM_INDEX_OWNER_PSGL, 0u},
    };
    if (count)
        *count = (uint32_t)(sizeof ranges / sizeof ranges[0]);
    return ranges;
}

/* The reserved LOCAL report ranges, ascending and disjoint. */
static inline const CellGcmIndexRange *cellGcmGetReservedReportRanges(uint32_t *count)
{
    static const CellGcmIndexRange ranges[] = {
        {CELL_GCM_REPORT_PROFILER_FIRST, CELL_GCM_REPORT_PROFILER_LAST, CELL_GCM_INDEX_OWNER_PROFILER, 0u},
    };
    if (count)
        *count = (uint32_t)(sizeof ranges / sizeof ranges[0]);
    return ranges;
}

/* The owner of a reserved index, or 0 when the index is the application's (or out of range: see the *_COUNT). */
static inline uint32_t cellGcm__rangeOwner(const CellGcmIndexRange *r, uint32_t n, uint32_t index)
{
    for (uint32_t i = 0; i < n; i++)
        if (index >= r[i].first && index <= r[i].last)
            return r[i].owner;
    return 0u;
}

/* 0: the label is the application's; otherwise its CELL_GCM_INDEX_OWNER_*.  An index >= 256 returns 0 but is invalid. */
static inline uint32_t cellGcmGetLabelOwner(uint32_t index)
{
    uint32_t n;
    const CellGcmIndexRange *r = cellGcmGetReservedLabelRanges(&n);
    return cellGcm__rangeOwner(r, n, index);
}

/* 0: the local report is the application's; otherwise its CELL_GCM_INDEX_OWNER_*. */
static inline uint32_t cellGcmGetReportOwner(uint32_t index)
{
    uint32_t n;
    const CellGcmIndexRange *r = cellGcmGetReservedReportRanges(&n);
    return cellGcm__rangeOwner(r, n, index);
}

/* ---- serial arithmetic (A4) ------------------------------------------------------------------------------------- */

/* 1 when `current` has reached `target` in 32-bit serial order: (int32_t)(current - target) >= 0, computed without
 * signed overflow.  Exact while the true distance between the two is below 2^31; at a distance of 2^31 or more the
 * answer is the wrap-around one and is NOT meaningful.  So a writer that bumps a label by 1 per frame may have at most
 * 2^31 - 1 frames in flight against one reader, which no ring of frame slots approaches. */
static inline int cellGcmIsSerialReached(uint32_t current, uint32_t target)
{
    return (uint32_t)(current - target) < 0x80000000u;
}

/* Reads a label word once and orders that read before every later load and store of this thread.
 *
 * On the PPU: one volatile 32-bit load, then `sync`.  The label area is RSX-visible memory that the RSX writes and the
 * PPU reads uncached; lwsync does not order accesses to caching-inhibited storage, so the heavyweight `sync` is used:
 * after it returns, a later PPU store into a buffer the RSX read before writing this value (an arena slot) cannot be
 * performed ahead of the label read.  That this ordering, plus the RSX's own write ordering of the label after its
 * reads, makes buffer reuse safe is the A2 question; see docs/gcm-labels.md for what is and is not verified.
 * Elsewhere (host tests): an acquire load plus a sequentially consistent fence. */
static inline uint32_t cellGcmReadLabelOrdered(const volatile uint32_t *label)
{
#if defined(__PPU__) || defined(__powerpc__) || defined(__powerpc64__) || defined(__PPC__)
    uint32_t v = *label;
    __asm__ __volatile__("sync" ::: "memory");
    return v;
#else
    uint32_t v = __atomic_load_n((const uint32_t *)(uintptr_t)label, __ATOMIC_ACQUIRE);
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    return v;
#endif
}

/* Non-blocking: 1 when the label word at `label` has reached `target` (serial order), else 0. */
static inline int cellGcmIsLabelValuePassed(const volatile uint32_t *label, uint32_t target)
{
    return cellGcmIsSerialReached(cellGcmReadLabelOrdered(label), target);
}

/* Non-blocking: 1 when GCM label `index` has reached `target` (serial order), else 0.  Never waits, never writes. */
static inline int cellGcmIsLabelPassed(uint8_t index, uint32_t target)
{
    return cellGcmIsLabelValuePassed((const volatile uint32_t *)cellGcmGetLabelAddress(index), target);
}

#ifdef __cplusplus
}
#endif

#endif /* PS3TC_GCM_LABELS_H */
