// The C++ guard (r2 3.2 / B2): ends its zone at every scope exit, including early return and an exception.
#include "rt.h"

namespace {

int cxx_early(int i)
{
    PS3DK_PROF_ZONE("cxx");
    if (i & 1)
        return 1;
    {
        PS3DK_PROF_ZONE("cxx_inner");
        if (i & 2)
            throw 7;
    }
    return 0;
}

}  // namespace

extern "C" int rt_cxx_guard_scope_exit(const char *out)
{
    ps3dk_prof_config c;
    char path[512];
    rt_cap cap;
    int i, thrown = 0;
    rt_config(&c);
    CHECK_OK(rt_start(out, &c, 1));
    for (i = 0; i < 8; i++) {
        try {
            (void)cxx_early(i);
        } catch (int) {
            thrown++;
        }
    }
    CHECK_EQ(thrown, 2);   // i = 2 and 6
    CHECK_EQ(ps3dk_prof_test_read(PS3DK_PROF_TEST_DEPTH), 0);
    CHECK_EQ(ps3dk_prof_test_read(PS3DK_PROF_TEST_REFUSED), 0);
    CHECK_EQ(ps3dk_prof_shutdown(), PS3DK_PROF_OK);
    CHECK_EQ(rt_capture_path(out, path, sizeof path), 0);
    CHECK_EQ(rt_cap_load(path, &cap), 0);
    CHECK_EQ(rt_count(&cap, 1, RT_BEGIN, -1), 12);   // 8 outer + 4 inner
    CHECK_EQ(rt_count(&cap, 1, RT_END, 0), 12);
    rt_cap_free(&cap);
    return rt_expect(out, 0);
}
