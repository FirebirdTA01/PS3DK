/* sys/spu_thread_group.h - SPU thread-group type + constants.
 *
 * PSL1GHT has sys_spu_thread_group_* syscalls in <sys/spu.h> but
 * doesn't expose the GROUP_TYPE constants; reference SDK samples
 * expect them in this header.  Values sourced from reference SDK
 * 475.001 sys/spu_thread_group.h.
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

typedef uint32_t sys_memory_container_t;

#endif /* __PS3DK_SYS_SPU_THREAD_GROUP_H__ */
