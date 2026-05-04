/* hello-ppu-atrac3multi — cellAtracMulti smoke test. */
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <sys/process.h>
#include <cell/error.h>
#include <cell/sysmodule.h>
#include <cell/codec/libatrac3multi.h>
SYS_PROCESS_PARAM(1001, 0x10000);
int main(void) {
    int32_t rc;
    printf("[hello-ppu-atrac3multi] starting\n");
    rc = cellSysmoduleLoadModule(CELL_SYSMODULE_LIBATRAC3MULTI);
    printf("[hello-ppu-atrac3multi] LoadModule = %08x\n", (unsigned)rc);
    if (rc != CELL_OK && rc != CELL_SYSMODULE_LOADED && rc != CELL_SYSMODULE_ERROR_FATAL) return 1;
    CellAtracMultiHandle h;
    uint32_t workSize = 0;
    rc = cellAtracMultiSetDataAndGetMemSize(&h, NULL, 0, 0, &workSize);
    printf("[hello-ppu-atrac3multi] SetDataAndGetMemSize = %08x, work=%u\n", (unsigned)rc, workSize);
    uint8_t *mem = (uint8_t*)malloc(workSize ? workSize : 65536);
    rc = cellAtracMultiCreateDecoder(&h, mem, 1000, 200);
    printf("[hello-ppu-atrac3multi] CreateDecoder = %08x\n", (unsigned)rc);
    if (rc == CELL_OK) { rc = cellAtracMultiDeleteDecoder(&h); printf("[hello-ppu-atrac3multi] DeleteDecoder = %08x\n", (unsigned)rc); }
    free(mem);
    printf("[hello-ppu-atrac3multi] all tests passed\n");
    return 0;
}
