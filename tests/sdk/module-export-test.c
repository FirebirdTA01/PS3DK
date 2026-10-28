/*
 * Probe for <sys/moduleexport.h>; see module-export-test.sh.
 *
 * CASE_GOOD (default): a module declaring its identity and entry points.
 * The other CASE_* values must each fail to compile.
 */
#include <stddef.h>
#include <sys/moduleexport.h>

#ifdef __cplusplus
#define PROBE_C extern "C"
#else
#define PROBE_C
#endif

PROBE_C int probe_start(size_t args, void *argp);
PROBE_C int probe_stop(size_t args, void *argp);

#if defined(CASE_NAME_TOO_LONG)
SYS_MODULE_INFO(a_module_name_of_28_chars_xx, 0, 1, 0);
#elif defined(CASE_NAME_EMPTY)
SYS_MODULE_INFO(, 0, 1, 0);
#elif defined(CASE_ATTR_TOO_WIDE)
SYS_MODULE_INFO(probe_mod, 0x10000, 1, 0);
#elif defined(CASE_VERSION_TOO_WIDE)
SYS_MODULE_INFO(probe_mod, 0, 256, 0);
#elif defined(CASE_EXIT)
SYS_MODULE_INFO(probe_mod, 0, 1, 0);
SYS_MODULE_EXIT(probe_stop);
#elif defined(CASE_STOP_LEVEL)
SYS_MODULE_INFO(probe_mod, 0, 1, 0);
SYS_MODULE_STOP_LEVEL(SYS_MODULE_STOP_LEVEL_SYSTEM);
#else
SYS_MODULE_INFO(probe_mod_27_character_long,
                SYS_MODULE_ATTR_CANT_STOP | SYS_MODULE_ATTR_EXCLUSIVE_START, 3, 9);
SYS_MODULE_START(probe_start);
SYS_MODULE_STOP(probe_stop);
#endif

PROBE_C int probe_start(size_t args, void *argp)
{
    (void)args;
    (void)argp;
    return SYS_PRX_RESIDENT;
}

PROBE_C int probe_stop(size_t args, void *argp)
{
    (void)args;
    (void)argp;
    return SYS_PRX_RESIDENT;
}

/* The constants a module source may use. */
typedef char probe_constants[(SYS_MODULE_NAME_LEN == 27 &&
                              SYS_MODULE_ATTR_CANT_STOP == 0x0001 &&
                              SYS_MODULE_ATTR_EXCLUSIVE_LOAD == 0x0002 &&
                              SYS_MODULE_ATTR_EXCLUSIVE_START == 0x0004 &&
                              SYS_MODULE_STOP_LEVEL_USER == 0x00000000 &&
                              SYS_MODULE_STOP_LEVEL_SYSTEM == 0x00004000)
                             ? 1 : -1];
