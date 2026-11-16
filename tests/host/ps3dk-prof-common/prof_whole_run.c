/* Shared host-test helper: every successful profiler init is followed by PS3DK_PROF_CAPTURE_WHOLE_RUN(), so a test
 * written before capture windows records through the same windowed path as a real application that asks for its
 * whole run. Linked with -Wl,--wrap=ps3dk_prof_init -Wl,--wrap=ps3dk_prof_init_ex (the explicit inits of a test and
 * the automatic init at startup alike). A test of the window API itself, or of an application with no trigger, does
 * not link it.  It always needs the live window API, whatever flags the including test compiles it with. */
#ifndef PS3DK_PROF
#define PS3DK_PROF 1
#endif
#include <ps3dk/prof.h>

#ifdef __cplusplus
extern "C" {
#endif
int __real_ps3dk_prof_init(const ps3dk_prof_config *cfg);
int __real_ps3dk_prof_init_ex(const ps3dk_prof_config *cfg, const void *ext);
int __wrap_ps3dk_prof_init(const ps3dk_prof_config *cfg);
int __wrap_ps3dk_prof_init_ex(const ps3dk_prof_config *cfg, const void *ext);

/* A build without the extension has no ps3dk_prof_init_ex: this weak stand-in (never called there) resolves the
 * wrapper's reference; the library's own definition wins wherever it exists. */
int ps3dk_prof_init_ex(const ps3dk_prof_config *cfg, const void *ext) __attribute__((weak));
int ps3dk_prof_init_ex(const ps3dk_prof_config *cfg, const void *ext)
{
    (void)cfg;
    (void)ext;
    return PS3DK_PROF_ENOTSUP;
}

static void whole_run(void)
{
    (void)ps3dk_prof_capture_begin((ps3dk_prof_desc *)0, PS3DK_PROF_WINDOW_WHOLE, 0u);
}
int __wrap_ps3dk_prof_init(const ps3dk_prof_config *cfg)
{
    int rc = __real_ps3dk_prof_init(cfg);
    if (rc == PS3DK_PROF_OK)
        whole_run();
    return rc;
}
int __wrap_ps3dk_prof_init_ex(const ps3dk_prof_config *cfg, const void *ext)
{
    int rc = __real_ps3dk_prof_init_ex(cfg, ext);
    if (rc == PS3DK_PROF_OK)
        whole_run();
    return rc;
}
#ifdef __cplusplus
}
#endif
