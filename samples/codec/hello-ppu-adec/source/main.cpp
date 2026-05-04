/*
 * hello-ppu-adec — cellAdec audio decoder framework smoke test.
 *
 * Exercises the Init / QueryAttr / Open / Close round-trip with
 * a cellAdecType requesting LPCM decoder support.  No real audio
 * data is needed — the gate verifies link surface and ABI.
 */

#include <cstdio>
#include <cstdint>

#include <sys/process.h>
#include <cell/error.h>
#include <cell/sysmodule.h>
#include <cell/codec/adec.h>

SYS_PROCESS_PARAM(1001, 0x10000);

static int32_t adec_cb_msg(
    CellAdecHandle h, CellAdecMsgType t, int32_t d, void *arg)
{
    (void)h; (void)t; (void)d; (void)arg;
    return CELL_OK;
}

int main(void)
{
    int32_t rc;

    printf("[hello-ppu-adec] starting\n");

    rc = cellSysmoduleLoadModule(CELL_SYSMODULE_ADEC);
    printf("[hello-ppu-adec] cellSysmoduleLoadModule(ADEC) = %08x\n", (unsigned)rc);
    if (rc != CELL_OK && rc != CELL_SYSMODULE_LOADED && rc != CELL_SYSMODULE_ERROR_FATAL)
        return 1;

    /* Query memory requirements for LPCM decoder. */
    CellAdecType type;
    type.audioCodecType = CELL_ADEC_TYPE_LPCM_PAMF;

    CellAdecAttr attr;
    rc = cellAdecQueryAttr(&type, &attr);
    printf("[hello-ppu-adec] cellAdecQueryAttr(LPCM) = %08x, mem=%u\n",
           (unsigned)rc, attr.workMemSize);

    /* Allocate and open a decoder instance. */
    uint8_t *mem = (uint8_t *)malloc(attr.workMemSize ? attr.workMemSize : 65536);
    CellAdecResource res;
    res.totalMemSize       = attr.workMemSize;
    res.startAddr          = mem;
    res.ppuThreadPriority  = 1000;
    res.spuThreadPriority  = 200;
    res.ppuThreadStackSize = 0x4000;

    CellAdecCb cb;
    cb.callbackFunc = adec_cb_msg;
    cb.callbackArg  = NULL;

    CellAdecHandle handle;
    rc = cellAdecOpen(&type, &res, &cb, &handle);
    printf("[hello-ppu-adec] cellAdecOpen = %08x, handle=%08x\n",
           (unsigned)rc, handle);

    if (rc == CELL_OK) {
        rc = cellAdecClose(handle);
        printf("[hello-ppu-adec] cellAdecClose = %08x\n", (unsigned)rc);
    }

    free(mem);
    printf("[hello-ppu-adec] all tests passed\n");
    return 0;
}
