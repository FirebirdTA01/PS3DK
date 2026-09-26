/* sheap-p0-layout (SPU side): read the headers and first tree lines of a
 * plain and a keyed heap that the PPU initialised through the firmware,
 * and compare every field the SPU library reads with the layout it
 * assumes.
 *
 * args: plain heap EA, keyed heap EA, result block EA.
 * result: values[0] = mismatch count, then up to 7 (check, value) pairs.
 */
#include "common.h"

static uint8_t line[128] __attribute__((aligned(128)));
static uint32_t tree[32] __attribute__((aligned(128)));
static unsigned mismatches;

static uint64_t field64(unsigned off)
{
    uint64_t v;

    __builtin_memcpy(&v, line + off, 8);
    return v;
}

static uint32_t field32(unsigned off)
{
    uint32_t v;

    __builtin_memcpy(&v, line + off, 4);
    return v;
}

static void expect(unsigned id, uint64_t got, uint64_t want)
{
    if (got == want)
        return;
    if (mismatches < 7) {
        row_result[1 + 2 * mismatches] = id;
        row_result[2 + 2 * mismatches] = got;
    }
    ++mismatches;
    CHECK(id, 0);
}

/* First tree line: two bits per node, 16 nodes per big-endian word. */
static void expect_tree(unsigned first_id, uint64_t ea_tree, const uint32_t want[32])
{
    unsigned i;

    row_get(tree, ea_tree, 128);
    for (i = 0; i < 32; ++i)
        expect(first_id + i, tree[i], want[i]);
}

int main(uint64_t plain, uint64_t keyed, uint64_t ea_result, uint64_t unused)
{
    /* Node states after initialisation (fence marks only), from the
     * layout rules: plain 10240 -> nodes 1..205, keyed 10240 -> 1..125. */
    static const uint32_t plain_tree[32] = {
        0x22090090, 0x00002000, 0, 0x02000000, 0, 0, 0x00010000, 0,
    };
    static const uint32_t keyed_tree[32] = {
        0x22020002, 0x00000002, 0, 0x00000001,
    };
    unsigned i;

    (void)unused;

    row_get(line, plain, 128);
    expect(1, field32(0), 0);                   /* lock */
    expect(2, field64(8), plain);               /* ea_self */
    expect(3, field64(16), plain + 128);        /* ea_tree */
    expect(4, field64(24), 128);                /* s_tree */
    expect(5, field32(32), 205);                /* n_nodes */
    expect(6, field64(40), plain + 256);        /* ea_heap */
    expect(7, field64(48), 16384);              /* s_root */
    expect(8, field64(56), 9984);               /* s_buffer */
    expect(9, field32(64), 8);                  /* spu_tag1 */
    expect(10, field32(68), 8);                 /* spu_tag2 */
    expect_tree(11, plain + 128, plain_tree);   /* checks 11..42 */

    row_get(line, keyed, 128);
    expect(51, field32(0), 0);
    expect(52, field64(8), keyed);
    expect(53, field64(16), keyed + 2176);
    expect(54, field64(24), 128);
    expect(55, field32(32), 125);
    expect(56, field64(40), keyed + 2304);
    expect(57, field64(48), 8192);
    expect(58, field64(56), 7936);
    expect(59, field32(64), 3);
    expect(60, field32(68), 3);
    expect(61, field64(72), keyed + 128);       /* ea_keytable */
    expect(62, field64(80), 2048);              /* s_keytable */
    expect_tree(63, keyed + 2176, keyed_tree);  /* checks 63..94 */

    /* The key table is all zero: 256 empty entries. */
    for (i = 0; i < 16; ++i) {
        unsigned j, nonzero = 0;

        row_get(line, keyed + 128 + 128 * i, 128);
        for (j = 0; j < 128; ++j)
            nonzero |= line[j];
        expect(95, nonzero, 0);
    }

    row_result[0] = mismatches;
    row_finish(ea_result);
    return 0;
}
