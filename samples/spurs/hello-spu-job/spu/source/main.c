/*
 * hello-spu-job - SPU side.
 *
 * A real cellSpursJobMain2 binary.  The dispatcher branches into our
 * libspurs_job _start trampoline, which clears .bss via
 * cellSpursJobInitialize and tail-calls into the routine below.
 *
 * The PPU passes us:
 *   job->workArea.userData[0] = EA of a 16-byte sentinel buffer
 *   job->workArea.userData[1] = a magic value the PPU expects back
 *
 * We DMA-put a 16-byte block back to that EA whose first u32 lane is
 * the magic, second lane is the jobContext->dmaTag (so the PPU has
 * proof we read the per-job context), and the remaining lanes echo the
 * job's eaBinary low/high halves.
 */

#include <stdint.h>
#include <spu_intrinsics.h>
#include <spu_mfcio.h>

#include <cell/spurs/job_chain.h>
#include <cell/spurs/job_context.h>

/* CELL_SPU_LS_PARAM(heap_size, stack_size) - inline-asm macro from
 * <stdlib.h> in the proprietary SDK; emits the `_cell_spu_ls_param`
 * .data symbol (16 bytes: heap, stack, 0, 0) AND a `.cell_spu_ls_param`
 * section (8 bytes: heap, stack) that the reference jobbin2-wrapper
 * tool checks before accepting the ELF.  We re-implement it here so
 * we don't have to patch our newlib's stdlib.h. */
__asm__(
    ".pushsection .data\n"
    ".global _cell_spu_ls_param\n"
    ".align 4\n"
    ".type _cell_spu_ls_param, @object\n"
    ".size _cell_spu_ls_param, 16\n"
    "_cell_spu_ls_param:\n"
    ".long 0x100, 0x1000, 0, 0\n"   /* heap=256B, stack=4096B */
    ".popsection\n"
    ".pushsection .cell_spu_ls_param\n"
    ".long 0x100, 0x1000\n"
    ".popsection\n"
);

/* .SpuGUID - 16-byte GUID-like blob the jobbin2-wrapper tool expects
 * to find as a named section in the ELF.  The reference dispatcher
 * places this immediately before the entry point (vaddr 0xa00 with
 * entry at 0xa10).  For a user job binary the placement isn't
 * load-critical; we emit it as an allocatable readonly section that
 * the linker script anchors before .text but after .before_text.  The
 * 16 bytes are arbitrary placeholder content; the tool reads but
 * doesn't validate the value (the *presence* of the section is what
 * gates "valid Job ELF" acceptance). */
__asm__(
    ".pushsection .SpuGUID, \"ax\", @progbits\n"
    ".balign 1\n"
    ".long 0x436e8402, 0x42569682, 0x43d9e302, 0x43f75f82\n"
    ".popsection\n"
);

/* .note.spu_name - PT_NOTE segment with type=1, name="SPUNAME",
 * desc=basename of the SPU image.  Standard ELF NOTE structure
 * (namesz, descsz, type, name, desc — all word-aligned). */
__asm__(
    ".pushsection .note.spu_name, \"\", @note\n"
    ".balign 4\n"
    ".long 8\n"                          /* namesz = 8 ("SPUNAME\\0") */
    ".long 32\n"                         /* descsz = 32 (padded desc) */
    ".long 1\n"                          /* type = 1 */
    ".asciz \"SPUNAME\"\n"               /* name (8 bytes incl. NUL) */
    ".asciz \"hello_spu_job.elf\"\n"     /* desc (18 bytes; pad to 32) */
    ".zero 14\n"                          /* padding to 32-byte desc */
    ".popsection\n"
);

void cellSpursJobMain2(CellSpursJobContext2 *ctx, CellSpursJob256 *job)
{
    uint64_t out_ea = job->workArea.userData[0];
    uint64_t magic  = job->workArea.userData[1];

    __attribute__((aligned(16))) uint32_t buf[4];
    buf[0] = (uint32_t)magic;
    buf[1] = ctx->dmaTag;
    buf[2] = (uint32_t)(job->header.eaBinary);
    buf[3] = (uint32_t)(job->header.eaBinary >> 32);

    mfc_put(buf, out_ea, sizeof(buf), ctx->dmaTag, 0, 0);
    mfc_write_tag_mask(1u << ctx->dmaTag);
    mfc_read_tag_status_all();
}
