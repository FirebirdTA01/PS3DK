/* sys/spu_image.h - SPU program image lv2 surface.
 *
 * Reference-SDK-named types and entry points for loading an SPU ELF
 * into a sys_spu_image_t descriptor that sys_spu_thread_initialize
 * can consume.  Two typical paths:
 *
 *   - In-memory image:  sys_spu_image_import(&img, elf_bytes, type)
 *                       sys_spu_image_close(&img)
 *
 *   - File-backed:      sys_spu_image_open(&img, "/dev_hdd0/.../foo.elf")
 *                       sys_spu_image_close(&img)
 *
 * The struct byte-layout (4 x u32 = 16 bytes) is identical to
 * PSL1GHT's `sysSpuImage`; we ship reference-SDK field names
 * (entry_point, segs, nsegs) over a fresh struct definition rather
 * than typedef-aliasing onto PSL1GHT's, so sample code that writes
 * `img.entry_point = ...` compiles unchanged.  The lv2 syscall FNIDs
 * are shared - both spellings land on the same kernel exports.
 */
#ifndef __PS3DK_SYS_SPU_IMAGE_H__
#define __PS3DK_SYS_SPU_IMAGE_H__

#include <stdint.h>
#include <ppu-types.h>          /* sys_addr_t, u32, etc. */
#include <sys/spu.h>            /* PSL1GHT sysSpuImage* inline syscall forwarders */

#ifdef __cplusplus
extern "C" {
#endif

/* SPU segment types describe how each section of an SPU program is
 * loaded into Local Store: COPY transfers from main memory; FILL
 * blanks an LS region with a fixed 32-bit pattern; INFO is metadata
 * the kernel loader consumes but doesn't touch the LS for. */
#define SYS_SPU_SEGMENT_TYPE_COPY  0x0001
#define SYS_SPU_SEGMENT_TYPE_FILL  0x0002
#define SYS_SPU_SEGMENT_TYPE_INFO  0x0004

/* Image source types: USER images live in user-process memory and the
 * kernel must copy them in; KERNEL images are already kernel-resident
 * (used for built-in SPU helpers). */
#define SYS_SPU_IMAGE_TYPE_USER    0x0U
#define SYS_SPU_IMAGE_TYPE_KERNEL  0x1U

/* Import-mode flags: PROTECT validates / sandboxes the image
 * (typical for user code); DIRECT trusts the caller to have laid
 * out an already-valid descriptor. */
#define SYS_SPU_IMAGE_PROTECT      0x0U
#define SYS_SPU_IMAGE_DIRECT       0x1U

typedef struct sys_spu_segment {
    int        type;        /* SYS_SPU_SEGMENT_TYPE_{COPY,FILL,INFO} */
    uint32_t   ls_start;    /* LS offset where this segment lands */
    int        size;        /* segment size, 128 byte..16 KiB */
    union {
        sys_addr_t pa_start; /* COPY: main-memory source EA (128-byte aligned) */
        uint32_t   value;    /* FILL: 32-bit pattern */
        uint64_t   pad;
    } src;
} sys_spu_segment_t;

typedef struct sys_spu_image {
    uint32_t           type;        /* SYS_SPU_IMAGE_TYPE_USER / KERNEL */
    uint32_t           entry_point; /* ELF e_entry */
    sys_spu_segment_t *segs;        /* segment array (caller-allocated for OPEN) */
    int                nsegs;       /* number of valid segments */
} sys_spu_image_t;

/* Reference-SDK snake_case forwarders over PSL1GHT's sysSpuImage*
 * inline syscall wrappers.  Both sets bind to the same FNIDs - the
 * lv2 kernel exposes one set of entry points; only the C-level names
 * differ. */
static inline int sys_spu_image_import(sys_spu_image_t *img,
                                       const void *src,
                                       uint32_t type)
{
    return (int)sysSpuImageImport((sysSpuImage *)img, src, type);
}

static inline int sys_spu_image_close(sys_spu_image_t *img)
{
    return (int)sysSpuImageClose((sysSpuImage *)img);
}

static inline int sys_spu_image_open(sys_spu_image_t *img, const char *path)
{
    return (int)sysSpuImageOpen((sysSpuImage *)img, path);
}

static inline int sys_spu_image_open_by_fd(sys_spu_image_t *img,
                                           int fd,
                                           uint64_t offset)
{
    return (int)sysSpuImageOpenFd((sysSpuImage *)img, fd, offset);
}

#ifdef __cplusplus
}
#endif

#endif /* __PS3DK_SYS_SPU_IMAGE_H__ */
