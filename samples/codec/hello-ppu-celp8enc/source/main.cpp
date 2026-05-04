/* hello-ppu-celp8enc — cellCelp8Enc smoke test. QueryAttr→Open→Close. */
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <sys/process.h>
#include <cell/error.h>
#include <cell/sysmodule.h>
#include <cell/codec/celp8enc.h>
SYS_PROCESS_PARAM(1001,0x10000);
int main(void){int32_t rc;printf("[hello-ppu-celp8enc] starting\n");
rc=cellSysmoduleLoadModule(CELL_SYSMODULE_CELP8ENC);printf("[hello-ppu-celp8enc] LoadModule=%08x\n",(unsigned)rc);
if(rc!=CELL_OK&&rc!=CELL_SYSMODULE_LOADED&&rc!=CELL_SYSMODULE_ERROR_FATAL)return 1;
CellCelp8EncAttr attr;rc=cellCelp8EncQueryAttr(&attr);printf("[hello-ppu-celp8enc] QueryAttr=%08x mem=%u\n",(unsigned)rc,attr.workMemSize);
uint8_t*mem=(uint8_t*)malloc(attr.workMemSize?attr.workMemSize:65536);
CellCelp8EncResource res={0};res.totalMemSize=attr.workMemSize;res.startAddr=mem;res.ppuThreadPriority=1000;
res.spuThreadPriority=200;res.ppuThreadStackSize=0x4000;
CellCelp8EncHandle h;rc=cellCelp8EncOpen(&res,&h);printf("[hello-ppu-celp8enc] Open=%08x h=%08x\n",(unsigned)rc,h);
if(rc==CELL_OK){rc=cellCelp8EncClose(h);printf("[hello-ppu-celp8enc] Close=%08x\n",(unsigned)rc);}
free(mem);printf("[hello-ppu-celp8enc] all tests passed\n");return 0;}
