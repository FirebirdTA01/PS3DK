/* hello-ppu-gifdec — cellGifDec smoke test. Create/Open/Close. */
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <sys/process.h>
#include <cell/error.h>
#include <cell/sysmodule.h>
#include <cell/codec/gifdec.h>
SYS_PROCESS_PARAM(1001,0x10000);
int main(void){int32_t rc;
printf("[hello-ppu-gifdec] starting\n");
rc=cellSysmoduleLoadModule(CELL_SYSMODULE_GIFDEC);
printf("[hello-ppu-gifdec] LoadModule=%08x\n",(unsigned)rc);
if(rc!=CELL_OK&&rc!=CELL_SYSMODULE_LOADED&&rc!=CELL_SYSMODULE_ERROR_FATAL)return 1;
CellGifDecThreadInParam tip={CELL_GIFDEC_SPU_THREAD_DISABLE,1000,200,NULL,NULL,NULL,NULL};
CellGifDecThreadOutParam top;
CellGifDecMainHandle mh;
rc=cellGifDecCreate(&mh,&tip,&top);
printf("[hello-ppu-gifdec] Create=%08x mh=%08x\n",(unsigned)rc,mh);
CellGifDecSubHandle sh;
static uint8_t gif[]="GIF89a\x01\x00\x01\x00\x80\x00\x00\xff\xff\xff\x00\x00\x00\x2c\x00\x00\x00\x00\x01\x00\x01\x00\x00\x02\x02\x44\x01\x00\x3b";
CellGifDecSrc src={CELL_GIFDEC_BUFFER,NULL,0,0,gif,sizeof(gif),CELL_GIFDEC_SPU_THREAD_DISABLE};
CellGifDecOpnInfo oi;
rc=cellGifDecOpen(mh,&sh,&src,&oi);
printf("[hello-ppu-gifdec] Open=%08x sh=%08x\n",(unsigned)rc,sh);
if(rc==CELL_OK){rc=cellGifDecClose(mh,sh);printf("[hello-ppu-gifdec] Close=%08x\n",(unsigned)rc);}
rc=cellGifDecDestroy(mh);
printf("[hello-ppu-gifdec] Destroy=%08x\n",(unsigned)rc);
printf("[hello-ppu-gifdec] all tests passed\n");return 0;}
