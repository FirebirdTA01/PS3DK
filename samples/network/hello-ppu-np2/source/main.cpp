#include <cstdio>

#include <sys/process.h>

#include <cell/np/np2.h>
#include <cell/sysmodule.h>

SYS_PROCESS_PARAM(1001, 0x10000);

int main(void)
{
    int rc = cellSysmoduleLoadModule(CELL_SYSMODULE_NP2);
    std::printf("load_np2 rc=0x%08x\n", rc);
    if (rc != 0) {
        std::printf("hello-ppu-np2 done\n");
        return 1;
    }

    rc = sce_np2_initialize_default();
    std::printf("np2_init rc=0x%08x\n", rc);
    if (rc != 0) {
        cellSysmoduleUnloadModule(CELL_SYSMODULE_NP2);
        std::printf("hello-ppu-np2 done\n");
        return 1;
    }

    rc = sceNp2Term();
    std::printf("np2_term rc=0x%08x\n", rc);
    cellSysmoduleUnloadModule(CELL_SYSMODULE_NP2);
    std::printf("hello-ppu-np2 done\n");
    return 0;
}
