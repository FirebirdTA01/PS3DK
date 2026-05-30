#include <stdio.h>

#include <sys/process.h>
#include <cell/resc.h>
#include <cell/sysmodule.h>

SYS_PROCESS_PARAM(1001, 0x10000);

int main(void)
{
    CellRescBufferMode mode = CELL_RESC_UNDEFINED;

    int rc = cellSysmoduleLoadModule(CELL_SYSMODULE_RESC);
    printf("cellSysmoduleLoadModule(RESC) -> 0x%08x\n", (unsigned)rc);
    if (rc < 0) {
        return 1;
    }

    rc = cellRescVideoOutResolutionId2RescBufferMode(CELL_VIDEO_OUT_RESOLUTION_720,
                                                     &mode);
    printf("cellRescVideoOutResolutionId2RescBufferMode -> 0x%08x mode=%u\n",
           (unsigned)rc, (unsigned)mode);

    cellSysmoduleUnloadModule(CELL_SYSMODULE_RESC);
    return 0;
}
