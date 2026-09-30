/* spurs-ppu-sym (row spurs-ppu-sym): a SPURS task names two PPU globals with
 * CELL_SPURS_PPU_SYM.  The image is embedded with spu-elf-to-ppu-obj, so the
 * PPU link fills in their addresses.  Checks the task saw the addresses of
 * the globals, read their contents and wrote to them. */
#include "harness.h"
#include "../ppu_sym.h"
#include SUITE_SPU_HEADER

extern "C" {
alignas(16) volatile ps_target ps_target_a = { PS_MAGIC_A, 0, { 0, 0 } };
alignas(16) volatile ps_target ps_target_b = { PS_MAGIC_B, 0, { 0, 0 } };
}

alignas(16) static volatile ps_box s_box;

static uint32_t ea_of(const volatile void *p)
{
    return static_cast<uint32_t>(reinterpret_cast<uintptr_t>(p));
}

static int row_main()
{
    suite::watchdog(20);
    auto *spurs = new cell::Spurs::Spurs2;
    int rc = suite::spurs_up(spurs, "SuitePSym");
    if (rc) return suite::invalid("spurs", rc);
    cell::Spurs::Taskset *ts = suite::taskset_up(spurs, &rc);
    if (!ts) return suite::invalid("taskset", rc);

    suite::activity("SPU task reads two PPU globals by name");
    if ((rc = suite::launch(ts, SUITE_SPU_BIN, reinterpret_cast<uintptr_t>(&s_box), 0, 0)))
        return suite::invalid("launch", rc);
    int result = 0;
    if (!suite::wait_for([] { return s_box.state == 2; })) result = suite::fail("task done", s_box.state, 2);
    else if (s_box.eaA != ea_of(&ps_target_a)) result = suite::fail("address of ps_target_a", s_box.eaA, ea_of(&ps_target_a));
    else if (s_box.eaB != ea_of(&ps_target_b)) result = suite::fail("address of ps_target_b", s_box.eaB, ea_of(&ps_target_b));
    else if (s_box.magicA != PS_MAGIC_A) result = suite::fail("ps_target_a read", s_box.magicA, PS_MAGIC_A);
    else if (s_box.magicB != PS_MAGIC_B) result = suite::fail("ps_target_b read", s_box.magicB, PS_MAGIC_B);
    else if (ps_target_a.seen != 1 || ps_target_b.seen != 1) result = suite::fail("globals written", ps_target_a.seen | ps_target_b.seen << 8, 0x101);
    suite::taskset_down(ts);
    spurs->finalize();
    return result ? result : suite::ok();
}

SUITE_ENTRY_POINT(row_main)
