/*
 * cell/spurs/job_context.h - SPU-side helpers used inside a
 * cellSpursJobMain2 implementation.
 *
 * Two header-only inlines walk the per-job DMA list the runtime built
 * for us and resurrect either a void* per input element or a
 * {size, pointer} pair per element.  The buddy entry points
 * cellSpursJobMemoryCheck{Initialize,Test} land in libspurs.sprx via
 * stub trampolines.
 *
 * The descriptor types (CellSpursJobHeader, CellSpursJobContext2,
 * CellSpursJobInputDataElement) come from cell/spurs/job_descriptor.h
 * which is shared between PPU and SPU sides.
 */

#ifndef __PS3DK_CELL_SPURS_JOB_CONTEXT_H_SPU__
#define __PS3DK_CELL_SPURS_JOB_CONTEXT_H_SPU__

#include <stdint.h>
#include <cell/spurs/job_descriptor.h>
#include <cell/spurs/error.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * cellSpursJobGetPointerList - reconstruct the per-input pointer array
 * the runtime DMA'd into ioBuffer.
 *
 * The per-job DMA list sits immediately after the job header in LS;
 * each 8-byte entry encodes:
 *   bits  0-31 : low 32 bits of the source EA
 *   bits 32-46 : transferred size in bytes (15 bits)
 *
 * Entries are 16-byte stride aligned.  The element count is
 *   jobHeader->sizeDmaList / sizeof(uint64_t).
 *
 * For each entry we walk a moving cursor through ioBuffer.  The
 * pointer we hand back has the low 4 bits of the source EA OR'd in -
 * the runtime preserved them so the caller can recover sub-quadword
 * misalignment without an extra cycle.
 */
static inline int
cellSpursJobGetPointerList(void *outList[],
                           const CellSpursJobHeader     *jobHeader,
                           const CellSpursJobContext2   *jobContext)
{
    if (__builtin_expect(outList == 0 || jobHeader == 0 || jobContext == 0, 0))
        return CELL_SPURS_JOB_ERROR_NULL_POINTER;
    if (__builtin_expect((uintptr_t)outList    & 3 ||
                         (uintptr_t)jobHeader  & 7 ||
                         (uintptr_t)jobContext & 7, 0))
        return CELL_SPURS_JOB_ERROR_ALIGN;

    const uint64_t *dmaList = (const uint64_t *)((uintptr_t)jobHeader + sizeof(CellSpursJobHeader));
    const int       n       = (int)(jobHeader->sizeDmaList / sizeof(uint64_t));

    uintptr_t cursor = (uintptr_t)jobContext->ioBuffer;
    for (int i = 0; i < n; ++i) {
        uint64_t ent  = dmaList[i];
        uint32_t eal  = (uint32_t)ent;
        uint32_t size = ((uint32_t)(ent >> 32)) & 0x7fff;

        outList[i] = (void *)(cursor | (eal & 0xf));
        cursor    += (size + 0xf) & ~0xfu;
    }
    return CELL_OK;
}

/*
 * cellSpursJobGetInputDataElements - same walk, but yields
 * {size, pointer} pairs so the caller doesn't need to reach back into
 * the DMA list to recover transferred sizes.
 */
static inline int
cellSpursJobGetInputDataElements(CellSpursJobInputDataElement *elemList,
                                 const CellSpursJobHeader     *jobHeader,
                                 const CellSpursJobContext2   *jobContext)
{
    if (__builtin_expect(elemList == 0 || jobHeader == 0 || jobContext == 0, 0))
        return CELL_SPURS_JOB_ERROR_NULL_POINTER;
    if (__builtin_expect((uintptr_t)elemList   & 3 ||
                         (uintptr_t)jobHeader  & 7 ||
                         (uintptr_t)jobContext & 7, 0))
        return CELL_SPURS_JOB_ERROR_ALIGN;

    const uint64_t *dmaList = (const uint64_t *)((uintptr_t)jobHeader + sizeof(CellSpursJobHeader));
    const int       n       = (int)(jobHeader->sizeDmaList / sizeof(uint64_t));

    uintptr_t cursor = (uintptr_t)jobContext->ioBuffer;
    for (int i = 0; i < n; ++i) {
        uint64_t ent  = dmaList[i];
        uint32_t eal  = (uint32_t)ent;
        uint32_t size = ((uint32_t)(ent >> 32)) & 0x7fff;

        elemList[i].size    = size;
        elemList[i].pointer = (void *)(cursor | (eal & 0xf));
        cursor             += (size + 0xf) & ~0xfu;
    }
    return CELL_OK;
}

/*
 * Buffer-overrun watchdog the runtime arms before each job and
 * polls after.  Initialize copies the post-tail signature into the
 * scratch end; Test verifies it survived.
 */
extern int  cellSpursJobMemoryCheckInitialize(const CellSpursJobContext2 *jobContext,
                                              CellSpursJobHeader         *jobHeader);
extern int  cellSpursJobMemoryCheckTest      (uint16_t *cause);

#ifdef __cplusplus
}
#endif

#endif /* __PS3DK_CELL_SPURS_JOB_CONTEXT_H_SPU__ */
