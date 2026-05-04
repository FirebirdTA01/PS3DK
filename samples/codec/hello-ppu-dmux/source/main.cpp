#include <cstdio>
#include <cstdint>
#include <cstdlib>

#include <sys/process.h>

#include <cell/codec/dmux.h>
#include <cell/sysmodule.h>

SYS_PROCESS_PARAM(1001, 0x10000);

/* PAMF demuxer requires a message callback; the SPRX dispatches
 * DEMUX_DONE / FATAL_ERR / PROG_END_CODE through it. */
static uint32_t dmux_msg_cb(
    CellDmuxHandle h, const CellDmuxMsg *msg, void *arg)
{
    (void)h; (void)msg; (void)arg;
    return CELL_OK;
}

int main(void)
{
    int32_t rc;
    CellDmuxAttr attr;

    printf("[hello-ppu-dmux] starting\n");

    /* Load sysmodules — the PAMF backend must be loaded for
     * cellDmuxOpen to find its core-ops export.  Tolerate
     * CELL_SYSMODULE_LOADED (0) and CELL_SYSMODULE_ERROR_FATAL
     * (0x80012003) which mean the module was already present. */
    rc = cellSysmoduleLoadModule(CELL_SYSMODULE_DMUX);
    printf("[hello-ppu-dmux] cellSysmoduleLoadModule(DMUX) = %08x\n", (unsigned)rc);
    if (rc != CELL_OK && rc != CELL_SYSMODULE_LOADED && rc != CELL_SYSMODULE_ERROR_FATAL)
        return 1;

    rc = cellSysmoduleLoadModule(CELL_SYSMODULE_DMUX_PAMF);
    printf("[hello-ppu-dmux] cellSysmoduleLoadModule(DMUX_PAMF) = %08x\n", (unsigned)rc);
    if (rc != CELL_OK && rc != CELL_SYSMODULE_LOADED && rc != CELL_SYSMODULE_ERROR_FATAL)
        return 1;

    /*
     * Test 1: QueryAttr — ask the SPRX how much memory it needs for
     * a PAMF demuxer.  No stream data required.
     */
    CellDmuxType type;
    type.streamType = CELL_DMUX_STREAM_TYPE_PAMF;
    type.reserved1   = 0;
    type.reserved2   = 0;

    rc = cellDmuxQueryAttr(&type, &attr);
    if (rc != CELL_OK) {
        printf("[hello-ppu-dmux] FAIL: cellDmuxQueryAttr returned %08x\n", (unsigned)rc);
        return 1;
    }
    printf("[hello-ppu-dmux] cellDmuxQueryAttr OK — memSize=%u, ver=%u.%u\n",
           attr.memSize, attr.demuxerVerUpper, attr.demuxerVerLower);

    /*
     * Test 2: QueryAttr2 — extended type structure (same result for PAMF).
     */
    CellDmuxType2 type2;
    type2.streamType         = CELL_DMUX_STREAM_TYPE_PAMF;
    type2.streamSpecificInfo = NULL;

    CellDmuxAttr attr2;
    rc = cellDmuxQueryAttr2(&type2, &attr2);
    if (rc != CELL_OK) {
        printf("[hello-ppu-dmux] FAIL: cellDmuxQueryAttr2 returned %08x\n", (unsigned)rc);
        return 1;
    }
    printf("[hello-ppu-dmux] cellDmuxQueryAttr2 OK — memSize=%u\n", attr2.memSize);

    /*
     * Test 3: Open → instantiate a demuxer.
     */
    uint8_t *mem = (uint8_t *)malloc(attr.memSize);
    if (!mem) {
        printf("[hello-ppu-dmux] FAIL: malloc(%u) returned NULL\n", attr.memSize);
        return 1;
    }

    CellDmuxResource resource;
    resource.memAddr           = mem;
    resource.memSize           = attr.memSize;
    resource.ppuThreadPriority = 1000;
    resource.ppuThreadStackSize = 0x4000;
    resource.spuThreadPriority = 200;
    resource.numOfSpus          = 1;

    CellDmuxCb cb;
    cb.cbFunc = dmux_msg_cb;
    cb.cbArg  = NULL;

    CellDmuxHandle handle;
    rc = cellDmuxOpen(&type, &resource, &cb, &handle);
    if (rc != CELL_OK) {
        printf("[hello-ppu-dmux] FAIL: cellDmuxOpen returned %08x\n", (unsigned)rc);
        free(mem);
        return 1;
    }
    printf("[hello-ppu-dmux] cellDmuxOpen OK — handle=0x%08x\n", handle);

    /*
     * Test 4: Close — tear down the demuxer.
     */
    rc = cellDmuxClose(handle);
    if (rc != CELL_OK) {
        printf("[hello-ppu-dmux] FAIL: cellDmuxClose returned %08x\n", (unsigned)rc);
        free(mem);
        return 1;
    }
    printf("[hello-ppu-dmux] cellDmuxClose OK\n");

    free(mem);

    printf("[hello-ppu-dmux] all tests passed\n");
    return 0;
}
