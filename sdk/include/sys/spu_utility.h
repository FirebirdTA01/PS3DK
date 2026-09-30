/*! \file sys/spu_utility.h
 \brief LV2 SPU loader / ELF-image inspector helpers.

  the reference SDK source-compat surface for the small handful of sys_spu_*
  helpers that probe an SPU ELF image (entry point + segment list) and
  load programs into either a thread or a raw SPU.  The underlying LV2
  syscalls are reachable via PSL1GHT's <lv2/spu.h> wrappers — this
  header re-spells them under the reference SDK names so the reference SDK
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

/* Inspect an SPU ELF image in main memory: its entry point, and how many
 * segments sys_spu_elf_get_segments will describe.  Both are system
 * library (sysPrxForUser) functions, imported through liblv2_stub. */
int sys_spu_elf_get_information(sys_addr_t elf_img, uint32_t *entry, int *nseg);

/* Fill segments[0..nseg) with the image's load segments (COPY for file
 * contents, FILL for zeroed space), sources pointing into elf_img. */
int sys_spu_elf_get_segments(sys_addr_t elf_img, sys_spu_segment_t *segments, int nseg);

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
