/*
 * hello-ppu-spurs-core - minimal Spurs bring-up + thread enumeration.
 *
 * Exercises the cell::Spurs:: C++ class surface in <cell/spurs.h> by
 * standing up a 4-SPU Spurs2 instance without a preemption context
 * (EXCLUSIVE_NON_CONTEXT), enumerating its SPU thread IDs, and
 * finalising.  No SPU-side code - this is purely a PPU scheduler
 * smoke test that proves our clean-room Spurs header matches the
 * libspurs SPRX's ABI expectations end-to-end.
 *
 * Expected output:
 *   hello-ppu-spurs-core: Spurs bring-up smoke test
 *     spu_printf_initialize ok
 *     SpursAttribute::initialize ok
 *     Spurs2::initialize ok
 *     4 SPU threads:
 *       [0]   0x.......
 *       [1]   ...
 *     finalize ok
 *   SUCCESS
 */

#include <cstdio>
#include <cstring>
#include <cstdlib>

#include <sys/process.h>
#include <sys/spu_thread.h>
#include <sys/spu_thread_group.h>

#include <cell/spurs.h>
#include <spu_printf.h>

SYS_PROCESS_PARAM(1001, 0x10000);

/* Attribute tuning matching Sony's reference-sample defaults.  The
 * 4-SPU size leaves 2 for the system, so RPCS3 (which emulates 6 SPUs)
 * can scheduler us without contention. */
static const unsigned int kNumSpu            = 4;
static const int          kPpuThreadPriority = 2;
static const int          kSpuThreadPriority = 100;
static const char         kNamePrefix[]      = "HelloPpu";
static const int          kSpuPrintfPriority = 999;

static bool init_spurs(cell::Spurs::Spurs2 *spurs)
{
    cell::Spurs::SpursAttribute attr;

    int rc = cell::Spurs::SpursAttribute::initialize(
        &attr, kNumSpu, kSpuThreadPriority, kPpuThreadPriority,
        /* exitIfNoWork = */ false);
    if (rc) {
        std::printf("  SpursAttribute::initialize: %#x\n", rc);
        return false;
    }
    std::printf("  SpursAttribute::initialize ok\n");

    rc = attr.setNamePrefix(kNamePrefix, std::strlen(kNamePrefix));
    if (rc) { std::printf("  setNamePrefix: %#x\n", rc); return false; }

    rc = attr.setSpuThreadGroupType(SYS_SPU_THREAD_GROUP_TYPE_EXCLUSIVE_NON_CONTEXT);
    if (rc) { std::printf("  setSpuThreadGroupType: %#x\n", rc); return false; }

    rc = attr.enableSpuPrintfIfAvailable();
    if (rc) { std::printf("  enableSpuPrintfIfAvailable: %#x\n", rc); return false; }

    rc = cell::Spurs::Spurs2::initialize(spurs, &attr);
    if (rc) { std::printf("  Spurs2::initialize: %#x\n", rc); return false; }
    std::printf("  Spurs2::initialize ok\n");

    return true;
}

static void print_spurs_info(cell::Spurs::Spurs2 *spurs)
{
    unsigned int n = 0;
    int rc = spurs->getNumSpuThread(&n);
    if (rc) { std::printf("  getNumSpuThread: %#x\n", rc); return; }

    sys_spu_thread_t ids[CELL_SPURS_MAX_SPU];
    rc = spurs->getSpuThreadId(ids, &n);
    if (rc) { std::printf("  getSpuThreadId: %#x\n", rc); return; }

    std::printf("  %u SPU threads:\n", n);
    for (unsigned int i = 0; i < n; ++i)
        std::printf("    [%u]\t0x%08x\n", i, (unsigned)ids[i]);
}

int main(void)
{
    std::printf("hello-ppu-spurs-core: Spurs bring-up smoke test\n");

    int rc = spu_printf_initialize(kSpuPrintfPriority, 0);
    if (rc) {
        std::printf("  spu_printf_initialize: %#x -- FAILED\n", rc);
        return 1;
    }
    std::printf("  spu_printf_initialize ok\n");

    cell::Spurs::Spurs2 *spurs = new cell::Spurs::Spurs2;
    if (!spurs) {
        std::printf("  new Spurs2 -- FAILED\n");
        spu_printf_finalize();
        return 1;
    }

    if (!init_spurs(spurs)) {
        std::printf("FAILURE\n");
        delete spurs;
        spu_printf_finalize();
        return 1;
    }

    print_spurs_info(spurs);

    rc = spurs->finalize();
    if (rc) {
        std::printf("  finalize: %#x -- FAILED\n", rc);
        delete spurs;
        spu_printf_finalize();
        return 1;
    }
    std::printf("  finalize ok\n");

    delete spurs;

    rc = spu_printf_finalize();
    if (rc) std::printf("  spu_printf_finalize: %#x (non-fatal)\n", rc);

    std::printf("SUCCESS\n");
    return 0;
}
