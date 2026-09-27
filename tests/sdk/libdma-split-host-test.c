/*
 * libdma's large and unaligned splitters, compiled on the host against a
 * recording spu_mfcdma64 (see libdma-split-host-test.sh).  Every command
 * they issue must be one the MFC accepts:
 *   - 1, 2, 4 or 8 bytes, EA naturally aligned, LS with the same low four
 *     bits as EA; or
 *   - a multiple of 16 bytes up to 16KB, LS and EA both 16-byte aligned;
 * and together the commands must cover [ea, ea + size) exactly once, in
 * order, with the LS/EA distance kept, and the tag and command word passed
 * through unchanged.
 */
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>

void cellDmaLargeCmd(uintptr_t ls, uint64_t ea, uint32_t size,
                     uint32_t tag, uint32_t cmd);
void cellDmaUnalignedCmd(uintptr_t ls, uint64_t ea, uint32_t size,
                         uint32_t tag, uint32_t cmd);

#define MAX_CMDS 4096
struct cmd { uintptr_t ls; uint64_t ea; uint32_t size, tag, cmd; };
static struct cmd log_[MAX_CMDS];
static unsigned count, overflow;

void spu_mfcdma64_record(void *ls, unsigned int eah, unsigned int eal,
                         unsigned int size, unsigned int tag, unsigned int cmd)
{
    if (count == MAX_CMDS) { overflow = 1; return; }
    log_[count].ls = (uintptr_t)ls;
    log_[count].ea = ((uint64_t)eah << 32) | eal;
    log_[count].size = size;
    log_[count].tag = tag;
    log_[count].cmd = cmd;
    ++count;
}

static unsigned failures, cases;

static void judge(const char *who, uintptr_t ls, uint64_t ea, uint32_t size)
{
    uint64_t next_ea = ea;
    uintptr_t next_ls = ls;
    const char *why = 0;
    unsigned i;
    ++cases;
    if (overflow) why = "too many commands";
    for (i = 0; !why && i < count; ++i) {
        const struct cmd *c = &log_[i];
        uint32_t s = c->size;
        if (c->ea != next_ea || c->ls != next_ls) why = "gap, overlap or reorder";
        else if (c->tag != 7 || c->cmd != 0x01020040u) why = "tag or command word changed";
        else if (s == 1 || s == 2 || s == 4 || s == 8) {
            if (c->ea % s != 0) why = "small piece not naturally aligned";
            else if ((c->ea & 15) != (c->ls & 15)) why = "small piece LS/EA offsets differ";
        } else if (s == 0 || s % 16 != 0 || s > 16384) why = "illegal transfer size";
        else if ((c->ea & 15) != 0 || (c->ls & 15) != 0) why = "quadword piece not 16-byte aligned";
        next_ea = c->ea + s;
        next_ls = c->ls + s;
    }
    if (!why && next_ea != ea + size) why = "range not fully covered";
    if (why) {
        ++failures;
        if (failures <= 20)
            printf("FAIL %s ls=0x%" PRIxPTR " ea=0x%" PRIx64 " size=%" PRIu32 ": %s (cmd %u of %u, size %" PRIu32 ")\n",
                   who, ls, ea, size, why, i, count, i ? log_[i - 1].size : 0);
    }
}

int main(void)
{
    static const uint32_t big[] = { 16368, 16384, 16400, 32768, 40000, 65536 + 48 };
    uint64_t ea_base = 0x0000000312345000ull;   /* high word nonzero */
    uintptr_t ls_base = 0x10000;
    unsigned off, k;
    uint32_t size;

    for (off = 0; off < 16; ++off) {
        for (size = 0; size <= 300; ++size) {
            count = overflow = 0;
            cellDmaUnalignedCmd(ls_base + off, ea_base + off, size, 7, 0x01020040u);
            judge("unaligned", ls_base + off, ea_base + off, size);
        }
        for (k = 0; k < sizeof big / sizeof big[0]; ++k) {
            count = overflow = 0;
            cellDmaUnalignedCmd(ls_base + off, ea_base + off, big[k], 7, 0x01020040u);
            judge("unaligned", ls_base + off, ea_base + off, big[k]);
        }
    }
    for (size = 0; size <= 3 * 16384 + 64; size += 16) {
        count = overflow = 0;
        cellDmaLargeCmd(ls_base, ea_base, size, 7, 0x01020040u);
        judge("large", ls_base, ea_base, size);
    }
    printf("libdma-split: %u cases, %u failures\n", cases, failures);
    return failures ? 1 : 0;
}
