/* spurs-suite C++ job built with the -mspurs-job-initialize driver mode
 * alone (row spurs-job-cpp-driver): g++ links it, so libstdc++ is searched
 * before the SPURS runtime archives.  It uses a function-local static (the
 * C++ guards), a global with a constructor (_init) and a virtual call (a
 * relocated vtable), then reports in the job-chain slot layout:
 *   magic, DMA tag, eaBinary, 0x10b5 if every check passed (else the
 *   failing check's number | 0xbad0000).
 * workArea.userData[0] = result slot EA, userData[1] = magic. */
#include <stdint.h>
#include <spu_intrinsics.h>
#include <spu_mfcio.h>
#include <cell/spurs/job_chain.h>
#include <cell/spurs/job_context.h>

/* heap 256 B, stack 4 KB: the job binary wrapper requires the LS parameters */
__asm__(
    ".pushsection .data\n"
    ".global _cell_spu_ls_param\n"
    ".align 4\n"
    ".type _cell_spu_ls_param, @object\n"
    ".size _cell_spu_ls_param, 16\n"
    "_cell_spu_ls_param:\n"
    ".long 0x100, 0x1000, 0, 0\n"
    ".popsection\n"
    ".pushsection .cell_spu_ls_param\n"
    ".long 0x100, 0x1000\n"
    ".popsection\n");

namespace {

unsigned s_builds;

struct Counted {
    unsigned value;
    Counted() : value(0x5a) { ++s_builds; }
};

/* built on first call only */
unsigned lazy()
{
    static Counted c;
    return c.value;
}

struct Shape {
    virtual unsigned sides() const = 0;
    virtual ~Shape() {}
};
struct Triangle : Shape { unsigned sides() const override { return 3; } };
struct Hexagon : Shape { unsigned sides() const override { return 6; } };

struct Global {
    unsigned tag;
    Global() : tag(0x61) {}
};
Global g_global;

unsigned total(const Shape &a, const Shape &b) { return a.sides() + b.sides(); }

}   /* namespace */

extern "C" void cellSpursJobMain2(CellSpursJobContext2 *ctx, CellSpursJob256 *job)
{
    unsigned check = 0x10b5;
    if (lazy() != 0x5a || lazy() != 0x5a) check = 0xbad0001;
    else if (s_builds != 1) check = 0xbad0002;
    else if (g_global.tag != 0x61) check = 0xbad0003;
    else {
        Triangle t;
        Hexagon h;
        if (total(t, h) != 9) check = 0xbad0004;
    }

    __attribute__((aligned(16))) uint32_t buf[4];
    buf[0] = (uint32_t)job->workArea.userData[1];
    buf[1] = ctx->dmaTag;
    buf[2] = (uint32_t)job->header.eaBinary;
    buf[3] = check;
    mfc_put(buf, job->workArea.userData[0], sizeof buf, ctx->dmaTag, 0, 0);
    mfc_write_tag_mask(1u << ctx->dmaTag);
    mfc_read_tag_status_all();
}
