#include <cstdio>

#include <sys/process.h>

#include <cell/sysmodule.h>

SYS_PROCESS_PARAM(1001, 0x10000);

int main(void)
{
    int rc = cellSysmoduleLoadModule(CELL_SYSMODULE_TUS);
    std::printf("load_tus rc=0x%08x\n", rc);
    if (rc == 0) {
        cellSysmoduleUnloadModule(CELL_SYSMODULE_TUS);
    }
    std::printf("hello-ppu-np-tus done\n");
    return 0;
}
