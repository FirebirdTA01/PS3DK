/* I3a off build of prof_spu_lane.h (PS3DK_PROF undefined or 0), two independent proofs that no macro evaluates an
 * argument or references the runtime:
 *   - link: every macro argument contains a call to ps3dk_prof__offbuild_sentinel(), which is DEFINED NOWHERE; an
 *     evaluated argument leaves an undefined reference and the link fails (SPU and host links, no core or writer);
 *   - run (host, C and C++): every argument also increments n or calls side(); main returns 0 and prints
 *     "OFFBUILD PASS" only if both counters are still 0 and every result is ENOTSUP (0 for a zone index).
 * No target execution: on SPU this is linked only. */
#include <stdio.h>

#include "prof_spu_lane.h"

#if defined(PS3DK_PROF) && PS3DK_PROF
#error "the off-build fixture must be built with PS3DK_PROF undefined or 0"
#endif

#ifdef __cplusplus
extern "C" {
#endif
int ps3dk_prof__offbuild_sentinel(void); /* never defined */
#ifdef __cplusplus
}
#endif

static int g_n;
__attribute__((unused)) static int side(void) { return ++g_n; }
#define ARG(x) (n++, (void)side(), (void)ps3dk_prof__offbuild_sentinel(), x)

#if defined(__SPU__)
int main(unsigned long long spe, unsigned long long argp, unsigned long long envp)
#else
int main(void)
#endif
{
    ps3dk_prof_spu_writer w;
    int n = 0, r = 0;
    uint32_t z;
#if defined(__SPU__)
    (void)spe;
    (void)argp;
    (void)envp;
#endif
    r |= PS3DK_PROF_SPU_ATTACH(ARG(&w), ARG(1ull), ARG(1024u), ARG(2ull), ARG(5u), ARG((void *)0), ARG(16u),
                               ARG(0u)) != PS3DK_PROF_SPU_ENOTSUP;
    z = PS3DK_PROF_SPU_ZONE_BEGIN(ARG(&w), ARG("z"));
    PS3DK_PROF_SPU_ZONE_END(ARG(&w), ARG(z));
    PS3DK_PROF_SPU_MARK(ARG(&w), ARG("m"));
    r |= PS3DK_PROF_SPU_FLUSH(ARG(&w)) != PS3DK_PROF_SPU_ENOTSUP;
    r |= PS3DK_PROF_SPU_FINI(ARG(&w)) != PS3DK_PROF_SPU_ENOTSUP;
    r |= PS3DK_PROF_SPU_DEC_WRITE(ARG(&w), ARG(3u)) != PS3DK_PROF_SPU_ENOTSUP;
    if (r || z != 0u || n != 0 || g_n != 0) {
        printf("OFFBUILD FAIL r %d z %u n %d side %d\n", r, (unsigned)z, n, g_n);
        return 1;
    }
    printf("OFFBUILD PASS\n");
    return 0;
}
