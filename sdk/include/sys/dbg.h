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
/* the object attribute records the information calls report */
#include <sys/event.h>
/* SYS_PROCESS_PARAM: a debugger program declares its own process
 * parameters (thread priority and stack size) with this macro. */
#include <sys/process.h>

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
#ifndef SYS_DBG_SPU_MB_NUM
#define SYS_DBG_SPU_MB_NUM 4
#endif
#ifndef SYS_DBG_SPU_MFC_CQ_SR_NUM
#define SYS_DBG_SPU_MFC_CQ_SR_NUM 96
#endif

#ifndef __SYS_RWLOCK_T_DEFINED
#define __SYS_RWLOCK_T_DEFINED
typedef uint32_t sys_rwlock_t;
#endif
#ifndef __SYS_SEMAPHORE_T_DEFINED
#define __SYS_SEMAPHORE_T_DEFINED
typedef uint32_t sys_semaphore_t;   /* the same type as sys_sem_t */
#endif
typedef uint32_t sys_lwmutex_pseudo_id_t;
typedef uint32_t sys_lwcond_pseudo_id_t;

/* 128-bit register image, viewable at any element width. */
typedef union sys_dbg_vr {
    uint8_t  byte[16];
    uint16_t halfword[8];
    uint32_t word[4];
    uint64_t dw[2];
} sys_dbg_vr_t;

/* PPU registers as lv2 reports them: fpscr follows the 32 FPRs and is a
 * 32-bit field; cr is 32 bits wide. */
typedef struct sys_dbg_ppu_thread_context {
    uint64_t gpr[SYS_DBG_PPU_GPR_NUM];
    uint32_t cr;
    uint64_t xer;
    uint64_t lr;
    uint64_t ctr;
    uint64_t pc;
    uint64_t fpr[SYS_DBG_PPU_FPR_NUM];
    uint32_t fpscr;
    sys_dbg_vr_t vr[SYS_DBG_PPU_VR_NUM];
    sys_dbg_vr_t vscr;
} sys_dbg_ppu_thread_context_t;

typedef union sys_dbg_spu_gpr {
    uint8_t  byte[16];
    uint16_t halfword[8];
    uint32_t word[4];
    uint64_t dw[2];
} sys_dbg_spu_gpr_t;

typedef union sys_dbg_spu_fpscr {
    uint8_t  byte[16];
    uint16_t halfword[8];
    uint32_t word[4];
    uint64_t dw[2];
} sys_dbg_spu_fpscr_t;

/* SPU thread state.  The first form reports FPSCR as one word; the second
 * reports the full 128-bit FPSCR. */
typedef struct sys_dbg_spu_thread_context {
    sys_dbg_spu_gpr_t gpr[SYS_DBG_SPU_GPR_NUM];
    uint32_t npc;
    uint32_t fpscr;
    uint32_t srr0;
    uint32_t spu_status;
    uint64_t spu_cfg;
    uint32_t mb_stat;
    uint32_t ppu_mb;
    uint32_t spu_mb[SYS_DBG_SPU_MB_NUM];
    uint32_t decrementer;
    uint64_t mfc_cq_sr[SYS_DBG_SPU_MFC_CQ_SR_NUM];
} sys_dbg_spu_thread_context_t;

typedef struct sys_dbg_spu_thread_context2 {
    sys_dbg_spu_gpr_t gpr[SYS_DBG_SPU_GPR_NUM];
    uint32_t npc;
    sys_dbg_spu_fpscr_t fpscr;
    uint32_t srr0;
    uint32_t spu_status;
    uint64_t spu_cfg;
    uint32_t mb_stat;
    uint32_t ppu_mb;
    uint32_t spu_mb[SYS_DBG_SPU_MB_NUM];
    uint32_t decrementer;
    uint64_t mfc_cq_sr[SYS_DBG_SPU_MFC_CQ_SR_NUM];
} sys_dbg_spu_thread_context2_t;

/* Object information records.  Each starts with the object's creation
 * attributes.  The caller supplies the list buffers (wait_id_list and the
 * like, 32-bit addresses in both data models) and their capacity in the
 * *_num fields; lv2 fills them and reports the total in *_all_*_num. */
typedef struct sys_dbg_mutex_information {
    sys_mutex_attribute_t attr;
    sys_ppu_thread_t owner;
    int lock_counter;
    int cond_ref_counter;
    sys_cond_t cond_id;
    sys_ppu_thread_t *wait_id_list ATTRIBUTE_PRXPTR;
    uint32_t wait_threads_num;
    uint32_t wait_all_threads_num;
} sys_dbg_mutex_information_t;

typedef struct sys_dbg_cond_information {
    sys_cond_attribute_t attr;
    sys_mutex_t mutex_id;
    sys_ppu_thread_t *wait_id_list ATTRIBUTE_PRXPTR;
    uint32_t wait_threads_num;
    uint32_t wait_all_threads_num;
} sys_dbg_cond_information_t;

typedef struct sys_dbg_rwlock_information {
    sys_rwlock_attribute_t attr;
    sys_ppu_thread_t owner;
    sys_ppu_thread_t *r_wait_id_list ATTRIBUTE_PRXPTR;
    uint32_t r_wait_threads_num;
    uint32_t r_wait_all_threads_num;
    sys_ppu_thread_t *w_wait_id_list ATTRIBUTE_PRXPTR;
    uint32_t w_wait_threads_num;
    uint32_t w_wait_all_threads_num;
} sys_dbg_rwlock_information_t;

typedef struct sys_dbg_event_queue_information {
    sys_event_queue_attribute_t attr;
    sys_ipc_key_t event_queue_key;
    int queue_size;
    sys_ppu_thread_t *wait_id_list ATTRIBUTE_PRXPTR;
    uint32_t wait_threads_num;
    uint32_t wait_all_threads_num;
    sys_event_t *equeue_list ATTRIBUTE_PRXPTR;
    uint32_t readable_equeue_num;
    uint32_t readable_all_equeue_num;
} sys_dbg_event_queue_information_t;

typedef struct sys_dbg_semaphore_information {
    sys_semaphore_attribute_t attr;
    sys_semaphore_value_t max_val;
    sys_semaphore_value_t cur_val;
    sys_ppu_thread_t *wait_id_list ATTRIBUTE_PRXPTR;
    uint32_t wait_threads_num;
    uint32_t wait_all_threads_num;
} sys_dbg_semaphore_information_t;

typedef struct sys_dbg_lwmutex_information {
    sys_lwmutex_attribute_t attr;
    sys_ppu_thread_t owner;
    int lock_counter;
    sys_ppu_thread_t *wait_id_list ATTRIBUTE_PRXPTR;
    uint32_t wait_threads_num;
    uint32_t wait_all_threads_num;
} sys_dbg_lwmutex_information_t;

typedef struct sys_dbg_lwcond_information {
    sys_lwcond_attribute_t attr;
    sys_lwmutex_t *lwmutex ATTRIBUTE_PRXPTR;
    sys_ppu_thread_t *wait_id_list ATTRIBUTE_PRXPTR;
    uint32_t wait_threads_num;
    uint32_t wait_all_threads_num;
} sys_dbg_lwcond_information_t;

typedef struct sys_dbg_event_flag_wait_information {
    uint64_t bitptn;
    uint32_t mode;
} sys_dbg_event_flag_wait_information_t;

typedef struct sys_dbg_event_flag_information {
    sys_event_flag_attribute_t attr;
    uint64_t cur_bitptn;
    sys_ppu_thread_t *wait_id_list ATTRIBUTE_PRXPTR;
    sys_dbg_event_flag_wait_information_t *wait_info_list ATTRIBUTE_PRXPTR;
    uint32_t wait_threads_num;
    uint32_t wait_all_threads_num;
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
