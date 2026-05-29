#include <cstdio>

#include <sys/process.h>

#include <cell/ssl.h>
#include <cell/sysmodule.h>

SYS_PROCESS_PARAM(1001, 0x10000);

int main(void)
{
    int rc = cellSysmoduleLoadModule(CELL_SYSMODULE_SSL);
    if (rc != 0) {
        std::printf("load rc=0x%08x\n", rc);
        return 1;
    }

    rc = cell_ssl_initialize_default();
    std::printf("init rc=0x%08x\n", rc);
    if (rc != 0) {
        cellSysmoduleUnloadModule(CELL_SYSMODULE_SSL);
        return 1;
    }

    rc = cellSslEnd();
    std::printf("end rc=0x%08x\n", rc);

    cellSysmoduleUnloadModule(CELL_SYSMODULE_SSL);

    std::printf("hello-ppu-ssl done\n");
    return 0;
}
