/*
 * hello-ppu-spurs-event-flag - PPU-visible event-flag bring-up.
 *
 * Stands up a 4-SPU Spurs2 instance, creates an event flag with
 * ANY2ANY direction (so PPU can drive set / wait / clear), exercises
 * the PPU-facing ops, then tears down.  No SPU code involved; this
 * is a smoke test that our cell/spurs/event_flag.h header surface
 * and libspurs SPRX match up for the in-workload event-flag path.
 */

#include <cstdio>
#include <cstring>

#include <sys/process.h>
#include <sys/spu_thread_group.h>
#include <cell/spurs.h>
#include <spu_printf.h>

SYS_PROCESS_PARAM(1001, 0x10000);

static const int kSpuPrintfPriority = 999;

int main(void)
{
    std::printf("hello-ppu-spurs-event-flag\n");

    int rc = spu_printf_initialize(kSpuPrintfPriority, 0);
    if (rc) { std::printf("  spu_printf_initialize: %#x\n", rc); return 1; }

    /* Spurs2 creation - shared with the spurs-core sample's approach. */
    cell::Spurs::SpursAttribute attr;
    rc = cell::Spurs::SpursAttribute::initialize(&attr, 4, 100, 2, false);
    if (rc) { std::printf("  SpursAttribute::initialize: %#x\n", rc); return 1; }
    attr.setNamePrefix("EvtFlag", 7);
    attr.setSpuThreadGroupType(SYS_SPU_THREAD_GROUP_TYPE_EXCLUSIVE_NON_CONTEXT);
    attr.enableSpuPrintfIfAvailable();

    cell::Spurs::Spurs2 *spurs = new cell::Spurs::Spurs2;
    rc = cell::Spurs::Spurs2::initialize(spurs, &attr);
    if (rc) { std::printf("  Spurs2::initialize: %#x\n", rc); return 1; }
    std::printf("  Spurs2 up\n");

    /* Initialize event flag in-workload (IWL): no taskset required. */
    CellSpursEventFlag ef;
    std::memset(&ef, 0, sizeof(ef));
    rc = cellSpursEventFlagInitializeIWL(spurs, &ef,
                                         CELL_SPURS_EVENT_FLAG_CLEAR_MANUAL,
                                         CELL_SPURS_EVENT_FLAG_ANY2ANY);
    if (rc) { std::printf("  EventFlagInitializeIWL: %#x\n", rc); goto teardown; }
    std::printf("  EventFlag init ok (ANY2ANY, MANUAL clear)\n");

    /* Read-back the config through the Get accessors.  Not strictly
     * informative by themselves, but they route through the SPRX and
     * prove the layout survives DMA round-trips. */
    {
        CellSpursEventFlagDirection dir;
        CellSpursEventFlagClearMode mode;
        if (cellSpursEventFlagGetDirection(&ef, &dir) == 0 &&
            cellSpursEventFlagGetClearMode(&ef, &mode) == 0)
        {
            std::printf("  readback dir=%d clear=%d\n", (int)dir, (int)mode);
        }
    }

    /* Set 0x0003, try_wait with OR=any, then clear. */
    if ((rc = cellSpursEventFlagSet(&ef, 0x0003)) != 0)
        std::printf("  Set: %#x\n", rc);
    else
        std::printf("  Set 0x0003 ok\n");

    {
        uint16_t bits = 0x0001;
        rc = cellSpursEventFlagTryWait(&ef, &bits, CELL_SPURS_EVENT_FLAG_OR);
        std::printf("  TryWait OR bits=0x%04x rc=%#x\n", (unsigned)bits, rc);
    }

    if ((rc = cellSpursEventFlagClear(&ef, 0xffff)) != 0)
        std::printf("  Clear: %#x\n", rc);

teardown:
    rc = spurs->finalize();
    std::printf("  Spurs finalize: %#x\n", rc);
    delete spurs;

    spu_printf_finalize();

    std::printf("hello-ppu-spurs-event-flag: done\n");
    return 0;
}
