#include <stdio.h>

#include <sys/process.h>
#include <cell/dbgrsx.h>

SYS_PROCESS_PARAM(1001, 0x10000);

int main(void)
{
    int32_t rc = cellDbgRsxInit();
    printf("cellDbgRsxInit -> 0x%08x\n", (unsigned)rc);
    if (rc != CELL_OK) {
        return 1;
    }

    CellDbgRsxZcullRegion region;
    rc = cellDbgRsxGetZcullRegion(0, &region);
    printf("cellDbgRsxGetZcullRegion -> 0x%08x\n", (unsigned)rc);

    cellDbgRsxExit();
    return rc == CELL_OK ? 0 : 1;
}
