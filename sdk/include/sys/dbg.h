/*
 * sys/dbg.h - sys_lv2dbg public declarations.
 *
 * Backed by liblv2dbg_stub.a emitted from
 * tools/nidgen/nids/extracted/liblv2dbg_stub.yaml. Link with
 * -llv2dbg_stub and load CELL_SYSMODULE_LV2DBG before making calls.
 */
#ifndef __PS3DK_SYS_DBG_H__
#define __PS3DK_SYS_DBG_H__

#include <stddef.h>
#include <stdint.h>
#include <ppu-types.h>
#include <sys/spu_thread_group.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SYS_DBG_EVENT_PPU_EXCEPTION_HANDLER_SIGNALED 0x0000000000002000ULL

#define SYS_DBG_DABR_CTRL_READ  0x0000000000000005ULL
#define SYS_DBG_DABR_CTRL_WRITE 0x0000000000000006ULL
#define SYS_DBG_DABR_CTRL_CLEAR 0x0000000000000000ULL

#define SYS_DBG_MAT_TRANSPARENT 1ULL
#define SYS_DBG_MAT_WRITE       2ULL
#define SYS_DBG_MAT_READ_WRITE  4ULL
#define SYS_MAT_GRANULARITY     4096U

#define SYS_VM_STATE_LOCKED 0x0008ULL
#define SYS_VM_STATE_DIRTY  0x0010ULL

#ifndef SYS_DBG_PPU_GPR_NUM
#define SYS_DBG_PPU_GPR_NUM 32
#endif
#ifndef SYS_DBG_PPU_FPR_NUM
#define SYS_DBG_PPU_FPR_NUM 32
#endif
#ifndef SYS_DBG_PPU_VR_NUM
#define SYS_DBG_PPU_VR_NUM 32
#endif
#ifndef SYS_DBG_SPU_GPR_NUM
#define SYS_DBG_SPU_GPR_NUM 128
#endif

typedef uint32_t sys_rwlock_t;
typedef sys_sem_t sys_semaphore_t;
typedef uint32_t sys_event_flag_t;
typedef uint32_t sys_lwmutex_pseudo_id_t;
typedef uint32_t sys_lwcond_pseudo_id_t;

typedef struct sys_dbg_vr {
    uint32_t word[4];
} sys_dbg_vr_t;

typedef struct sys_dbg_ppu_thread_context {
    uint64_t gpr[SYS_DBG_PPU_GPR_NUM];
    uint64_t cr;
    uint64_t xer;
    uint64_t lr;
    uint64_t ctr;
    uint64_t pc;
    uint64_t fpscr;
    double   fpr[SYS_DBG_PPU_FPR_NUM];
    sys_dbg_vr_t vr[SYS_DBG_PPU_VR_NUM];
    sys_dbg_vr_t vscr;
} sys_dbg_ppu_thread_context_t;

typedef struct sys_dbg_spu_gpr {
    uint32_t word[4];
} sys_dbg_spu_gpr_t;

typedef struct sys_dbg_spu_fpscr {
    uint32_t word[4];
} sys_dbg_spu_fpscr_t;

typedef struct sys_dbg_spu_thread_context {
    sys_dbg_spu_gpr_t gpr[SYS_DBG_SPU_GPR_NUM];
    uint32_t npc;
    uint32_t fpscr;
    uint32_t mb_stat;
    uint32_t ppu_mb;
    uint32_t spu_mb[4];
    uint32_t decrementer;
    uint32_t mfc_cq_sr[96];
} sys_dbg_spu_thread_context_t;

typedef struct sys_dbg_spu_thread_context2 {
    sys_dbg_spu_gpr_t gpr[SYS_DBG_SPU_GPR_NUM];
    uint32_t npc;
    sys_dbg_spu_fpscr_t fpscr;
    uint32_t mb_stat;
    uint32_t ppu_mb;
    uint32_t spu_mb[4];
    uint32_t decrementer;
    uint32_t mfc_cq_sr[96];
} sys_dbg_spu_thread_context2_t;

typedef struct sys_dbg_mutex_information {
    sys_ppu_thread_t owner;
    uint32_t lock_counter;
    uint32_t cond_ref_counter;
    uint32_t cond_id;
} sys_dbg_mutex_information_t;

typedef struct sys_dbg_cond_information {
    sys_mutex_t mutex_id;
    uint32_t wait_threads_num;
    sys_ppu_thread_t wait_threads[16];
} sys_dbg_cond_information_t;

typedef struct sys_dbg_rwlock_information {
    sys_ppu_thread_t owner;
    uint32_t readers;
    uint32_t writers;
    uint32_t waiting_readers;
    uint32_t waiting_writers;
} sys_dbg_rwlock_information_t;

typedef struct sys_dbg_event_queue_information {
    uint32_t queue_size;
    uint32_t event_queue_depth;
    uint32_t wait_threads_num;
    sys_ppu_thread_t wait_threads[16];
} sys_dbg_event_queue_information_t;

typedef struct sys_dbg_semaphore_information {
    int32_t current_val;
    int32_t max_val;
    uint32_t wait_threads_num;
    sys_ppu_thread_t wait_threads[16];
} sys_dbg_semaphore_information_t;

typedef struct sys_dbg_lwmutex_information {
    sys_ppu_thread_t owner;
    uint32_t lock_counter;
    uint32_t wait_threads_num;
    sys_ppu_thread_t wait_threads[16];
} sys_dbg_lwmutex_information_t;

typedef struct sys_dbg_lwcond_information {
    sys_lwmutex_pseudo_id_t lwmutex_id;
    uint32_t wait_threads_num;
    sys_ppu_thread_t wait_threads[16];
} sys_dbg_lwcond_information_t;

typedef struct sys_dbg_event_flag_wait_information {
    sys_ppu_thread_t thread_id;
    uint64_t bitptn;
    uint32_t mode;
    uint32_t pad;
} sys_dbg_event_flag_wait_information_t;

typedef struct sys_dbg_event_flag_information {
    uint64_t bitptn;
    uint32_t wait_threads_num;
    uint32_t pad;
    sys_dbg_event_flag_wait_information_t *wait_info_list ATTRIBUTE_PRXPTR;
} sys_dbg_event_flag_information_t;

typedef enum sys_dbg_ppu_thread_status {
    PPU_THREAD_STATUS_IDLE,
    PPU_THREAD_STATUS_RUNNABLE,
    PPU_THREAD_STATUS_ONPROC,
    PPU_THREAD_STATUS_SLEEP,
    PPU_THREAD_STATUS_STOP,
    PPU_THREAD_STATUS_ZOMBIE,
    PPU_THREAD_STATUS_DELETED,
    PPU_THREAD_STATUS_UNKNOWN
} sys_dbg_ppu_thread_status_t;

typedef enum sys_dbg_spu_thread_group_status {
    SPU_THREAD_GROUP_STATUS_NOT_INITIALIZED,
    SPU_THREAD_GROUP_STATUS_INITIALIZED,
    SPU_THREAD_GROUP_STATUS_READY,
    SPU_THREAD_GROUP_STATUS_WAITING,
    SPU_THREAD_GROUP_STATUS_SUSPENDED,
    SPU_THREAD_GROUP_STATUS_WAITING_AND_SUSPENDED,
    SPU_THREAD_GROUP_STATUS_RUNNING,
    SPU_THREAD_GROUP_STATUS_STOPPED,
    SPU_THREAD_GROUP_STATUS_UNKNOWN
} sys_dbg_spu_thread_group_status_t;

typedef struct sys_vm_page_information {
    uint64_t state;
} sys_vm_page_information_t;

typedef enum sys_dbg_coredump_parameter {
    SYS_DBG_COREDUMP_OFF,
    SYS_DBG_COREDUMP_ON_SAVE_TO_APP_HOME,
    SYS_DBG_COREDUMP_ON_SAVE_TO_DEV_MS,
    SYS_DBG_COREDUMP_ON_SAVE_TO_DEV_USB,
    SYS_DBG_COREDUMP_ON_SAVE_TO_DEV_HDD0
} sys_dbg_coredump_parameter_t;

typedef void (*dbg_exception_handler_t)(uint64_t exception_type,
                                        sys_ppu_thread_t id,
                                        uint64_t dar);

int sys_dbg_read_ppu_thread_context(sys_ppu_thread_t id,
                                    sys_dbg_ppu_thread_context_t *ppu_context);
int sys_dbg_read_spu_thread_context(sys_spu_thread_t id,
                                    sys_dbg_spu_thread_context_t *spu_context);
int sys_dbg_read_spu_thread_context2(sys_spu_thread_t id,
                                     sys_dbg_spu_thread_context2_t *spu_context);
int sys_dbg_set_stacksize_ppu_exception_handler(size_t stacksize);
int sys_dbg_initialize_ppu_exception_handler(int prio);
int sys_dbg_finalize_ppu_exception_handler(void);
int sys_dbg_register_ppu_exception_handler(dbg_exception_handler_t callback,
                                           uint64_t ctrl_flags);
int sys_dbg_unregister_ppu_exception_handler(void);
int sys_dbg_signal_to_ppu_exception_handler(uint64_t flags);
int sys_dbg_get_mutex_information(sys_mutex_t id,
                                  sys_dbg_mutex_information_t *info);
int sys_dbg_get_cond_information(sys_cond_t id,
                                 sys_dbg_cond_information_t *info);
int sys_dbg_get_rwlock_information(sys_rwlock_t id,
                                   sys_dbg_rwlock_information_t *info);
int sys_dbg_get_event_queue_information(sys_event_queue_t id,
                                        sys_dbg_event_queue_information_t *info);
int sys_dbg_get_semaphore_information(sys_semaphore_t id,
                                      sys_dbg_semaphore_information_t *info);
int sys_dbg_get_lwmutex_information(sys_lwmutex_pseudo_id_t id,
                                    sys_dbg_lwmutex_information_t *info);
int sys_dbg_get_lwcond_information(sys_lwcond_pseudo_id_t id,
                                   sys_dbg_lwcond_information_t *info);
int sys_dbg_get_event_flag_information(sys_event_flag_t id,
                                       sys_dbg_event_flag_information_t *info);
int sys_dbg_get_ppu_thread_ids(sys_ppu_thread_t *ids,
                               uint64_t *ids_num,
                               uint64_t *all_ids_num);
int sys_dbg_get_spu_thread_group_ids(sys_spu_thread_group_t *ids,
                                     uint64_t *ids_num,
                                     uint64_t *all_ids_num);
int sys_dbg_get_spu_thread_ids(sys_spu_thread_group_t spu_thread_group_id,
                               sys_spu_thread_t *ids,
                               uint64_t *ids_num,
                               uint64_t *all_ids_num);
int sys_dbg_get_ppu_thread_name(sys_ppu_thread_t id, char *name);
int sys_dbg_get_spu_thread_name(sys_spu_thread_t id, char *name);
int sys_dbg_get_spu_thread_group_name(sys_spu_thread_group_t id, char *name);
int sys_dbg_get_ppu_thread_status(sys_ppu_thread_t id,
                                  sys_dbg_ppu_thread_status_t *status);
int sys_dbg_get_spu_thread_group_status(sys_spu_thread_group_t id,
                                        sys_dbg_spu_thread_group_status_t *status);
int sys_dbg_enable_floating_point_enabled_exception(sys_ppu_thread_t id,
                                                   uint64_t flags,
                                                   uint64_t opt1,
                                                   uint64_t opt2);
int sys_dbg_disable_floating_point_enabled_exception(sys_ppu_thread_t id,
                                                    uint64_t flags,
                                                    uint64_t opt1,
                                                    uint64_t opt2);
int sys_dbg_vm_get_page_information(sys_addr_t addr, unsigned int num,
                                    sys_vm_page_information_t *pageinfo);
int sys_dbg_set_address_to_dabr(uint64_t addr, uint64_t ctrl_flag);
int sys_dbg_get_address_from_dabr(uint64_t *addr, uint64_t *ctrl_flag);
int sys_dbg_signal_to_coredump_handler(uint64_t data1,
                                       uint64_t data2,
                                       uint64_t data3);
int sys_dbg_mat_set_condition(sys_addr_t addr, uint64_t cond);
int sys_dbg_mat_get_condition(sys_addr_t addr, uint64_t *condp);
int sys_dbg_get_coredump_params(sys_dbg_coredump_parameter_t *param);
int sys_dbg_set_mask_to_ppu_exception_handler(uint64_t mask, uint64_t flags);

#ifdef __cplusplus
}
#endif

#endif /* __PS3DK_SYS_DBG_H__ */
