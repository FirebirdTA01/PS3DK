/* One SPU zone descriptor: the single struct type shared by the lane API (prof_spu.h) and the PPU core
 * (psp_ppu.h), so registration reads it through its own type (no cast between distinct struct types). */
#ifndef PSP_DESC_H
#define PSP_DESC_H

#include <stdint.h>

struct ps3dk_prof_spu_desc {
    uint32_t ls_addr, line;
    const char *name, *file;
};

#endif
