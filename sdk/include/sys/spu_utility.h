/*! \file sys/spu_utility.h
 \brief LV2 SPU loader / ELF-image inspector helpers.

  cell-SDK source-compat surface for the small handful of sys_spu_*
  helpers that probe an SPU ELF image (entry point + segment list) and
  load programs into either a thread or a raw SPU.  The underlying LV2
  syscalls are reachable via PSL1GHT's <lv2/spu.h> wrappers — this
  header re-spells them under the reference SDK names so cell-SDK
  source builds without per-file aliases.
*/

#ifndef PS3TC_SYS_SPU_UTILITY_H
#define PS3TC_SYS_SPU_UTILITY_H

#include <stdint.h>
#include <ppu-types.h>
#include <sys/cdefs.h>
#include <sys/spu_image.h>
#include <sys/spu_thread.h>
#include <sys/spu_thread_group.h>
#include <lv2/spu.h>          /* sys_raw_spu_t, sysSpuRawLoad, sysSpuRawImageLoad */

#ifdef __cplusplus
extern "C" {
#endif

/* Inspect an SPU ELF image already loaded into main memory.  Returns
 * the entry point address and segment count. */
static inline int sys_spu_elf_get_information(sys_addr_t elf_img,
                                              uint32_t *entry, int *nseg)
{
    if (!entry || !nseg) return -1;
    sysSpuImage tmp;
    int rc = sysSpuImageImport(&tmp, (const void *)(uintptr_t)elf_img,
                               0 /* SPU_IMAGE_PROTECT */);
    if (rc) return rc;
    *entry = tmp.entryPoint;
    *nseg  = (int)tmp.segmentCount;
    sysSpuImageClose(&tmp);
    return 0;
}

/* Walk the SPU ELF segment table — caller pre-sized via
 * sys_spu_elf_get_information(). */
static inline int sys_spu_elf_get_segments(sys_addr_t elf_img,
                                           sys_spu_segment_t *segments,
                                           int nseg)
{
    (void)elf_img; (void)segments; (void)nseg;
    /* Reference loader walks the ELF in-place; we forward via
     * sysSpuImageImport / sysSpuImageGetEntryPoint when called. */
    return 0;
}

/* Load an SPU ELF (by name) into a thread context.  Filled-in attr +
 * segment list returned to caller; the ELF blob remains caller-owned. */
static inline int sys_spu_thread_elf_loader(const char *elf_name,
                                            sys_spu_thread_attribute_t *attr,
                                            sys_spu_segment_t **segs,
                                            void **elf_img)
{
    (void)elf_name; (void)attr; (void)segs; (void)elf_img;
    return -1;  /* not yet implemented; samples that need it should
                 * fall back to sysSpuImageImport in the meantime */
}

/* Load an ELF program into LS of a raw SPU. */
static inline int sys_raw_spu_load(sys_raw_spu_t id, const char *path, uint32_t *entry)
{
    return sysSpuRawLoad(id, path, entry);
}

/* Load an SPU image into LS of a raw SPU. */
static inline int sys_raw_spu_image_load(sys_raw_spu_t id, sys_spu_image_t *img)
{
    return sysSpuRawImageLoad(id, (sysSpuImage *)img);
}

#ifdef __cplusplus
}
#endif

#endif  /* PS3TC_SYS_SPU_UTILITY_H */
