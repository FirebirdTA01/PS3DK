#include <stdio.h>

#include <sys/process.h>
#include <cell/rudp.h>
#include <cell/sysmodule.h>

SYS_PROCESS_PARAM(1001, 0x10000);

int main(void)
{
    int rc = cellSysmoduleLoadModule(CELL_SYSMODULE_RUDP);
    printf("load rudp rc=0x%08x\n", rc);
    if (rc != 0 && rc != CELL_SYSMODULE_LOADED) return 1;

    rc = cellRudpInit(NULL);
    printf("cellRudpInit rc=0x%08x\n", rc);
    if (rc == 0 || rc == CELL_RUDP_ERROR_ALREADY_INITIALIZED) {
        int end_rc = cellRudpEnd();
        printf("cellRudpEnd rc=0x%08x\n", end_rc);
    }

    cellSysmoduleUnloadModule(CELL_SYSMODULE_RUDP);
    return rc == 0 || rc == CELL_RUDP_ERROR_ALREADY_INITIALIZED ? 0 : 1;
}
