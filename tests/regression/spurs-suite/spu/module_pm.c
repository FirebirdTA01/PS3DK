/* spurs-suite custom policy module: the SPURS kernel loads it at LS 0xa00
 * and enters it for its workload.  Each run counts itself in the box the
 * workload argument points at, then gives the SPU back (module.h). */
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
    mfc_put(&box, arg, sizeof box, 0, 0, 0);
    mfc_read_tag_status_all();
    cellSpursModuleExit();
}
