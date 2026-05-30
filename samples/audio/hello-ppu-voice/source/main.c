#include <stdio.h>
#include <string.h>

#include <sys/process.h>
#include <cell/sysmodule.h>
#include <cell/voice.h>

SYS_PROCESS_PARAM(1001, 0x10000);

int main(void)
{
    int rc = cellSysmoduleLoadModule(CELL_SYSMODULE_VOICE);
    printf("load voice rc=0x%08x\n", rc);
    if (rc != 0 && rc != CELL_SYSMODULE_LOADED) return 1;

    CellVoiceInitParam init;
    memset(&init, 0, sizeof(init));
    init.version = CELLVOICE_VERSION_100;
    init.appType = CELLVOICE_APPTYPE_GAME_1MB;

    rc = cellVoiceInitEx(&init);
    printf("cellVoiceInitEx rc=0x%08x\n", rc);
    if (rc == 0 || rc == CELL_VOICE_ERROR_LIBVOICE_INITIALIZED) {
        int end_rc = cellVoiceEnd();
        printf("cellVoiceEnd rc=0x%08x\n", end_rc);
    }

    cellSysmoduleUnloadModule(CELL_SYSMODULE_VOICE);
    return rc == 0 || rc == CELL_VOICE_ERROR_LIBVOICE_INITIALIZED ? 0 : 1;
}
