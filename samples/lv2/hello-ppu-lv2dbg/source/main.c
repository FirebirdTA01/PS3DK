#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>

#include <sys/process.h>
#include <sys/dbg.h>
#include <cell/sysmodule.h>

SYS_PROCESS_PARAM(1001, 0x10000);

int main(void)
{
    int rc = cellSysmoduleLoadModule(CELL_SYSMODULE_LV2DBG);
    printf("load lv2dbg rc=0x%08x\n", rc);
    if (rc != 0 && rc != CELL_SYSMODULE_LOADED) {
        return 1;
    }

    sys_ppu_thread_t ids[16] = {0};
    uint64_t ids_num = 16;
    uint64_t all_ids_num = 0;

    rc = sys_dbg_get_ppu_thread_ids(ids, &ids_num, &all_ids_num);
    printf("sys_dbg_get_ppu_thread_ids rc=0x%08x ids=%" PRIu64 " all=%" PRIu64 "\n",
           rc, ids_num, all_ids_num);
    for (uint64_t i = 0; i < ids_num && i < 16; ++i) {
        printf("  ppu[%" PRIu64 "]=0x%016" PRIx64 "\n", i, ids[i]);
    }

    cellSysmoduleUnloadModule(CELL_SYSMODULE_LV2DBG);
    return rc == 0 ? 0 : 1;
}
