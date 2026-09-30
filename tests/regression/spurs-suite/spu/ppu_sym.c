/* spurs-ppu-sym row, SPU task: take the addresses of two PPU globals with
 * CELL_SPURS_PPU_SYM, read each one through its address and mark it seen,
 * then report the addresses and what was read. */
#include <stdint.h>
#include <spu_intrinsics.h>
#include <spu_mfcio.h>
#include <cell/spurs.h>
#include <cell/spurs/task.h>
#include "../ppu_sym.h"

static ps_box box;
static ps_target target;

static uint32_t visit(uint32_t ea)
{
    mfc_get(&target, ea, sizeof target, 1, 0, 0);
    mfc_write_tag_mask(1u << 1);
    mfc_read_tag_status_all();
    target.seen = 1;
    mfc_put(&target, ea, sizeof target, 1, 0, 0);
    mfc_write_tag_mask(1u << 1);
    mfc_read_tag_status_all();
    return target.magic;
}

int cellSpursTaskMain(qword argTask, uint64_t argTaskset)
{
    struct { uint64_t box; uint32_t a2, a3; } arg __attribute__((aligned(16)));
    (void)argTaskset;
    *(qword *)&arg = argTask;

    box.eaA = CELL_SPURS_PPU_SYM(ps_target_a);
    box.eaB = CELL_SPURS_PPU_SYM(ps_target_b);
    box.magicA = box.eaA ? visit(box.eaA) : 0;
    box.magicB = box.eaB ? visit(box.eaB) : 0;
    box.state = 2;
    mfc_put(&box, arg.box, sizeof box, 1, 0, 0);
    mfc_write_tag_mask(1u << 1);
    mfc_read_tag_status_all();
    return 0;
}
