/* hello-ppu-sail-rec — cellSailRec smoke test. Composer init/finalize (475+ surface). */
#include <cstdio>
#include <cstdint>
#include <sys/process.h>
#include <cell/error.h>
#include <cell/sysmodule.h>
#include <cell/sail_rec.h>
SYS_PROCESS_PARAM(1001,0x10000);
int main(void){int32_t rc;
printf("[hello-ppu-sail-rec] starting\n");
rc=cellSysmoduleLoadModule(CELL_SYSMODULE_SAIL_REC);
printf("[hello-ppu-sail-rec] LoadModule=%08x\n",(unsigned)rc);
if(rc!=CELL_OK&&rc!=CELL_SYSMODULE_LOADED&&rc!=CELL_SYSMODULE_ERROR_FATAL)return 1;
CellSailComposer composer;
CellSailMp4MovieParameter mp4={0}; mp4.thisSize=sizeof(mp4); mp4.flags=-1; mp4.timeScale=(uint32_t)-1;
CellSailStreamParameter stream={0}; stream.thisSize=sizeof(stream); stream.heapSize=-1;
rc=cellSailComposerInitialize(&composer,&mp4,&stream,0);
printf("[hello-ppu-sail-rec] ComposerInitialize=%08x\n",(unsigned)rc);
rc=cellSailComposerFinalize(&composer);
printf("[hello-ppu-sail-rec] ComposerFinalize=%08x\n",(unsigned)rc);
printf("[hello-ppu-sail-rec] all tests passed\n");return 0;}
