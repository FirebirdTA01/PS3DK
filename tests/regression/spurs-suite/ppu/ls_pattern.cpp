/* spurs-suite LS patterns (row spurs-ls-pattern).
 *   context  a task generates patterns, sizes a save area, reads its own
 *            pattern, sets a smaller one (which its taskset record must
 *            follow and a yield must keep) and restores it
 *   elf      a task reads the loadable-segment and read-only patterns of
 *            its own ELF, with and without a header buffer, plus the
 *            argument errors; the PPU reads the same ELF and checks them */
#include "harness.h"
#include "../ls_pattern.h"
#include SUITE_SPU_HEADER

alignas(128) static volatile result_slot g_result[LP_SLOTS];
alignas(128) static lp_report s_report;

static uint32_t be32(const uint8_t *p) { return (uint32_t(p[0]) << 24) | (p[1] << 16) | (p[2] << 8) | p[3]; }
static uint32_t be16(const uint8_t *p) { return (uint32_t(p[0]) << 8) | p[1]; }

/* set blocks [first, end) of a 128-bit pattern, block 0 = MSB of word 0 */
static void set_blocks(uint32_t w[4], uint32_t first, uint32_t end)
{
    for (uint32_t b = first; b < end && b < 128; ++b)
        w[b / 32] |= 0x80000000u >> (b % 32);
}

/* the patterns of an SPU ELF: every block a PT_LOAD segment's memory
 * touches, and the blocks wholly inside a read-only segment's file image */
static void expected(const uint8_t *elf, uint32_t loadable[4], uint32_t readonly[4])
{
    const uint32_t phoff = be32(elf + 0x1c), step = be16(elf + 0x2a), n = be16(elf + 0x2c);
    for (uint32_t i = 0; i < n; ++i) {
        const uint8_t *ph = elf + phoff + i * step;
        const uint32_t vaddr = be32(ph + 8), filesz = be32(ph + 16), memsz = be32(ph + 20);
        if (be32(ph) != 1)
            continue;
        set_blocks(loadable, vaddr / 2048, (vaddr + memsz + 2047) / 2048);
        if (!(be32(ph + 24) & 2))
            set_blocks(readonly, (vaddr + 2047) / 2048, (vaddr + filesz) / 2048);
    }
}

static int run(cell::Spurs::Taskset *ts, unsigned kind, const char *what)
{
    int rc = suite::launch(ts, SUITE_SPU_BIN, reinterpret_cast<uintptr_t>(g_result),
                           static_cast<uint32_t>(reinterpret_cast<uintptr_t>(&s_report)), kind);
    if (rc)
        return suite::invalid(what, rc);
    suite::activity("waiting for task: %s", what);
    if (!suite::wait_for([&] { return suite::slot_done(g_result[kind], kind); }))
        return suite::fail(what, g_result[kind].magic, RESULT_MAGIC | kind);
    if (g_result[kind].status) {
        std::printf("%s: step %u got %#x\n", what, g_result[kind].status, g_result[kind].extra);
        return suite::fail(what, g_result[kind].status, 0);
    }
    return 0;
}

static int row_main()
{
    suite::watch_slots(g_result, LP_SLOTS);
    suite::watchdog(30);
    std::memset((void *)g_result, 0, sizeof g_result);
    std::memset(&s_report, 0, sizeof s_report);
    auto *spurs = new cell::Spurs::Spurs2;
    int rc = suite::spurs_up(spurs, "SuiteLsp");
    if (rc) return suite::invalid("spurs", rc);
    cell::Spurs::Taskset *ts = suite::taskset_up(spurs, &rc);
    if (!ts) return suite::invalid("taskset", rc);
    int result;

    if ((result = run(ts, LP_CONTEXT, "context pattern"))) return result;
    if ((result = run(ts, LP_ELF, "ELF patterns"))) return result;

    uint32_t loadable[4] = {}, readonly[4] = {};
    expected(reinterpret_cast<const uint8_t *>(SUITE_SPU_BIN), loadable, readonly);
    for (unsigned i = 0; i < 4; ++i) {
        if (s_report.loadable[i] != loadable[i])
            return suite::fail("loadable pattern word", s_report.loadable[i], loadable[i]);
        if (s_report.readonly[i] != readonly[i])
            return suite::fail("read-only pattern word", s_report.readonly[i], readonly[i]);
        if (s_report.loadable_buf[i] != loadable[i])
            return suite::fail("loadable pattern (caller buffer)", s_report.loadable_buf[i], loadable[i]);
        if (s_report.readonly_buf[i] != readonly[i])
            return suite::fail("read-only pattern (caller buffer)", s_report.readonly_buf[i], readonly[i]);
        if (readonly[i] & ~loadable[i])
            return suite::fail("read-only outside loadable", readonly[i], loadable[i]);
    }
    /* the task image starts at the task top: block 6 loads, blocks 0..5 never */
    if ((loadable[0] & 0xfe000000u) != 0x02000000u)
        return suite::fail("loadable blocks 0..6", loadable[0] & 0xfe000000u, 0x02000000u);

    suite::taskset_down(ts);
    spurs->finalize();
    return suite::ok();
}

SUITE_ENTRY_POINT(row_main)
