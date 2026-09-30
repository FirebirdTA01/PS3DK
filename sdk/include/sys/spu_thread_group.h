/* sys/spu_thread_group.h - SPU thread-group type + constants.
 *
 * PSL1GHT has sys_spu_thread_group_* syscalls in <sys/spu.h> but
 * doesn't expose the GROUP_TYPE constants; reference SDK samples
 * expect them in this header.  Values match the reference-SDK
 * sys/spu_thread_group.h.
 */
#ifndef __PS3DK_SYS_SPU_THREAD_GROUP_H__
#define __PS3DK_SYS_SPU_THREAD_GROUP_H__

#include <stdint.h>
#include <stddef.h>
#include <sys/return_code.h>

#include <ppu-lv2.h>
#include <ppu-types.h>
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

#ifndef _SYS_EVENT_TYPE_T_DEFINED
#define _SYS_EVENT_TYPE_T_DEFINED
typedef uint32_t sys_event_type_t;
#endif

/* sys_spu_thread_group_connect_event event types, and the event source
 * keys an event queue receives them under. */
#define SYS_SPU_THREAD_GROUP_EVENT_RUN               0x1
#define SYS_SPU_THREAD_GROUP_EVENT_EXCEPTION         0x2
#define SYS_SPU_THREAD_GROUP_EVENT_SYSTEM_MODULE     0x4
#define SYS_SPU_THREAD_GROUP_EVENT_RUN_KEY           0xFFFFFFFF53505500ULL
#define SYS_SPU_THREAD_GROUP_EVENT_EXCEPTION_KEY     0xFFFFFFFF53505503ULL
#define SYS_SPU_THREAD_GROUP_EVENT_SYSTEM_MODULE_KEY 0xFFFFFFFF53505504ULL

/* Exception causes reported by the group's exception event. */
#define SYS_SPU_EXCEPTION_NO_VALUE          0x0U
#define SYS_SPU_EXCEPTION_DMA_ALIGNMENT     0x0001U
#define SYS_SPU_EXCEPTION_DMA_COMMAND       0x0002U
#define SYS_SPU_EXCEPTION_SPU_ERROR         0x0004U
#define SYS_SPU_EXCEPTION_MFC_FIR           0x0008U
#define SYS_SPU_EXCEPTION_MFC_SEGMENT       0x0010U
#define SYS_SPU_EXCEPTION_MFC_STORAGE       0x0020U
#define SYS_SPU_EXCEPTION_STOP_CALL         0x0100U
#define SYS_SPU_EXCEPTION_STOP_BREAK        0x0200U
#define SYS_SPU_EXCEPTION_HALT              0x0400U
#define SYS_SPU_EXCEPTION_UNKNOWN_SIGNAL    0x0800U
#define SYS_SPU_EXCEPTION_NON_CONTEXT       0x1000U
#define SYS_SPU_EXCEPTION_MAT               0x2000U

/* sys_spu_thread_group_log commands. */
#define SYS_SPU_THREAD_GROUP_LOG_ON         0x0
#define SYS_SPU_THREAD_GROUP_LOG_OFF        0x1
#define SYS_SPU_THREAD_GROUP_LOG_GET_STATUS 0x2

#ifndef _SYS_MEMORY_CONTAINER_T_DEFINED
#define _SYS_MEMORY_CONTAINER_T_DEFINED
typedef uint32_t sys_memory_container_t;
#endif

/* Reference-SDK snake_case typedef aliases.  PSL1GHT spells the
 * group handle `sys_spu_group_t` and the attribute
 * structs in camelCase; reference samples use _t-suffixed snake_case
 * throughout.  These typedef aliases are layout-identical to the PSL1GHT
 * spellings.  (Note: the canonical thread argument type sys_spu_thread_argument_t
 * is defined below with Sony-standard arg1..arg4 member naming, distinct from
 * PSL1GHT's sysSpuThreadArgument arg0..arg3). */
typedef sys_spu_group_t            sys_spu_thread_group_t;
typedef sysSpuThreadAttribute      sys_spu_thread_attribute_t;
typedef sysSpuThreadGroupAttribute sys_spu_thread_group_attribute_t;

/* Canonical reference-SDK sys_spu_thread_argument_t (arg1..arg4).
 * Distinct from legacy PSL1GHT sysSpuThreadArgument (arg0..arg3). */
#ifndef _SYS_SPU_THREAD_ARGUMENT_T_DEFINED
#define _SYS_SPU_THREAD_ARGUMENT_T_DEFINED
typedef struct sys_spu_thread_argument {
    uint64_t arg1;
    uint64_t arg2;
    uint64_t arg3;
    uint64_t arg4;
} sys_spu_thread_argument_t;
#endif

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
        (x).arg1 = (x).arg2 = (x).arg3 = (x).arg4 = 0; \
    } while (0)

#ifndef sysSpuThreadArgumentInitialize
#define sysSpuThreadArgumentInitialize(x)      \
    do {                                       \
        (x).arg0 = (x).arg1 = (x).arg2 = (x).arg3 = 0; \
    } while (0)
#endif

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

/* Create the group's SPU threads in memory container `c`. */
#define sys_spu_thread_group_attribute_memory_container(x, c)          \
    do {                                                             \
        (x).type |= SYS_SPU_THREAD_GROUP_TYPE_MEMORY_FROM_CONTAINER; \
        (x).option.ct = (c);                                         \
    } while (0)

/* ------------------------------------------------------------------ *
 * Group scheduling, events and logging (Lv-2 syscalls 167..254)
 * ------------------------------------------------------------------ */

#ifdef __cplusplus
extern "C" {
#endif

static inline int sys_spu_thread_group_suspend(sys_spu_thread_group_t id)
{
    lv2syscall1(174, id);
    return_to_user_prog(int);
}

static inline int sys_spu_thread_group_resume(sys_spu_thread_group_t id)
{
    lv2syscall1(175, id);
    return_to_user_prog(int);
}

/* Give up the group's SPUs to waiting groups of the same priority. */
static inline int sys_spu_thread_group_yield(sys_spu_thread_group_t id)
{
    lv2syscall1(176, id);
    return_to_user_prog(int);
}

static inline int sys_spu_thread_group_set_priority(sys_spu_thread_group_t id, int priority)
{
    lv2syscall2(179, id, priority);
    return_to_user_prog(int);
}

static inline int sys_spu_thread_group_get_priority(sys_spu_thread_group_t id, int *priority)
{
    lv2syscall2(180, id, (u64)(uintptr_t)priority);
    return_to_user_prog(int);
}

/* Start `ngroups` other groups when group `gid` exits. */
static inline int sys_spu_thread_group_start_on_exit(sys_spu_thread_group_t gid, int ngroups,
                                                     sys_spu_thread_group_t *groups)
{
    lv2syscall3(167, gid, ngroups, (u64)(uintptr_t)groups);
    return_to_user_prog(int);
}

static inline int sys_spu_thread_group_connect_event(sys_spu_thread_group_t id,
                                                     sys_event_queue_t eq, sys_event_type_t et)
{
    lv2syscall3(185, id, eq, et);
    return_to_user_prog(int);
}

static inline int sys_spu_thread_group_disconnect_event(sys_spu_thread_group_t id,
                                                        sys_event_type_t et)
{
    lv2syscall2(186, id, et);
    return_to_user_prog(int);
}

/* Connect every thread's user events to `eq` on the first free SPU port
 * in the `req` bitmap; the port chosen is stored to *spup. */
static inline int sys_spu_thread_group_connect_event_all_threads(sys_spu_thread_group_t id,
                                                                 sys_event_queue_t eq,
                                                                 uint64_t req, uint8_t *spup)
{
    lv2syscall4(251, id, eq, req, (u64)(uintptr_t)spup);
    return_to_user_prog(int);
}

static inline int sys_spu_thread_group_disconnect_event_all_threads(sys_spu_thread_group_t id,
                                                                    uint8_t spup)
{
    lv2syscall2(252, id, spup);
    return_to_user_prog(int);
}

static inline int sys_spu_thread_group_set_cooperative_victims(sys_spu_thread_group_t id,
                                                               uint32_t victims)
{
    lv2syscall2(250, id, victims);
    return_to_user_prog(int);
}

/* SYS_SPU_THREAD_GROUP_LOG_ON / _OFF / _GET_STATUS; the status is
 * stored to *stat. */
static inline int sys_spu_thread_group_log(int command, int *stat)
{
    lv2syscall2(254, command, (u64)(uintptr_t)stat);
    return_to_user_prog(int);
}

#ifdef __cplusplus
}
#endif

#endif /* __PS3DK_SYS_SPU_THREAD_GROUP_H__ */
