/* sys/spu_thread_group.h - SPU thread-group type + constants.
 *
 * PSL1GHT has sys_spu_thread_group_* syscalls in <sys/spu.h> but
 * doesn't expose the GROUP_TYPE constants; reference SDK samples
 * expect them in this header.  Values match the reference-SDK
 * sys/spu_thread_group.h.
 */
#ifndef __PS3DK_SYS_SPU_THREAD_GROUP_H__
#define __PS3DK_SYS_SPU_THREAD_GROUP_H__

#include <sys/spu.h>

#define SYS_SPU_THREAD_GROUP_TYPE_NORMAL                              0x00
#define SYS_SPU_THREAD_GROUP_TYPE_SEQUENTIAL                          0x01
#define SYS_SPU_THREAD_GROUP_TYPE_SYSTEM                              0x02
#define SYS_SPU_THREAD_GROUP_TYPE_MEMORY_FROM_CONTAINER               0x04
#define SYS_SPU_THREAD_GROUP_TYPE_NON_CONTEXT                         0x08
#define SYS_SPU_THREAD_GROUP_TYPE_EXCLUSIVE_NON_CONTEXT               0x18
#define SYS_SPU_THREAD_GROUP_TYPE_COOPERATE_WITH_SYSTEM               0x20

/* Join-state codes returned by sys_spu_thread_group_join in `*cause`. */
#define SYS_SPU_THREAD_GROUP_JOIN_GROUP_EXIT        0x0001
#define SYS_SPU_THREAD_GROUP_JOIN_ALL_THREADS_EXIT  0x0002
#define SYS_SPU_THREAD_GROUP_JOIN_TERMINATED        0x0004

typedef uint32_t sys_memory_container_t;

/* Reference-SDK snake_case typedef aliases.  PSL1GHT spells the
 * group handle `sys_spu_group_t` and the attribute / argument
 * structs in camelCase; reference samples use _t-suffixed snake_case
 * throughout.  All four aliases are layout-identical to the PSL1GHT
 * spellings so a sample can mix either name without casts. */
typedef sys_spu_group_t            sys_spu_thread_group_t;
typedef sysSpuThreadArgument       sys_spu_thread_argument_t;
typedef sysSpuThreadAttribute      sys_spu_thread_attribute_t;
typedef sysSpuThreadGroupAttribute sys_spu_thread_group_attribute_t;

/* SPU thread option constants (reference-SDK names). */
#ifndef SYS_SPU_THREAD_OPTION_NONE
#define SYS_SPU_THREAD_OPTION_NONE              0x0u
#endif
#ifndef SYS_SPU_THREAD_OPTION_ASYNC_INTR_ENABLE
#define SYS_SPU_THREAD_OPTION_ASYNC_INTR_ENABLE 0x1u
#endif
#ifndef SYS_SPU_THREAD_OPTION_DEC_SYNC_TB_ENABLE
#define SYS_SPU_THREAD_OPTION_DEC_SYNC_TB_ENABLE 0x2u
#endif

/* ------------------------------------------------------------------ *
 * Reference-SDK initialize macros.
 *
 * PSL1GHT ships the same set under sysSpuThread{Attribute,Argument,
 * GroupAttribute}{Initialize,Name}.  Reference samples expect the
 * snake_case spelling.  The macros poke struct fields directly, so
 * the PSL1GHT field-name layout is what we follow:
 *
 *   sysSpuThreadArgument fields are arg0..arg3 (PSL1GHT) — the
 *   reference SDK names them arg1..arg4 but the layout is identical;
 *   the initialize macro just zeros all four so it doesn't matter
 *   which name space callers reach for.
 * ------------------------------------------------------------------ */
#define sys_spu_thread_attribute_initialize(x) \
    do {                                       \
        (x).name   = NULL;                     \
        (x).nsize  = 0;                        \
        (x).option = SYS_SPU_THREAD_OPTION_NONE; \
    } while (0)

#define sys_spu_thread_attribute_name(x, s)    \
    do {                                       \
        (x).name = (s);                        \
        if ((s) == NULL) {                     \
            (x).nsize = 0;                     \
        } else {                               \
            int _n = 0;                        \
            for (; (_n < 127) && ((s)[_n] != '\0'); _n++) {} \
            (x).nsize = _n + 1;                \
        }                                      \
    } while (0)

#define sys_spu_thread_attribute_option(x, f)  \
    do { (x).option = (f); } while (0)

#define sys_spu_thread_argument_initialize(x)  \
    do {                                       \
        (x).arg0 = (x).arg1 = (x).arg2 = (x).arg3 = 0; \
    } while (0)

#define sys_spu_thread_group_attribute_initialize(x) \
    do {                                             \
        (x).name  = NULL;                            \
        (x).nsize = 0;                               \
        (x).type  = SYS_SPU_THREAD_GROUP_TYPE_NORMAL; \
    } while (0)

#define sys_spu_thread_group_attribute_name(x, s)    \
    do {                                             \
        (x).name = (s);                              \
        if ((s) == NULL) {                           \
            (x).nsize = 0;                           \
        } else {                                     \
            int _n = 0;                              \
            for (; (_n < 127) && ((s)[_n] != '\0'); _n++) {} \
            (x).nsize = _n + 1;                      \
        }                                            \
    } while (0)

#define sys_spu_thread_group_attribute_type(x, t)    \
    do { (x).type = (t); } while (0)

#endif /* __PS3DK_SYS_SPU_THREAD_GROUP_H__ */
