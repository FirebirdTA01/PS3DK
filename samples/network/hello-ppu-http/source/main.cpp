#include <cstdio>

#include <sys/process.h>

#include <cell/sysmodule.h>

SYS_PROCESS_PARAM(1001, 0x10000);

int main(void)
{
    int rc = cellSysmoduleLoadModule(CELL_SYSMODULE_NET);
    std::printf("load_net rc=0x%08x\n", rc);
    if (rc == 0) {
        rc = cellSysmoduleLoadModule(CELL_SYSMODULE_HTTP);
        std::printf("load_http rc=0x%08x\n", rc);
        if (rc == 0) {
            cellSysmoduleUnloadModule(CELL_SYSMODULE_HTTP);
        }
        cellSysmoduleUnloadModule(CELL_SYSMODULE_NET);
    }

    std::printf("hello-ppu-http done\n");
    return 0;
}
