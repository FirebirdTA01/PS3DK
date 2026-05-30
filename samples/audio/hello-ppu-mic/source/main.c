#include <stdio.h>

#include <sys/process.h>
#include <cell/mic.h>
#include <cell/sysmodule.h>

SYS_PROCESS_PARAM(1001, 0x10000);

int main(void)
{
    int rc = cellSysmoduleLoadModule(CELL_SYSMODULE_MIC);
    printf("load mic rc=0x%08x\n", rc);
    if (rc != 0 && rc != CELL_SYSMODULE_LOADED) return 1;

    rc = cellMicInit();
    printf("cellMicInit rc=0x%08x\n", rc);
    if (rc == 0 || rc == CELL_MICIN_ERROR_ALREADY_INIT) {
        int end_rc = cellMicEnd();
        printf("cellMicEnd rc=0x%08x\n", end_rc);
    }

    cellSysmoduleUnloadModule(CELL_SYSMODULE_MIC);
    return rc == 0 || rc == CELL_MICIN_ERROR_ALREADY_INIT ? 0 : 1;
}
