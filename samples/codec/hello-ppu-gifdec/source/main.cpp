/* hello-ppu-gifdec — cellGifDec validation test. Create/Open/Close. */
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <sys/process.h>
#include <cell/error.h>
#include <cell/sysmodule.h>
#include <cell/codec/gifdec.h>
SYS_PROCESS_PARAM(1001, 0x10000);

static void *allocate(uint32_t bytes, void *)
{
    return std::malloc(bytes);
}

static int32_t release(void *ptr, void *)
{
    std::free(ptr);
    return 0;
}

static bool checked(const char *operation, int32_t rc)
{
    std::printf("[hello-ppu-gifdec] %s=%08x%s\n", operation,
                (unsigned)rc, rc == CELL_OK ? "" : " FAIL");
    return rc == CELL_OK;
}

int main(void)
{
    std::puts("[hello-ppu-gifdec] starting");
    int32_t rc = cellSysmoduleLoadModule(CELL_SYSMODULE_GIFDEC);
    std::printf("[hello-ppu-gifdec] LoadModule=%08x\n", (unsigned)rc);
    if (rc != CELL_OK && rc != CELL_SYSMODULE_LOADED) return 1;

    /* The decoder requires allocation callbacks even with SPU use disabled. */
    CellGifDecThreadInParam tip = {
        CELL_GIFDEC_SPU_THREAD_DISABLE, 1000, 200,
        allocate, NULL, release, NULL
    };
    CellGifDecThreadOutParam top = {};
    CellGifDecMainHandle mh = 0;
    if (!checked("Create", cellGifDecCreate(&mh, &tip, &top))) return 1;

    static uint8_t gif[] =
        "GIF89a\x01\x00\x01\x00\x80\x00\x00\xff\xff\xff\x00\x00\x00"
        "\x2c\x00\x00\x00\x00\x01\x00\x01\x00\x00\x02\x02\x44\x01\x00\x3b";
    CellGifDecSrc src = {
        CELL_GIFDEC_BUFFER, NULL, 0, 0, gif, sizeof(gif) - 1,
        CELL_GIFDEC_SPU_THREAD_DISABLE
    };
    CellGifDecSubHandle sh = 0;
    CellGifDecOpnInfo oi = {};
    bool ok = checked("Open", cellGifDecOpen(mh, &sh, &src, &oi));
    if (ok) {
        CellGifDecInfo info = {};
        ok = checked("ReadHeader", cellGifDecReadHeader(mh, sh, &info));
        if (ok) {
            std::printf("[hello-ppu-gifdec] dimensions=%ux%u\n",
                        info.SWidth, info.SHeight);
            ok = info.SWidth == 1 && info.SHeight == 1;
        }
        const bool closed = checked("Close", cellGifDecClose(mh, sh));
        ok = ok && closed;
    }
    const bool destroyed = checked("Destroy", cellGifDecDestroy(mh));
    ok = ok && destroyed;
    std::puts(ok ? "[hello-ppu-gifdec] all tests passed"
                 : "[hello-ppu-gifdec] FAIL");
    return ok ? 0 : 1;
}
