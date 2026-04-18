/*
 * PS3 Custom Toolchain — <sys/process.h>
 *
 * Process-level SDK surface: spawn/exit primitives, `.sys_proc_param`
 * emitter, per-object-class count constants, and PID accessors.  The
 * constant values below are part of the PS3 LV2 ABI (what the loader
 * reads out of the .sys_proc_param section, what SYSCALL_PROCESS_*
 * syscalls accept).  Function wrappers forward to the numbered LV2
 * syscalls declared in <lv2/syscalls.h>.
 *
 * Sony-name aliases (SYS_PROCESS_PRIMARY_STACK_SIZE_*, exitspawn) are
 * threaded in alongside the long-standing PSL1GHT spellings so code
 * authored against either surface builds unchanged.
 */

#ifndef PS3TC_SYS_PROCESS_H
#define PS3TC_SYS_PROCESS_H

#include <ppu-lv2.h>
#include <ppu-types.h>
#include <lv2/process.h>
#include <lv2/syscalls.h>

/* ---- .sys_proc_param header layout constants ------------------------ */

#define SYS_PROCESS_SPAWN_MAGIC                  0x13bcc5f6

#define SYS_PROCESS_SPAWN_VERSION_1              0x00000001
#define SYS_PROCESS_SPAWN_VERSION_084            0x00008400
#define SYS_PROCESS_SPAWN_VERSION_090            0x00009000
#define SYS_PROCESS_SPAWN_VERSION_330            0x00330000
#define SYS_PROCESS_SPAWN_VERSION_INVALID        0xffffffff

#define SYS_PROCESS_SPAWN_MALLOC_PAGE_SIZE_NONE  0x00000000
#define SYS_PROCESS_SPAWN_MALLOC_PAGE_SIZE_64K   0x00010000
#define SYS_PROCESS_SPAWN_MALLOC_PAGE_SIZE_1M    0x00100000

#define SYS_PROCESS_SPAWN_FW_VERSION_192         0x00192001
#define SYS_PROCESS_SPAWN_FW_VERSION_330         0x00330000
#define SYS_PROCESS_SPAWN_FW_VERSION_UNKNOWN     0xffffffff
#define SYS_PROCESS_SPAWN_SDK_VERSION_UNKNOWN    0xffffffff

#define SYS_PROCESS_SPAWN_PPC_SEG_DEFAULT        0x00000000
#define SYS_PROCESS_SPAWN_PPC_SEG_OVLM           0x00000001
#define SYS_PROCESS_SPAWN_PPC_SEG_PRX            0x00000002

/* Primary-thread stack-size tags written into the .sys_proc_param
 * header.  These are byte-values the loader interprets as 32K … 1M
 * requests.  Both PSL1GHT's long-standing SPAWN_STACK_SIZE_* names
 * and the Sony-spelled PRIMARY_STACK_SIZE_* names resolve to the same
 * underlying constants. */
#define SYS_PROCESS_SPAWN_STACK_SIZE_32K         0x10
#define SYS_PROCESS_SPAWN_STACK_SIZE_64K         0x20
#define SYS_PROCESS_SPAWN_STACK_SIZE_96K         0x30
#define SYS_PROCESS_SPAWN_STACK_SIZE_128K        0x40
#define SYS_PROCESS_SPAWN_STACK_SIZE_256K        0x50
#define SYS_PROCESS_SPAWN_STACK_SIZE_512K        0x60
#define SYS_PROCESS_SPAWN_STACK_SIZE_1M          0x70

#define SYS_PROCESS_PRIMARY_STACK_SIZE_32K       SYS_PROCESS_SPAWN_STACK_SIZE_32K
#define SYS_PROCESS_PRIMARY_STACK_SIZE_64K       SYS_PROCESS_SPAWN_STACK_SIZE_64K
#define SYS_PROCESS_PRIMARY_STACK_SIZE_96K       SYS_PROCESS_SPAWN_STACK_SIZE_96K
#define SYS_PROCESS_PRIMARY_STACK_SIZE_128K      SYS_PROCESS_SPAWN_STACK_SIZE_128K
#define SYS_PROCESS_PRIMARY_STACK_SIZE_256K      SYS_PROCESS_SPAWN_STACK_SIZE_256K
#define SYS_PROCESS_PRIMARY_STACK_SIZE_512K      SYS_PROCESS_SPAWN_STACK_SIZE_512K
#define SYS_PROCESS_PRIMARY_STACK_SIZE_1M        SYS_PROCESS_SPAWN_STACK_SIZE_1M

/* ---- .sys_proc_param section emitter -------------------------------- */
/*
 * We build the 36-byte .sys_proc_param record via inline asm so the
 * trailing crash_dump_param_addr word can be an R_PPC64_ADDR32
 * relocation against __sys_process_crash_dump_param.  A plain C
 * struct initializer can't express that — (uint32_t)(uintptr_t)&func
 * isn't a constant expression under ELFv1 PPC64, where function
 * descriptors resolve at link time.  Declaring the weak extern inside
 * the macro (via __asm__(".weak …")) keeps the pseudo-symbol at file
 * scope without dragging a typed C declaration into every TU.  If no
 * user crash-dump callback is linked in, the relocation resolves to
 * zero and the loader skips the hook.
 */
#define PS3TC_PROC_PARAM_STRINGIFY_EXPAND(x) #x
#define PS3TC_PROC_PARAM_STRINGIFY(x) PS3TC_PROC_PARAM_STRINGIFY_EXPAND(x)

#define PS3TC_SYS_PROCESS_PARAM_EMIT(prio, stacksize, ppc_seg)               \
    __asm__ (                                                                \
        ".weak __sys_process_crash_dump_param\n"                             \
        ".section \".sys_proc_param\",\"a\",@progbits\n"                     \
        ".balign 8\n"                                                        \
        ".globl __sys_process_param\n"                                       \
        ".type __sys_process_param, @object\n"                               \
        ".size __sys_process_param, 36\n"                                    \
        "__sys_process_param:\n"                                             \
        ".long 36\n"                                                         \
        ".long " PS3TC_PROC_PARAM_STRINGIFY(SYS_PROCESS_SPAWN_MAGIC) "\n"    \
        ".long " PS3TC_PROC_PARAM_STRINGIFY(SYS_PROCESS_SPAWN_VERSION_330) "\n" \
        ".long " PS3TC_PROC_PARAM_STRINGIFY(SYS_PROCESS_SPAWN_SDK_VERSION_UNKNOWN) "\n" \
        ".long " PS3TC_PROC_PARAM_STRINGIFY(prio) "\n"                       \
        ".long " PS3TC_PROC_PARAM_STRINGIFY(stacksize) "\n"                  \
        ".long " PS3TC_PROC_PARAM_STRINGIFY(SYS_PROCESS_SPAWN_MALLOC_PAGE_SIZE_1M) "\n" \
        ".long " PS3TC_PROC_PARAM_STRINGIFY(ppc_seg) "\n"                    \
        ".long __sys_process_crash_dump_param\n"                             \
    )

#define SYS_PROCESS_PARAM(prio, stacksize) \
    PS3TC_SYS_PROCESS_PARAM_EMIT(prio, stacksize, SYS_PROCESS_SPAWN_PPC_SEG_DEFAULT)

#define SYS_PROCESS_PARAM_OVLM(prio, stacksize) \
    PS3TC_SYS_PROCESS_PARAM_EMIT(prio, stacksize, SYS_PROCESS_SPAWN_PPC_SEG_OVLM)

#define SYS_PROCESS_PARAM_FIXED(prio, stacksize) \
    PS3TC_SYS_PROCESS_PARAM_EMIT(prio, stacksize, SYS_PROCESS_SPAWN_PPC_SEG_PRX)

/* ---- object-class identifiers (SYS_OBJECT_*) ------------------------ */
/* sysProcessGetNumberOfObject reports population per class. */
#define SYS_OBJECT_MEM                   (0x08UL)
#define SYS_OBJECT_MUTEX                 (0x85UL)
#define SYS_OBJECT_COND                  (0x86UL)
#define SYS_OBJECT_RWLOCK                (0x88UL)
#define SYS_OBJECT_INTR_TAG              (0x0AUL)
#define SYS_OBJECT_INTR_SERVICE_HANDLE   (0x0BUL)
#define SYS_OBJECT_EVENT_QUEUE           (0x8DUL)
#define SYS_OBJECT_EVENT_PORT            (0x0EUL)
#define SYS_OBJECT_TRACE                 (0x21UL)
#define SYS_OBJECT_SPUIMAGE              (0x22UL)
#define SYS_OBJECT_PRX                   (0x23UL)
#define SYS_OBJECT_SPUPORT               (0x24UL)
#define SYS_OBJECT_LWMUTEX               (0x95UL)
#define SYS_OBJECT_TIMER                 (0x11UL)
#define SYS_OBJECT_SEMAPHORE             (0x96UL)
#define SYS_OBJECT_FS_FD                 (0x73UL)
#define SYS_OBJECT_LWCOND                (0x97UL)
#define SYS_OBJECT_EVENT_FLAG            (0x98UL)

#ifdef __cplusplus
extern "C" {
#endif

/* ---- parsed representation of the .sys_proc_param record ----------- */
typedef struct _sys_process_param
{
    u32 size;
    u32 magic;
    u32 version;
    u32 sdk_version;
    s32 prio;
    u32 stacksize;
    u32 malloc_pagesize;
    u32 ppc_seg;
    u32 crash_dump_param_addr;
} sys_process_param_t;

/* ---- syscall wrappers ---------------------------------------------- */

LV2_SYSCALL sysProcessGetPid(void)
{
    lv2syscall0(SYSCALL_PROCESS_GETPID);
    return_to_user_prog(sys_pid_t);
}

LV2_SYSCALL sysProcessGetPpid(void)
{
    lv2syscall0(SYSCALL_PROCESS_GETPPID);
    return_to_user_prog(sys_pid_t);
}

LV2_SYSCALL sysProcessGetNumberOfObject(u32 object, size_t *count_out)
{
    lv2syscall2(SYSCALL_PROCESS_GET_NUMBER_OF_OBJECT,
                (u32)object, (u32)(u64)count_out);
    return_to_user_prog(s32);
}

LV2_SYSCALL sysProcessIsSpuLockLinkReservation(u32 addr, u64 flags)
{
    lv2syscall2(SYSCALL_PROCESS_IS_SPU_LOCK_LINE_RESERVATION_ADDRESS,
                (u32)addr, flags);
    return_to_user_prog(s32);
}

LV2_SYSCALL sysProcessGetPpuGuid(void)
{
    lv2syscall0(SYSCALL_PROCESS_GET_PPU_GUID);
    return_to_user_prog(sys_addr_t);
}

/* ---- exit / spawn -------------------------------------------------- */
/* Replace the current process with a new one loaded from `path`,
 * passing `argv` and `envp` through and seeding an optional data
 * buffer (a user-space address — sys_addr_t — not a native pointer)
 * for the target.  Mirrors the sys_process_exitspawn2 LV2 syscall;
 * `priority` and `flags` are forwarded verbatim. */
static inline void exitspawn(const char *path,
                             const char *argv[],
                             const char *envp[],
                             sys_addr_t data, size_t size,
                             int priority, u64 flags)
{
    sysProcessExitSpawn2(path, argv, envp,
                         (void *)(uintptr_t)data, size,
                         priority, flags);
}

#ifdef __cplusplus
}
#endif

#endif  /* PS3TC_SYS_PROCESS_H */
