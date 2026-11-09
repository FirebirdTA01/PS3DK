/* Runs the actual configure-only constructor and configuration implementation. */
#include "wait_internal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

uint32_t g_gate;
static unsigned checks, failures, allocations, frees;
#define CHECK(x) do { ++checks; if (!(x)) { ++failures; fprintf(stderr,"line %u: %s\n",__LINE__,#x); } } while (0)
void *pal_alloc(size_t n) { ++allocations; return calloc(1,n); }
void pal_free(void *p) { ++frees; free(p); }
extern void __ps3dk_prof_wait_config_anchor(void) __attribute__((weak));
extern int ps3dk_prof_init(const ps3dk_prof_config *) __attribute__((weak));
extern int ps3dk_prof_shutdown(void) __attribute__((weak));

#ifdef EXPLICIT_EARLY
__attribute__((constructor(98))) static void early_override(void)
{
    const ps3dk_prof_wait_config cfg={sizeof cfg,1048576,3,8};
    CHECK(ps3dk_prof_wait_configure(&cfg)==0);
}
#endif

__attribute__((constructor(100))) static void ordering_check(void)
{
    CHECK(__ps3dk_prof_wait_config_anchor != NULL);
    CHECK(p_wait_options.size==sizeof p_wait_options);
    CHECK(p_wait_options.max_bytes==1048576);
#ifdef EXPLICIT_EARLY
    CHECK(p_wait_options.max_actors==3 && p_wait_options.ring_records==8);
#else
    CHECK(p_wait_options.max_actors==0 && p_wait_options.ring_records==128);
#endif
    CHECK(g_gate==G_UNINIT && allocations==0 && frees==0);
    CHECK(ps3dk_prof_init==NULL && ps3dk_prof_shutdown==NULL);
}

int main(void)
{
    prof p; memset(&p,0,sizeof p); p.nslots=4;
    if (__ps3dk_prof_wait_config_anchor) __ps3dk_prof_wait_config_anchor();
    CHECK(p_wait_init(&p)==0);
    if (p.waits) {
#ifdef EXPLICIT_EARLY
        CHECK(p.waits->cfg.max_actors==3 && p.waits->cfg.ring_records==8);
#else
        CHECK(p.waits->cfg.max_actors==4 && p.waits->cfg.ring_records==128);
#endif
        p_wait_unwind(&p);
    }
    CHECK(allocations==1 && frees==1);
    const ps3dk_prof_wait_config cfg={sizeof cfg,1048576,2,16};
    CHECK(ps3dk_prof_wait_configure(&cfg)==0);
    CHECK(p_wait_options.max_actors==2 && p_wait_options.ring_records==16);
    CHECK(p_wait_init(&p)==0);
    if(p.waits) { CHECK(p.waits->cfg.max_actors==2 && p.waits->cfg.ring_records==16); p_wait_unwind(&p); }
    g_gate=G_READY;
    CHECK(ps3dk_prof_wait_configure(&cfg)==PS3DK_PROF_EALREADY);
    CHECK(p_wait_options.max_actors==2);
    printf("%u checks, %u failed\n",checks,failures);
    return failures?1:0;
}
