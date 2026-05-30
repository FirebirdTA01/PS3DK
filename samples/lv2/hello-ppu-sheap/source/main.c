#include <stdint.h>
#include <stdio.h>

#include <sys/process.h>
#include <cell/sheap.h>
#include <cell/sysmodule.h>

SYS_PROCESS_PARAM(1001, 0x10000);

static uint8_t heap_area[CELL_SHEAP_MIN_HEAP_SIZE] __attribute__((aligned(128)));

int main(void)
{
    int rc = cellSysmoduleLoadModule(CELL_SYSMODULE_SHEAP);
    printf("load sheap rc=0x%08x\n", rc);
    if (rc != 0 && rc != CELL_SYSMODULE_LOADED) return 1;

    rc = cellSheapInitialize(heap_area, sizeof(heap_area), 0);
    printf("cellSheapInitialize rc=0x%08x\n", rc);
    if (rc == 0) {
        printf("query max=%d free=%d\n",
               cellSheapQueryMax(heap_area),
               cellSheapQueryFree(heap_area));
    }

    cellSysmoduleUnloadModule(CELL_SYSMODULE_SHEAP);
    return rc == 0 ? 0 : 1;
}
