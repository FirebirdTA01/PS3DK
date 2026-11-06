// PROBE-G (r2 10.3, B2): the C++ guard is neither copyable nor movable.  build-and-run.sh compiles this three ways:
// -DPROBE_G_CONTROL must compile; -DPROBE_G_COPY and -DPROBE_G_MOVE must each FAIL to compile.
#define PS3DK_PROF 1
#include <ps3dk/prof.h>

#include <utility>

static ps3dk_prof_desc d = {"g", "", "probe_g.cpp", 1u, 1u, 0u, 0u};

void probe(void);
void probe(void)
{
    ps3dk_prof::zone_guard g1(&d, PS3DK_PROF_TOKEN_NONE);
#if defined(PROBE_G_COPY)
    ps3dk_prof::zone_guard g2 = g1;
    (void)g2;
#elif defined(PROBE_G_MOVE)
    ps3dk_prof::zone_guard g3 = std::move(g1);
    (void)g3;
#elif !defined(PROBE_G_CONTROL)
#error "define PROBE_G_CONTROL, PROBE_G_COPY or PROBE_G_MOVE"
#endif
}
