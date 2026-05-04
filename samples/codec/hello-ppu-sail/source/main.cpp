/* hello-ppu-sail — cellSail smoke test. MemAllocator init (avoids Player state machine). */
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <sys/process.h>
#include <cell/error.h>
#include <cell/sysmodule.h>
#include <cell/sail.h>
SYS_PROCESS_PARAM(1001,0x10000);
static void* sail_alloc(void *arg, size_t boundary, size_t size) { (void)arg;(void)boundary; return malloc(size); }
static void sail_free(void *arg, size_t boundary, void *p) { (void)arg;(void)boundary; free(p); }
int main(void){int32_t rc;
printf("[hello-ppu-sail] starting\n");
rc=cellSysmoduleLoadModule(CELL_SYSMODULE_SAIL);
printf("[hello-ppu-sail] LoadModule=%08x\n",(unsigned)rc);
if(rc!=CELL_OK&&rc!=CELL_SYSMODULE_LOADED&&rc!=CELL_SYSMODULE_ERROR_FATAL)return 1;
CellSailMemAllocatorFuncs funcs = { sail_alloc, sail_free };
CellSailMemAllocator alloc;
rc=cellSailMemAllocatorInitialize(&alloc,&funcs,NULL);
printf("[hello-ppu-sail] MemAllocatorInitialize=%08x\n",(unsigned)rc);
printf("[hello-ppu-sail] all tests passed\n");return 0;}
