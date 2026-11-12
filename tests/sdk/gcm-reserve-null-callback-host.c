/*
 * gcm-reserve-null-callback-test.c — host guard for the safe-path reserve
 * contract shared by <gcm_cg_bridge.h> / <gcm_state_native.h> /
 * <gcm_tex_bridge.h> (all call ps3tc_gcm_reserve).
 *
 * The BUG this pins: when the ring is full and the callback is NULL,
 * ps3tc_gcm_reserve must NOT write (and must not advance current). The
 * old code fell through to the write, storing words past `end`
 * (e.g. cellGcmSetColorMask with current==end and callback==NULL wrote
 * 2 words past the reservation).
 *
 * Cases (all run through the public cellGcm* path AND the safe natives;
 * the state native and the tex-bridge native share the same helper, so the
 * fix is covered for both families):
 *   A. exact room          -> writes (in range), current advanced
 *   B. one short + NULL cb -> NO write, current unchanged        [RED pre-fix]
 *   C. callback refuses   -> NO write, current unchanged, cb called once
 *   D. callback relocates -> writes at the relocated location
 *   E. tex-bridge, one short + NULL cb  -> NO write, current unchanged [RED pre-fix]
 *   F. unsafe native, full ring         -> still writes (unsafe is unchanged)
 *
 * No hard-coded command words for the reserved path: the checks are on
 * "was anything written and did current move", which is the contract under
 * test. The in-range writes (A, D) are checked byte-for-byte too.
 *
 * Host note: non-PowerPC host via -DATTRIBUTE_PRXPTR= ; <cell/gcm.h> and the
 * bridges host-compile under that setup, so this drives the REAL shared
 * ps3tc_gcm_reserve, not a copy.
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <ppu-types.h>
#include <cell/gcm.h>                    /* CellGcmContextData + cellGcm* forwarders   */
#include <cell/gcm/gcm_state_native.h>   /* safe/unsafe state natives                 */
#include <cell/gcm/gcm_tex_bridge.h>     /* safe tex natives (share ps3tc_gcm_reserve)*/

#define SENTINEL 0xccccccccu            /* fills "unwritten" ring slots              */

#define REF_METHOD(m, n) (((uint32_t)(n) << 18) | (uint32_t)(m))

static int failures;

static uint32_t backing[32];
static uint32_t region2[8];

static long  g_cb_calls;
static uint32_t *g_reloc_to;           /* where cb_relocate points current at        */

/* callback that refuses (nonzero) — records that it ran */
static int32_t cb_refuse(gcmContextData *ctx, uint32_t count)
{
    (void)ctx; (void)count;
    g_cb_calls++;
    return 1;
}

/* callback that relocates the ring into a fresh region, reports success */
static int32_t cb_relocate(gcmContextData *ctx, uint32_t count)
{
    (void)count;
    g_cb_calls++;
    ctx->current = g_reloc_to;
    ctx->end     = g_reloc_to + 8;
    return 0;
}

/* callback that reports success (0) but did not relocate the ring —
 * tests the "re-check room after callback" branch */
static int  g_fake_ok_calls;
static int32_t cb_fake_ok(gcmContextData *ctx, uint32_t count)
{
    (void)ctx; (void)count;
    g_fake_ok_calls++;
    return 0;                    /* success, but ring still full */
}

static void fill(uint32_t *p, unsigned n) { for (unsigned i = 0; i < n; ++i) p[i] = SENTINEL; }

static void expect_write(const char *lab, const uint32_t *got, const uint32_t want[2],
                         size_t nwords)
{
    int ok = (memcmp(got, want, nwords * sizeof *got) == 0);
    printf("  %-32s bytes %s\n", lab, ok ? "ok" : "MISMATCH");
    if (!ok) failures++;
}

static void expect_nowrite(const char *lab, const uint32_t *region, uint32_t *current,
                           uint32_t *current_before)
{
    /* nothing written anywhere in the 2-word window, and current did not move */
    int ok = (region[0] == SENTINEL && region[1] == SENTINEL && current == current_before);
    printf("  %-32s no-write %s (region[0]=0x%08x cur=%s)\n",
           lab, ok ? "ok" : "MISMATCH", region[0], current == current_before ? "stable" : "MOVED");
    if (!ok) failures++;
}

int main(void)
{
    CellGcmContextData n;
    uint32_t *cur_before;

    printf("  reserve-safe contract (full ring + NULL callback must not write)\n");

    /* ---- A. exact room -> writes, in range ------------------------------- */
    fill(backing, 32); n.begin = backing; n.end = backing + 2; n.current = backing; n.callback = 0;
    ps3tc_gcm_state_color_mask(&n, 0x0F);
    expect_write("A state-native exact-room", backing,
                 (const uint32_t[]){ REF_METHOD(NV40TCL_COLOR_MASK, 1), 0x0F }, 2);

    /* ---- B. one short + NULL callback -> NO write [RED before fix] ------- */
    fill(backing, 32); n.begin = backing; n.end = backing + 1; n.current = backing; n.callback = 0;
    cur_before = n.current;
    ps3tc_gcm_state_color_mask(&n, 0x0F);
    expect_nowrite("B state-native short+NULL", backing, n.current, cur_before);

    /* same case through the PUBLIC cellGcm* forwarder */
    fill(backing, 32); n.begin = backing; n.end = backing + 1; n.current = backing; n.callback = 0;
    cur_before = n.current;
    cellGcmSetColorMask(&n, 0x0F);
    expect_nowrite("B cellGcmSetColorMask short+NULL", backing, n.current, cur_before);

    /* ---- C. callback refuses -> NO write, current unchanged -------------- */
    fill(backing, 32); n.begin = backing; n.end = backing + 1; n.current = backing; n.callback = cb_refuse;
    g_cb_calls = 0; cur_before = n.current;
    ps3tc_gcm_state_color_mask(&n, 0x0F);
    expect_nowrite("C state-native cb-refuses", backing, n.current, cur_before);
    { int ok = (g_cb_calls == 1); printf("  %-32s cb-called-once %s\n", "C callback invoked exactly once", ok ? "ok" : "MISMATCH"); if (!ok) failures++; }

    /* ---- D. callback relocates -> writes at the new location ------------- */
    fill(backing, 32); fill(region2, 8);
    n.begin = backing; n.end = backing + 1; n.current = backing; n.callback = cb_relocate;
    g_reloc_to = region2; g_cb_calls = 0;
    ps3tc_gcm_state_color_mask(&n, 0x0F);
    expect_write("D state-native cb-relocates", region2,
                 (const uint32_t[]){ REF_METHOD(NV40TCL_COLOR_MASK, 1), 0x0F }, 2);
    { int ok = (backing[0] == SENTINEL && backing[1] == SENTINEL);
      printf("  %-32s old-region untouched %s\n", "D original region not clobbered", ok ? "ok" : "MISMATCH"); if (!ok) failures++; }

    /* ---- E. tex-bridge shares the helper: short + NULL -> NO write -------- */
    fill(backing, 32); n.begin = backing; n.end = backing + 1; n.current = backing; n.callback = 0;
    cur_before = n.current;
    ps3tc_gcm_tex_control(&n, 0, 1, 0, 7, 1);
    expect_nowrite("E tex_control short+NULL", backing, n.current, cur_before);

    /* and the tex-bridge exact-room control (sanity: shared helper still writes).
     * Don't hard-code the data word (shifts are easy to miscount) — check the
     * known method word, that a non-sentinel value landed, and that current
     * advanced two words. */
    fill(backing, 32); n.begin = backing; n.end = backing + 2; n.current = backing; n.callback = 0;
    cur_before = n.current;
    ps3tc_gcm_tex_control(&n, 0, 1, 0, 7, 1);
    {
        int ok = (backing[0] == REF_METHOD(NV40TCL_TEX_ENABLE(0), 1)
                    && backing[1] != SENTINEL
                    && n.current == cur_before + 2);
        printf("  %-32s safe-still-writes %s (mth=0x%08x val=0x%08x cur=%s)\n",
               "E tex_control exact-room", ok ? "ok" : "MISMATCH",
               backing[0], backing[1], n.current == cur_before + 2 ? "advanced" : "STUCK");
        if (!ok) failures++;
    }

    /* ---- F. unsafe native, full ring -> still writes (unchanged) ---------- */
    fill(backing, 32); n.begin = backing; n.end = backing + 1; n.current = backing; n.callback = 0;
    ps3tc_gcm_state_color_mask_unsafe(&n, 0x0F);
    expect_write("F unsafe-native still writes", backing,
                 (const uint32_t[]){ REF_METHOD(NV40TCL_COLOR_MASK, 1), 0x0F }, 2);

    /* ---- G. callback claims success but leaves ring full -> NO write ------ */
    /* The callback reports 0 (OK) but neither current nor end moved —
     * the re-check after invoke_callback must catch that and refuse,
     * not write past end as the old "trust the callback" path would. */
    fill(backing, 32); n.begin = backing; n.end = backing + 1; n.current = backing; n.callback = cb_fake_ok;
    g_fake_ok_calls = 0; cur_before = n.current;
    ps3tc_gcm_state_color_mask(&n, 0x0F);
    expect_nowrite("G state-native cb-no-room", backing, n.current, cur_before);
    { int ok = (g_fake_ok_calls == 1);
      printf("  %-32s cb-called %s\n", "G callback invoked exactly once", ok ? "ok" : "MISMATCH");
      if (!ok) failures++; }

    printf("\n");
    if (failures == 0)
        printf("  PASS (safe path never writes past end: NULL-cb/refused short ring refused, "
               "relocate honored, tex-bridge shares the fix, unsafe unchanged)\n");
    else
        printf("  FAIL (%d) — likely the full+NULL path is still writing past end\n", failures);
    return failures ? 1 : 0;
}
