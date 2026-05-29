#include <cstdio>

#include <sys/process.h>

#include <cell/libnetctl.h>
#include <cell/sysmodule.h>

SYS_PROCESS_PARAM(1001, 0x10000);

int main(void)
{
    int rc = cellSysmoduleLoadModule(CELL_SYSMODULE_NETCTL);
    if (rc != 0) {
        std::printf("load rc=0x%08x\n", rc);
        return 1;
    }

    rc = cellNetCtlInit();
    if (rc != 0) {
        std::printf("init rc=0x%08x\n", rc);
        cellSysmoduleUnloadModule(CELL_SYSMODULE_NETCTL);
        return 1;
    }

    int state = -1;
    rc = cellNetCtlGetState(&state);
    std::printf("state rc=0x%08x state=%d\n", rc, state);

    union CellNetCtlInfo info = {};
    rc = cellNetCtlGetInfo(CELL_NET_CTL_INFO_DEVICE, &info);
    std::printf("device rc=0x%08x device=%u\n", rc, (unsigned)info.device);

    cellNetCtlTerm();
    cellSysmoduleUnloadModule(CELL_SYSMODULE_NETCTL);

    std::printf("hello-ppu-netctl done\n");
    return 0;
}
