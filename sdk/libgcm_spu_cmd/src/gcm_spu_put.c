/*
 * PS3 Custom Toolchain — sdk/libgcm_spu_cmd/src/gcm_spu_put.c
 *
 * cellGcmSpuPutSegment: DMA an ended local-store command buffer to its
 * main-memory segment.  SPU only.
 */
#include <stdint.h>
#include <spu_mfcio.h>
#include <cell/gcm/gcm_spu_cmd.h>

int32_t cellGcmSpuPutSegment(const CellGcmSpuContextData *c, uint64_t ea, uint32_t tag)
{
    uint32_t bytes, done = 0;
    uintptr_t ls;

    if (!c || !c->begin || !c->ended) return CELL_GCM_SPU_ERROR_STATE;
    if (c->overflowWords) return CELL_GCM_SPU_ERROR_OVERFLOW;
    ls = (uintptr_t)c->begin;
    if ((ea & 15u) || (ls & 15u) || tag > 31u) return CELL_GCM_SPU_ERROR_INVALID;
    bytes = ((uint32_t)(c->current - c->begin) * 4u + 15u) & ~15u;
    while (done < bytes) {
        uint32_t n = bytes - done > 16384u ? 16384u : bytes - done;
        mfc_put((volatile void *)(ls + done), ea + done, n, tag, 0, 0);
        done += n;
    }
    mfc_write_tag_mask(1u << tag);
    mfc_read_tag_status_all();
    return CELL_OK;
}
