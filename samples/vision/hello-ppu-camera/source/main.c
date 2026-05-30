#include <stdio.h>

#include <sys/process.h>
#include <cell/camera.h>
#include <cell/sysmodule.h>

SYS_PROCESS_PARAM(1001, 0x10000);

int main(void)
{
    int rc = cellSysmoduleLoadModule(CELL_SYSMODULE_CAMERA);
    printf("cellSysmoduleLoadModule(CAMERA) -> 0x%08x\n", (unsigned)rc);
    if (rc < 0) {
        return 1;
    }

    rc = cellCameraInit();
    printf("cellCameraInit -> 0x%08x\n", (unsigned)rc);

    if (rc == CELL_OK || rc == CELL_CAMERA_ERROR_ALREADY_INIT) {
        int end_rc = cellCameraEnd();
        printf("cellCameraEnd -> 0x%08x\n", (unsigned)end_rc);
    }

    cellSysmoduleUnloadModule(CELL_SYSMODULE_CAMERA);
    return 0;
}
