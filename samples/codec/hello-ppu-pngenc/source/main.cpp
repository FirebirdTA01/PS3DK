/* hello-ppu-pngenc — cellPngEnc smoke test. QueryAttr→Open→Close. */
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <sys/process.h>
#include <cell/error.h>
#include <cell/sysmodule.h>
#include <cell/codec/pngenc.h>
SYS_PROCESS_PARAM(1001,0x10000);
int main(void){int32_t rc;
printf("[hello-ppu-pngenc] starting\n");
rc=cellSysmoduleLoadModule(CELL_SYSMODULE_PNGENC);
printf("[hello-ppu-pngenc] LoadModule=%08x\n",(unsigned)rc);
if(rc!=CELL_OK&&rc!=CELL_SYSMODULE_LOADED&&rc!=CELL_SYSMODULE_ERROR_FATAL)return 1;
CellPngEncConfig cfg={640,480,8,false,0,NULL,0};
CellPngEncAttr attr;
rc=cellPngEncQueryAttr(&cfg,&attr);
printf("[hello-ppu-pngenc] QueryAttr=%08x mem=%u\n",(unsigned)rc,attr.memSize);
uint8_t *mem=(uint8_t*)malloc(attr.memSize?attr.memSize:65536);
CellPngEncResource res={mem,attr.memSize,1000,200};
CellPngEncHandle h;
rc=cellPngEncOpen(&cfg,&res,&h);
printf("[hello-ppu-pngenc] Open=%08x h=%08x\n",(unsigned)rc,h);
if(rc==CELL_OK){rc=cellPngEncClose(h);printf("[hello-ppu-pngenc] Close=%08x\n",(unsigned)rc);}
free(mem);
printf("[hello-ppu-pngenc] all tests passed\n");return 0;}
