/* Compile this adapter separately against the candidate and the pinned historical headers. */
#include "i3b_table.h"
#include "psp_ppu.h"
#include <string.h>

void i3b_table(const i3b_desc *input, uint32_t n, uint32_t build_len,
              uint8_t *arena, size_t arena_size, i3b_table_result *r)
{
    psp_ppu p;
    psp_cfg cfg = {64, 1, 1u << 20, 1024};
    psp_limits lim = {0};
    psp_mem mem = {0};
    psp_out out = {0};
    static uint8_t scratch[PSP_SCRATCH_BYTES], copy[512];
    static psp_desc descs[4096];
    psp_storage storage = {scratch, sizeof scratch, copy, sizeof copy, arena, arena_size};
    uint8_t bid[65];
    memset(r, 0, sizeof *r);
    r->index = 9;
    r->rc = -1;
    lim.lanes_life = lim.lanes_live = 64;
    lim.images = 32;
    lim.epochs = 256;
    lim.loss_chunks = lim.brackets_epoch = 65536;
    lim.brackets_file = 262144;
    if (n > 4096) {
        r->init_rc = -1;
        return;
    }
    for (uint32_t i = 0; i < n; i++)
        descs[i] = (psp_desc){input[i].ls_addr, input[i].line, input[i].name, input[i].file};
    memset(bid, 0x3c, sizeof bid);
    memset(arena, 0x5a, arena_size);
    r->init_rc = psp_ppu_init(&p, &cfg, &lim, &mem, &out, &storage);
    if (r->init_rc != 0)
        return;
    r->rc = psp_image_add(&p, bid, build_len, descs, n, &r->index);
    r->arena_used = p.arena_used;
    r->nimages = p.nimages;
    if (r->rc != 0)
        return;
    const psp_image *image = &p.images[r->index];
    r->build_len = image->build_len;
    r->ndesc = image->ndesc;
    memcpy(r->build_id, image->build_id, image->build_len);
    r->desc_offset = (const uint8_t *)image->descs - arena;
    r->desc_bytes = sizeof(psp_desc) * image->ndesc;
    for (uint32_t i = 0; i < image->ndesc; i++) {
        const psp_desc *d = &image->descs[i];
        r->descs[i] = (i3b_stored_desc){d->ls_addr, d->line,
            d->name ? (size_t)(d->name - (const char *)arena) : SIZE_MAX,
            d->file ? (size_t)(d->file - (const char *)arena) : SIZE_MAX};
    }
}
