/* The SPU port: MFC channel operations as static inlines (design I3 r2 sections 2 and 4).  SPU builds only.
 * Ordering does not rely on -msafe-dma: every command issue is preceded, and every completion read followed, by a
 * compiler memory barrier, so local-store stores stay before the DMA that reads them and loads stay after the wait. */
#ifndef PSP_PORT_SPU_H
#define PSP_PORT_SPU_H

#include <spu_mfcio.h>
#include <stdint.h>

#include "psp_as.h"

_Static_assert(MFC_PUTLLC_STATUS == PSP_RAW_PUTLLC_FAILED, "raw putllc status bit");
_Static_assert(MFC_PUTLLUC_STATUS == PSP_RAW_PUTLLUC_DONE, "raw putlluc status bit");
_Static_assert(MFC_GETLLAR_STATUS == PSP_RAW_GETLLAR_DONE, "raw getllar status bit");

#define PSP_SPU_BARRIER() __asm__ __volatile__("" ::: "memory")

static inline uint32_t psp_port_spu_rd_dec(void) { return spu_readch(SPU_RdDec); }
static inline void psp_port_spu_wr_dec(uint32_t v) { spu_writech(SPU_WrDec, v); }
/* the events the library must not raise or consume: reservation lost, tag-status update */
static inline int psp_port_spu_blocking_events(void)
{
    return (spu_readch(SPU_RdEventMask) & (MFC_LLR_LOST_EVENT | MFC_TAG_STATUS_UPDATE_EVENT)) != 0;
}
static inline uint32_t psp_port_spu_rd_tag_mask(void) { return spu_readch(MFC_RdTagMask); }
static inline void psp_port_spu_wr_tag_mask(uint32_t m) { mfc_write_tag_mask(m); }
static inline uint32_t psp_port_spu_tag_wait_all(void)
{
    uint32_t st;
    PSP_SPU_BARRIER();
    st = mfc_read_tag_status_all();
    PSP_SPU_BARRIER();
    return st;
}
static inline void psp_port_spu_getllar(uint64_t ea, void *ls)
{
    PSP_SPU_BARRIER();
    mfc_getllar(ls, ea, 0, 0);
}
static inline void psp_port_spu_putllc(uint64_t ea, const void *ls)
{
    PSP_SPU_BARRIER();
    mfc_putllc((volatile void *)(uintptr_t)ls, ea, 0, 0);
}
static inline void psp_port_spu_putlluc(uint64_t ea, const void *ls)
{
    PSP_SPU_BARRIER();
    mfc_putlluc((volatile void *)(uintptr_t)ls, ea, 0, 0);
}
static inline uint32_t psp_port_spu_rd_atomic_raw(void)
{
    uint32_t st = mfc_read_atomic_status();
    PSP_SPU_BARRIER();
    return st;
}
/* size is 8 (8-aligned) or a multiple of 16 (16-aligned), at most 16 KiB: the core guarantees it */
static inline void psp_port_spu_put(const void *ls, uint64_t ea, uint32_t size, uint32_t tag)
{
    PSP_SPU_BARRIER();
    mfc_put((volatile void *)(uintptr_t)ls, ea, size, tag, 0, 0);
}

#endif
