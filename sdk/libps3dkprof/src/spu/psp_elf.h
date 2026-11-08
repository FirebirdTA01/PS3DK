/* The SPU program's ELF, as raw file bytes held by the PPU (an embedspu blob or a file read into memory; NOT a
 * sys_spu_image_t): validation of the descriptor section and the build id (design P3.2 I3b r2).  Portable C, byte
 * decoding only, every scan and count bounded; no allocation; the view lives on the caller's stack. */
#ifndef PSP_ELF_H
#define PSP_ELF_H

#include <stdint.h>

#include "psp_ppu.h"

#define psp_elf_parse ps3dk_prof__psp_elf_parse
#define psp_elf_get ps3dk_prof__psp_elf_get
#define psp_elf_text_ok ps3dk_prof__psp_elf_text_ok

#define PSP_ELF_SIZE_MAX (16u << 20) /* the supported file-size cap */
#define PSP_ELF_PHNUM_MAX 64u
#define PSP_ELF_SHNUM_MAX 4096u
#define PSP_ELF_NOTE_MAX 4096u
#define PSP_ELF_LS_END 0x40000u /* the 256 KiB local store */

typedef struct psp_elf_view {
    const uint8_t *b;
    uint64_t size;
    uint32_t phoff, phnum;
    uint32_t desc_addr, desc_off, n;
    uint32_t build_len;
    uint8_t build_id[64]; /* kind byte first: 0x01 + GNU build id, or 0x02 + FNV-1a-64 (big-endian) */
} psp_elf_view;

/* 0 and *v filled, or -1 (EINVAL): every rule of the design, all records validated */
int psp_elf_parse(const void *elf, uint64_t size, psp_elf_view *v);
/* a psp_desc_src getter over a parsed view (ctx = the view): record i, re-derived and re-checked */
int psp_elf_get(void *ctx, uint32_t i, psp_desc_rec *out);
/* the ELF string policy: lo..hi bytes of printable ASCII 0x20..0x7E, NUL-terminated before `end` */
int psp_elf_text_ok(const uint8_t *s, const uint8_t *end, uint32_t lo, uint32_t hi, uint32_t *len);

#endif
