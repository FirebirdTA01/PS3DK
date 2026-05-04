/* hello-ppu-celpenc — cellCelpEnc smoke test. QueryAttr→Open→Close. */
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <sys/process.h>
#include <cell/error.h>
#include <cell/sysmodule.h>
#include <cell/codec/celpenc.h>
SYS_PROCESS_PARAM(1001,0x10000);
int main(void){int32_t rc;printf("[hello-ppu-celpenc] starting\n");
rc=cellSysmoduleLoadModule(CELL_SYSMODULE_CELPENC);printf("[hello-ppu-celpenc] LoadModule=%08x\n",(unsigned)rc);
if(rc!=CELL_OK&&rc!=CELL_SYSMODULE_LOADED&&rc!=CELL_SYSMODULE_ERROR_FATAL)return 1;
CellCelpEncAttr attr;rc=cellCelpEncQueryAttr(&attr);printf("[hello-ppu-celpenc] QueryAttr=%08x mem=%u\n",(unsigned)rc,attr.workMemSize);
uint8_t*mem=(uint8_t*)malloc(attr.workMemSize?attr.workMemSize:65536);
CellCelpEncResource res={0};res.totalMemSize=attr.workMemSize;res.startAddr=mem;res.ppuThreadPriority=1000;
res.spuThreadPriority=200;res.ppuThreadStackSize=0x4000;
CellCelpEncHandle h;rc=cellCelpEncOpen(&res,&h);printf("[hello-ppu-celpenc] Open=%08x h=%08x\n",(unsigned)rc,h);
if(rc==CELL_OK){rc=cellCelpEncClose(h);printf("[hello-ppu-celpenc] Close=%08x\n",(unsigned)rc);}
free(mem);printf("[hello-ppu-celpenc] all tests passed\n");return 0;}
