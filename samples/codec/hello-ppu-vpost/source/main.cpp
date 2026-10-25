/*
 * hello-ppu-vpost — cellVpost Post-Processing validation test.
 * QueryAttr → Open → Close round-trip.
 */
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <malloc.h>
#include <sys/process.h>
#include <cell/error.h>
#include <cell/sysmodule.h>
#include <cell/vpost.h>
SYS_PROCESS_PARAM(1001, 0x10000);
int main(void) {
    int32_t rc;
    printf("[hello-ppu-vpost] starting\n");
    rc = cellSysmoduleLoadModule(CELL_SYSMODULE_VPOST);
    printf("[hello-ppu-vpost] LoadModule = %08x\n", (unsigned)rc);
    if (rc != CELL_OK && rc != CELL_SYSMODULE_LOADED) return 1;
    CellVpostCfgParam cfg = {0};
    cfg.inMaxWidth = 1920; cfg.inMaxHeight = 1080;
    cfg.inDepth = CELL_VPOST_PIC_DEPTH_8;
    cfg.inPicFmt = CELL_VPOST_PIC_FMT_IN_YUV420_PLANAR;
    cfg.outMaxWidth = 1920; cfg.outMaxHeight = 1080;
    cfg.outDepth = CELL_VPOST_PIC_DEPTH_8;
    cfg.outPicFmt = CELL_VPOST_PIC_FMT_OUT_RGBA_ILV;
    CellVpostAttr attr = {};
    rc = cellVpostQueryAttr(&cfg, &attr);
    printf("[hello-ppu-vpost] QueryAttr = %08x, mem=%u\n", (unsigned)rc, attr.memSize);
    if (rc != CELL_OK || attr.memSize == 0) return 1;
    uint8_t *mem = (uint8_t*)memalign(128, attr.memSize);
    if (!mem) { puts("[hello-ppu-vpost] FAIL work allocation"); return 1; }
    CellVpostResource res = {};
    res.memAddr = mem; res.memSize = attr.memSize;
    res.ppuThreadPriority = 1000; res.ppuThreadStackSize = 0x4000;
    res.spuThreadPriority = 200; res.numOfSpus = 1;
    CellVpostHandle h = 0;
    rc = cellVpostOpen(&cfg, &res, &h);
    printf("[hello-ppu-vpost] Open = %08x, h=%08x\n", (unsigned)rc, h);
    bool ok = rc == CELL_OK;
    if (ok) {
        rc = cellVpostClose(h);
        printf("[hello-ppu-vpost] Close = %08x\n", (unsigned)rc);
        ok = rc == CELL_OK;
    }
    free(mem);
    printf("[hello-ppu-vpost] lifecycle %s (no frame processing)\n", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}
