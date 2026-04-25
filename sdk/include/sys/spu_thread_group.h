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

#endif /* __PS3DK_SYS_SPU_THREAD_GROUP_H__ */
