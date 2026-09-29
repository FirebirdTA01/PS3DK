/* spu-celldma, SPU side: drives each cellDma transfer family against PPU
 * memory and checks what it can see locally.  Exits 0, or the number of the
 * first failed check; the PPU side then checks what landed in its memory. */
#include <stdint.h>
#include <spu_mfcio.h>
#include <cell/dma.h>
#include <sys/spu_thread.h>
#include "../values.h"

#define TAG 5

static celldma_control control;
static uint8_t buf[SLOT_BYTES + 32] __attribute__((aligned(128)));
static uint8_t bigbuf[BIG_BYTES] __attribute__((aligned(128)));
static volatile uint32_t line[32] __attribute__((aligned(128)));

static void wait(void) { cellDmaWaitTagStatusAll(1u << TAG); }

/* Unaligned get of [base+off, base+off+size) into buf+off; nothing else in
   buf may change. */
static int unaligned_get(unsigned base, unsigned off, unsigned size)
{
    unsigned i;
    for (i = 0; i < sizeof buf; ++i)
        buf[i] = 0xee;
    cellDmaUnalignedGet(buf + off, control.src + base + off, size, TAG, 0, 0);
    wait();
    for (i = 0; i < sizeof buf; ++i) {
        int inside = i >= off && i < off + size;
        if (inside ? buf[i] != src_byte(base + i) : buf[i] != 0xee)
            return 1;
    }
    return 0;
}

int main(uint64_t control_ea, uint64_t arg2, uint64_t arg3, uint64_t arg4)
{
    static const unsigned sizes[] = { 1, 2, 3, 4, 5, 7, 8, 9, 13, 15, 16, 17, 21, 33, 100, 1000 };
    unsigned off, k, i, base;
    uint32_t st;
    (void)arg2; (void)arg3; (void)arg4;

    cellDmaGet(&control, control_ea, sizeof control, TAG, 0, 0);
    wait();

    /* 1: unaligned gets, every start offset, sizes 1..1000, two bases */
    for (base = 0; base <= 4096; base += 4096)
        for (off = 0; off < 16; ++off)
            for (k = 0; k < sizeof sizes / sizeof sizes[0]; ++k)
                if (unaligned_get(base, off, sizes[k]))
                    sys_spu_thread_exit(1);

    /* 2: unaligned puts, one case per PPU slot (the PPU checks the slots) */
    for (k = 0; k < SLOTS; ++k) {
        for (i = 0; i < sizeof buf; ++i)
            buf[i] = put_byte(i);
        cellDmaUnalignedPut(buf + put_off[k], control.dst + k * SLOT_BYTES + put_off[k],
                            put_size[k], TAG, 0, 0);
        wait();
    }

    /* 3: large get of 40KB from the source */
    cellDmaLargeGet(bigbuf, control.src, BIG_BYTES, TAG, 0, 0);
    wait();
    for (i = 0; i < BIG_BYTES; ++i)
        if (bigbuf[i] != src_byte(i))
            sys_spu_thread_exit(3);

    /* 4: large put of 40KB (the PPU checks `big`) */
    for (i = 0; i < BIG_BYTES; ++i)
        bigbuf[i] = big_byte(i);
    cellDmaLargePut(bigbuf, control.big, BIG_BYTES, TAG, 0, 0);
    wait();

    /* 5: small gets of 1, 2, 4 and 8 bytes at matching offsets */
    for (i = 0; i < sizeof buf; ++i)
        buf[i] = 0xee;
    cellDmaSmallGet(buf + 1, control.src + 1, 1, TAG, 0, 0);
    cellDmaSmallGet(buf + 2, control.src + 2, 2, TAG, 0, 0);
    cellDmaSmallGet(buf + 4, control.src + 4, 4, TAG, 0, 0);
    cellDmaSmallGet(buf + 8, control.src + 8, 8, TAG, 0, 0);
    wait();
    for (i = 0; i < 16; ++i)
        if (i == 0 ? buf[i] != 0xee : buf[i] != src_byte(i))   /* 1@1 2@2 4@4 8@8 cover 1..15 */
            sys_spu_thread_exit(5);

    /* 6: small puts of 8, 4, 2 and 1 bytes (the PPU checks `small`) */
    for (i = 0; i < 16; ++i)
        buf[i] = put_byte(i);
    cellDmaSmallPut(buf + 8, control.small + 8, 8, TAG, 0, 0);
    cellDmaSmallPut(buf + 4, control.small + 4, 4, TAG, 0, 0);
    cellDmaSmallPut(buf + 2, control.small + 2, 2, TAG, 0, 0);
    cellDmaSmallPut(buf + 1, control.small + 1, 1, TAG, 0, 0);
    wait();

    /* 7: scalar gets, big-endian compositions of the source bytes */
    if (cellDmaGetUint8(control.src + 5, TAG, 0, 0) != src_byte(5))
        sys_spu_thread_exit(7);
    if (cellDmaGetUint16(control.src + 6, TAG, 0, 0) != (uint16_t)((src_byte(6) << 8) | src_byte(7)))
        sys_spu_thread_exit(7);
    {
        uint32_t w = 0; uint64_t d = 0;
        for (i = 0; i < 4; ++i) w = (w << 8) | src_byte(12 + i);
        for (i = 0; i < 8; ++i) d = (d << 8) | src_byte(24 + i);
        if (cellDmaGetUint32(control.src + 12, TAG, 0, 0) != w)
            sys_spu_thread_exit(7);
        if (cellDmaGetUint64(control.src + 24, TAG, 0, 0) != d)
            sys_spu_thread_exit(7);
    }

    /* 8: scalar puts (the PPU checks `scalars`) */
    cellDmaPutUint64(SCALAR64, control.scalars + SCALAR64_AT, TAG, 0, 0);
    cellDmaPutUint32(SCALAR32, control.scalars + SCALAR32_AT, TAG, 0, 0);
    cellDmaPutUint16(SCALAR16, control.scalars + SCALAR16_AT, TAG, 0, 0);
    cellDmaPutUint8(SCALAR8, control.scalars + SCALAR8_AT, TAG, 0, 0);

    /* 9: lock-line increments with reservation retry, then unconditional
       and queued unconditional line writes (the PPU checks `line`) */
    for (k = 0; k < ATOMIC_ADDS; ++k) {
        do {
            cellDmaGetllar(line, control.line, 0, 0);
            (void)cellDmaWaitAtomicStatus();
            line[0] += 1;
            cellDmaPutllc(line, control.line, 0, 0);
            st = cellDmaWaitAtomicStatus();
        } while (st & MFC_PUTLLC_STATUS);
    }
    cellDmaGetllar(line, control.line, 0, 0);
    (void)cellDmaWaitAtomicStatus();
    line[1] = LINE_LLUC;
    cellDmaPutlluc(line, control.line, 0, 0);
    (void)cellDmaWaitAtomicStatus();
    line[2] = LINE_QLLUC;
    cellDmaPutqlluc(line, control.line, TAG, 0, 0);
    wait();

    /* 10: cancel-and-wait reports the completed tag */
    cellDmaGet(buf, control.src, 128, TAG, 0, 0);
    if (!(cellDmaCancelAndWaitTagStatusAll(1u << TAG) & (1u << TAG)))
        sys_spu_thread_exit(10);
    if (!(cellDmaCancelAndWaitTagStatusAny(1u << TAG) & (1u << TAG)))
        sys_spu_thread_exit(10);

    sys_spu_thread_exit(0);
    return 0;
}
