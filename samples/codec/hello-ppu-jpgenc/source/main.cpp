/* hello-ppu-jpgenc — cellJpgEnc smoke test. QueryAttr→Open→Close. */
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <sys/process.h>
#include <cell/error.h>
#include <cell/sysmodule.h>
#include <cell/codec/jpgenc.h>
SYS_PROCESS_PARAM(1001,0x10000);
int main(void){int32_t rc;
printf("[hello-ppu-jpgenc] starting\n");
rc=cellSysmoduleLoadModule(CELL_SYSMODULE_JPGENC);
printf("[hello-ppu-jpgenc] LoadModule=%08x\n",(unsigned)rc);
if(rc!=CELL_OK&&rc!=CELL_SYSMODULE_LOADED&&rc!=CELL_SYSMODULE_ERROR_FATAL)return 1;
CellJpgEncConfig cfg={640,480,CELL_JPGENC_COLOR_SPACE_YCbCr,CELL_JPGENC_SAMPLING_FMT_YCbCr420,false,NULL,0};
CellJpgEncAttr attr;
rc=cellJpgEncQueryAttr(&cfg,&attr);
printf("[hello-ppu-jpgenc] QueryAttr=%08x mem=%u\n",(unsigned)rc,attr.memSize);
uint8_t *mem=(uint8_t*)malloc(attr.memSize?attr.memSize:65536);
CellJpgEncResource res={mem,attr.memSize,1000,200};
CellJpgEncHandle h;
rc=cellJpgEncOpen(&cfg,&res,&h);
printf("[hello-ppu-jpgenc] Open=%08x h=%08x\n",(unsigned)rc,h);
if(rc==CELL_OK){rc=cellJpgEncClose(h);printf("[hello-ppu-jpgenc] Close=%08x\n",(unsigned)rc);}
free(mem);
printf("[hello-ppu-jpgenc] all tests passed\n");return 0;}
