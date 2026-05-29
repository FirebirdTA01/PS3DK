#include <cstdio>

#include <sys/process.h>

#include <cell/libnet.h>
#include <cell/sysmodule.h>

SYS_PROCESS_PARAM(1001, 0x10000);

int main(void)
{
    int rc = cellSysmoduleLoadModule(CELL_SYSMODULE_NET);
    if (rc != 0) {
        std::printf("load rc=0x%08x\n", rc);
        return 1;
    }

    rc = sys_net_initialize_network();
    std::printf("init rc=0x%08x\n", rc);
    if (rc != 0) {
        cellSysmoduleUnloadModule(CELL_SYSMODULE_NET);
        return 1;
    }

    struct in_addr primary = {};
    struct in_addr secondary = {};
    rc = sys_net_get_lib_name_server(&primary, &secondary);
    std::printf("get_lib_name_server rc=0x%08x primary=0x%08x secondary=0x%08x\n",
                rc, (unsigned)primary.s_addr, (unsigned)secondary.s_addr);

    rc = sys_net_finalize_network();
    std::printf("finalize rc=0x%08x\n", rc);

    cellSysmoduleUnloadModule(CELL_SYSMODULE_NET);

    std::printf("hello-ppu-libnet done\n");
    return 0;
}
