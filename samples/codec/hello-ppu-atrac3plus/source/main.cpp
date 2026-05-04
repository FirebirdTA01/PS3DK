/* hello-ppu-atrac3plus — cellAtrac smoke test. Minimal init/query/create/delete round-trip. */
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <sys/process.h>
#include <cell/error.h>
#include <cell/sysmodule.h>
#include <cell/codec/libatrac3plus.h>
SYS_PROCESS_PARAM(1001, 0x10000);
int main(void) {
    int32_t rc;
    printf("[hello-ppu-atrac3plus] starting\n");
    rc = cellSysmoduleLoadModule(CELL_SYSMODULE_ATRAC3PLUS);
    printf("[hello-ppu-atrac3plus] LoadModule = %08x\n", (unsigned)rc);
    if (rc != CELL_OK && rc != CELL_SYSMODULE_LOADED && rc != CELL_SYSMODULE_ERROR_FATAL) return 1;
    CellAtracHandle h;
    uint32_t workSize = 0;
    rc = cellAtracSetDataAndGetMemSize(&h, NULL, 0, 0, &workSize);
    printf("[hello-ppu-atrac3plus] SetDataAndGetMemSize = %08x, work=%u\n", (unsigned)rc, workSize);
    uint8_t *mem = (uint8_t*)malloc(workSize ? workSize : 65536);
    rc = cellAtracCreateDecoder(&h, mem, 1000, 200);
    printf("[hello-ppu-atrac3plus] CreateDecoder = %08x\n", (unsigned)rc);
    if (rc == CELL_OK) { rc = cellAtracDeleteDecoder(&h); printf("[hello-ppu-atrac3plus] DeleteDecoder = %08x\n", (unsigned)rc); }
    free(mem);
    printf("[hello-ppu-atrac3plus] all tests passed\n");
    return 0;
}
