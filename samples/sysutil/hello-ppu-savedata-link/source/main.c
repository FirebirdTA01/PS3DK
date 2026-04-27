/*
 * hello-ppu-savedata-link — link-only test for the full cellSaveData
 * Sony-name surface.  Takes the address of every documented entry
 * point so the linker has to actually resolve each one against the
 * stub archives.  We never call them with valid arguments — running
 * a real save/load callback flow requires user content + RPCS3
 * configuration outside this test's scope.  If the link succeeds and
 * the resulting SELF boots far enough to print "all 39 resolved", the
 * Sony-name FNID dispatch is wired end-to-end.
 */

#include <stdio.h>
#include <stdint.h>

#include <sys/process.h>
#include <cell/sysmodule.h>
#include <cell/sysutil_savedata.h>

SYS_PROCESS_PARAM(1001, 0x10000);

/* Take addresses through a volatile sink so the compiler / linker
 * can't constant-fold the references away. */
static volatile void *g_sink;
#define BIND(fn)  g_sink = (void *)(uintptr_t)(fn)

int main(void)
{
    printf("hello-ppu-savedata-link: cellSaveData link test\n");

    /* Already-existing static-inline forwarders (taking address forces
     * a synthesized real symbol — these still resolve via PSL1GHT). */
    BIND(cellSaveDataAutoLoad2);
    BIND(cellSaveDataAutoSave2);
    BIND(cellSaveDataListLoad2);
    BIND(cellSaveDataListSave2);
    BIND(cellSaveDataFixedLoad2);
    BIND(cellSaveDataFixedSave2);
    BIND(cellSaveDataListAutoLoad);
    BIND(cellSaveDataListAutoSave);
    BIND(cellSaveDataDelete2);

    /* Legacy v1 (libsysutil_savedata_extra_stub.a). */
    BIND(cellSaveDataAutoLoad);
    BIND(cellSaveDataAutoSave);
    BIND(cellSaveDataListLoad);
    BIND(cellSaveDataListSave);
    BIND(cellSaveDataFixedLoad);
    BIND(cellSaveDataFixedSave);
    BIND(cellSaveDataDelete);

    /* Mid-tier (libsysutil_savedata_extra_stub.a). */
    BIND(cellSaveDataEnableOverlay);
    BIND(cellSaveDataFixedDelete);

    /* User-scoped (libsysutil_savedata_extra_stub.a). */
    BIND(cellSaveDataUserAutoLoad);
    BIND(cellSaveDataUserAutoSave);
    BIND(cellSaveDataUserFixedDelete);
    BIND(cellSaveDataUserFixedLoad);
    BIND(cellSaveDataUserFixedSave);
    BIND(cellSaveDataUserListAutoLoad);
    BIND(cellSaveDataUserListAutoSave);
    BIND(cellSaveDataUserListLoad);
    BIND(cellSaveDataUserListSave);

    /* Import / Export / Delete / GetListItem (libsysutil_savedata_stub.a). */
    BIND(cellSaveDataFixedExport);
    BIND(cellSaveDataFixedImport);
    BIND(cellSaveDataGetListItem);
    BIND(cellSaveDataListDelete);
    BIND(cellSaveDataListExport);
    BIND(cellSaveDataListImport);
    BIND(cellSaveDataUserFixedExport);
    BIND(cellSaveDataUserFixedImport);
    BIND(cellSaveDataUserGetListItem);
    BIND(cellSaveDataUserListDelete);
    BIND(cellSaveDataUserListExport);
    BIND(cellSaveDataUserListImport);

    printf("hello-ppu-savedata-link: all 39 resolved\n");
    return 0;
}
