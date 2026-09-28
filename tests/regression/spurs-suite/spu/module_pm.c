/* spurs-suite custom policy module: the SPURS kernel loads it at LS 0xa00
 * and enters it for its workload.  Each run counts itself in the box the
 * workload argument points at, then gives the SPU back (module.h).  When
 * the box names a work unit, the module loads that relocatable image at
 * M_UNIT_BASE, clears 4 KB after it for its .bss, and calls it. */
#include <stdint.h>
#include <spu_mfcio.h>
#include <cell/spurs/common.h>
#include <cell/spurs/policy_module.h>
#include "../module.h"

/* in .data: the module image is loaded flat, without a .bss */
static module_box box __attribute__((section(".data"))) = { 0 };

void cellSpursModuleEntry(uintptr_t context, uint64_t arg)
{
    (void)context;
    mfc_get(&box, arg, sizeof box, 0, 0, 0);
    mfc_write_tag_mask(1u << 0);
    mfc_read_tag_status_all();
    box.magic = M_MAGIC;
    box.runs += 1;
    box.wid = cellSpursGetWorkloadId();
    box.spu = cellSpursGetCurrentSpuId();
    box.arg = (uint32_t)arg;
    if (box.unitEa) {
        const uint32_t size = (box.unitSize + 15) & ~15u;
        uint32_t off;
        for (off = 0; off < size; off += 16384)
            mfc_get((volatile void *)(M_UNIT_BASE + off), box.unitEa + off,
                    size - off > 16384 ? 16384 : size - off, 0, 0, 0);
        mfc_read_tag_status_all();
        for (off = 0; off < 4096; off += 4)
            *(volatile uint32_t *)(M_UNIT_BASE + size + off) = 0;
        box.unitResult = ((unsigned (*)(void))M_UNIT_BASE)();
    }
    mfc_put(&box, arg, sizeof box, 0, 0, 0);
    mfc_read_tag_status_all();
    cellSpursModuleExit();
}
