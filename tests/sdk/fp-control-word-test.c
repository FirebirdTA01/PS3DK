/* Pins every NV40TCL_FP_CONTROL word the SDK's one builder can produce
 * (sdk/include/cell/gcm/gcm_fp_control.h).  Driven by fp-control-word-test.sh,
 * which compiles this file twice: once with -DFP_CONTROL_PARENT_FORMULA, the
 * builder as every consumer had it before 2026-09-06, which must FAIL on the
 * rows marked "depth" and on no other - the proof that this table can fail -
 * and once against the real header, which must pass every row.
 *
 * Each row states what KIND of expectation it is.  "assertion": a word the
 * SDK is required to produce, with the provenance of the bits in the header.
 * "artefact": a word recorded because the code produces it today, NOT
 * because it is known right - the H0+DEPTH row collapses onto the H0-alone
 * word because 0x0e carries both meanings (t_96daf53b).
 * When that conflict is settled the artefact row is to be rewritten, and a
 * green test here is no argument against rewriting it. */
#include <stdio.h>
#include <stdint.h>

#ifdef FP_CONTROL_PARENT_FORMULA
/* The pre-2026-09-06 arithmetic, verbatim in shape: bits 16 and 18 of the
 * cgb fragmentControl, never 17; the CGprogram path likewise ignored
 * depthReplace.  Compiled ONLY for the negative run. */
static inline uint32_t ps3tc_fp_control_word(uint32_t h0, uint32_t depth,
                                             uint32_t kil, uint32_t regs)
{
    const uint32_t n = regs > 2u ? regs : 2u;
    uint32_t low = h0 ? 0x0eu : 0x40u;
    (void)depth;
    if (kil) low |= 1u << 7;
    return low | (1u << 10) | (n << 24);
}
static inline uint32_t ps3tc_fp_control_from_cgb(uint32_t fc, uint32_t regs)
{
    return ps3tc_fp_control_word((fc >> 16) & 1u, 0u, (fc >> 18) & 1u, regs);
}
#else
#include <cell/gcm/gcm_fp_control.h>
#endif

struct row {
    const char *label;
    const char *kind;      /* assertion | artefact */
    int from_cgb;          /* 0: (h0, depth, kil, regs); 1: (fragmentControl, regs) */
    uint32_t a, b, c, d;   /* h0, depth, kil, regs  -or-  fragmentControl, regs, -, - */
    uint32_t expect;
    int depth_row;         /* the parent formula must fail exactly these */
};

static const struct row rows[] = {
    /* label                              kind         cgb  h0 dp kl regs        expect      depth */
    { "nothing set, regs 2",              "assertion", 0,  0, 0, 0, 2,          0x02000440u, 0 },
    { "H0 alone (lore, untested)",        "assertion", 0,  1, 0, 0, 2,          0x0200040eu, 0 },
    { "DEPTH alone (measured 0x4e)",      "assertion", 0,  0, 1, 0, 2,          0x0200044eu, 1 },
    { "KIL alone",                        "assertion", 0,  0, 0, 1, 2,          0x020004c0u, 0 },
    { "DEPTH + KIL",                      "assertion", 0,  0, 1, 1, 2,          0x020004ceu, 1 },
    { "H0 + DEPTH collides onto H0",      "artefact",  0,  1, 1, 0, 2,          0x0200040eu, 0 },
    { "regs 1 clamps to the NV40 min 2",  "assertion", 0,  0, 0, 0, 1,          0x02000440u, 0 },
    { "regs 7, full word",                "assertion", 0,  0, 0, 0, 7,          0x07000440u, 0 },
    { "regs 48, full word",               "assertion", 0,  0, 1, 1, 48,         0x300004ceu, 1 },
    { "cgb bit 17 = depthReplace",        "assertion", 1,  0x00020000u, 3, 0, 0, 0x0300044eu, 1 },
    { "cgb bits 16|18 = H0, KIL",         "assertion", 1,  0x00050000u, 3, 0, 0, 0x0300048eu, 0 },
    { "cgb partialTexType must not leak", "assertion", 1,  0x0000ffffu, 2, 0, 0, 0x02000440u, 0 },
    { "cgb bit 17 with regs 9",           "assertion", 1,  0x00020000u, 9, 0, 0, 0x0900044eu, 1 },
};

int main(void)
{
    int failures = 0;
    const size_t n = sizeof(rows) / sizeof(rows[0]);
    for (size_t i = 0; i < n; i++) {
        const struct row *r = &rows[i];
        uint32_t got = r->from_cgb ? ps3tc_fp_control_from_cgb(r->a, r->b)
                                   : ps3tc_fp_control_word(r->a, r->b, r->c, r->d);
        int ok = got == r->expect;
        printf("%s %s [%s]%s: got 0x%08x expect 0x%08x\n",
               ok ? "PASS" : "FAIL", r->label, r->kind,
               r->depth_row ? " {depth}" : "", (unsigned)got, (unsigned)r->expect);
        if (!ok) failures++;
    }
    printf("fp-control-word: %d/%d rows pass\n", (int)(n - (size_t)failures), (int)n);
    return failures ? 1 : 0;
}
