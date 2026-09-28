/* spurs-suite SPU job-queue runtime: C++ in a job linked with
 * -mspurs-job-initialize and --emit-fixups.  Checks virtual calls through
 * a vtable the runtime relocated, a table of string pointers, a global
 * object built by a constructor before the job and a function-local
 * static built on first use; a global's destructor reports to the PPU
 * after the job returns (Q_CPP in jq_spu.h). */
#include <stdint.h>
#include <spu_mfcio.h>
#include "../jq_spu.h"

namespace {

struct Shape {
    virtual unsigned sides() const { return 0; }
    virtual ~Shape() {}
};
struct Triangle : Shape { unsigned sides() const override { return 3; } };
struct Square : Shape { unsigned sides() const override { return 4; } };

unsigned g_built;                       /* .bss: the runtime clears it for every job */
uint64_t g_reportEa;

struct Global {
    unsigned value;
    Global() : value(0x600d) { g_built += 1; }
    ~Global()
    {
        static uint32_t done[4] __attribute__((aligned(16)));
        if (!g_reportEa)
            return;
        done[0] = Q_CPP_DTOR;
        done[1] = g_built;
        mfc_put(done, g_reportEa, sizeof done, 4, 0, 0);
        mfc_write_tag_mask(1u << 4);
        mfc_read_tag_status_all();
    }
};
Global g_global;

const char *const g_names[] = { "zero", "one", "two" };

struct Lazy {
    unsigned value;
    Lazy() : value(7) { g_built += 100; }
};

unsigned lazy()
{
    static Lazy l;
    return l.value;
}

const Shape &pick(unsigned i)
{
    static const Triangle t;
    static const Square s;
    return i ? static_cast<const Shape &>(s) : static_cast<const Shape &>(t);
}

} // namespace

/* returns the first failed step, 0 when all pass */
extern "C" unsigned jq_cpp_run(uint64_t reportEa, uint32_t *got)
{
    g_reportEa = reportEa;
    *got = g_global.value;
    if (g_global.value != 0x600d)
        return 101;                     /* constructor did not run */
    *got = g_built;
    if (g_built != 1)
        return 102;                     /* constructed once, not zero or twice */
    *got = pick(0).sides() * 10 + pick(1).sides();
    if (*got != 34)
        return 103;                     /* virtual call through the relocated vtable */
    *got = (uint32_t)(g_names[2][1] << 8 | g_names[1][0]);
    if (*got != ('w' << 8 | 'o'))
        return 104;                     /* pointer table relocated */
    *got = lazy() + lazy();
    if (*got != 14 || g_built != 101)
        return 105;                     /* function-local static built once */
    return 0;
}
