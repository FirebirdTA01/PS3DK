/* sys/raw_spu.h - Raw SPU management: the LV2 raw SPU calls under their
 * reference names, and the addresses of a raw SPU's local store and
 * problem-state registers.
 *
 * A raw SPU is mapped at RAW_SPU_BASE_ADDR + RAW_SPU_OFFSET * id: its local
 * store at offset 0, its problem-state registers at RAW_SPU_PROB_OFFSET.
 * The system calls themselves are the ones <sys/spu.h> already wraps
 * (sysSpuRaw*); the functions below forward to them. */
#ifndef PS3TC_SYS_RAW_SPU_H
#define PS3TC_SYS_RAW_SPU_H

#include <stdint.h>
#include <sys/fixed_addr.h>
#include <sys/spu.h>
#include <sys/spu_utility.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef uint32_t sys_class_id_t;
typedef void sys_raw_spu_attribute_t;

#define RAW_SPU_OFFSET        0x00100000UL
#define RAW_SPU_LS_OFFSET     0x00000000UL
#define RAW_SPU_PROB_OFFSET   0x00040000UL

#define LS_BASE_ADDR(id)   (RAW_SPU_OFFSET * (id) + RAW_SPU_BASE_ADDR + RAW_SPU_LS_OFFSET)
#define PROB_BASE_ADDR(id) (RAW_SPU_OFFSET * (id) + RAW_SPU_BASE_ADDR + RAW_SPU_PROB_OFFSET)

/* Problem-state register offsets (from PROB_BASE_ADDR; the signal
 * notification registers lie beyond the first page).  <sys/spu.h> spells
 * some of them the same way. */
#ifndef MFC_LSA
#define MFC_LSA           0x3004U
#endif
#ifndef MFC_EAH
#define MFC_EAH           0x3008U
#endif
#ifndef MFC_EAL
#define MFC_EAL           0x300CU
#endif
#ifndef MFC_Size_Tag
#define MFC_Size_Tag      0x3010U
#endif
#ifndef MFC_Class_CMD
#define MFC_Class_CMD     0x3014U
#endif
#ifndef MFC_CMDStatus
#define MFC_CMDStatus     0x3014U
#endif
#ifndef MFC_QStatus
#define MFC_QStatus       0x3104U
#endif
#ifndef Prxy_QueryType
#define Prxy_QueryType    0x3204U
#endif
#ifndef Prxy_QueryMask
#define Prxy_QueryMask    0x321CU
#endif
#ifndef Prxy_TagStatus
#define Prxy_TagStatus    0x322CU
#endif
#ifndef SPU_Out_MBox
#define SPU_Out_MBox      0x4004U
#endif
#ifndef SPU_In_MBox
#define SPU_In_MBox       0x400CU
#endif
#ifndef SPU_MBox_Status
#define SPU_MBox_Status   0x4014U
#endif
#ifndef SPU_RunCntl
#define SPU_RunCntl       0x401CU
#endif
#ifndef SPU_Status
#define SPU_Status        0x4024U
#endif
#ifndef SPU_NPC
#define SPU_NPC           0x4034U
#endif
#ifndef SPU_Sig_Notify_1
#define SPU_Sig_Notify_1  0x1400CU
#endif
#ifndef SPU_Sig_Notify_2
#define SPU_Sig_Notify_2  0x1C00CU
#endif

/* Effective address of a problem-state register / a local store byte. */
#define get_reg_addr(id, offset) ((uintptr_t)(PROB_BASE_ADDR(id) + (offset)))
#define get_ls_addr(id, offset)  ((uintptr_t)(LS_BASE_ADDR(id) + (offset)))

static inline void sys_raw_spu_mmio_write(int id, int offset, uint32_t value)
{
	*(volatile uint32_t *)get_reg_addr(id, offset) = value;
}

static inline uint32_t sys_raw_spu_mmio_read(int id, int offset)
{
	return *(volatile uint32_t *)get_reg_addr(id, offset);
}

static inline void sys_raw_spu_mmio_write_ls(int id, int offset, uint32_t value)
{
	*(volatile uint32_t *)get_ls_addr(id, offset) = value;
}

static inline uint32_t sys_raw_spu_mmio_read_ls(int id, int offset)
{
	return *(volatile uint32_t *)get_ls_addr(id, offset);
}

static inline int sys_raw_spu_create(sys_raw_spu_t *id, sys_raw_spu_attribute_t *attr)
{
	return sysSpuRawCreate(id, (u32 *)attr);
}

static inline int sys_raw_spu_destroy(sys_raw_spu_t id)
{
	return sysSpuRawDestroy(id);
}

static inline int sys_raw_spu_create_interrupt_tag(sys_raw_spu_t id, sys_class_id_t class_id,
                                                   sys_hw_thread_t hwthread, sys_interrupt_tag_t *intrtag)
{
	return sysSpuRawCreateInterrupTag(id, class_id, hwthread, intrtag);
}

static inline int sys_raw_spu_set_int_mask(sys_raw_spu_t id, sys_class_id_t class_id, uint64_t mask)
{
	return sysSpuRawSetIntMask(id, class_id, mask);
}

static inline int sys_raw_spu_get_int_mask(sys_raw_spu_t id, sys_class_id_t class_id, uint64_t *mask)
{
	return sysSpuRawGetIntMask(id, class_id, mask);
}

static inline int sys_raw_spu_set_int_stat(sys_raw_spu_t id, sys_class_id_t class_id, uint64_t stat)
{
	return sysSpuRawSetIntStat(id, class_id, stat);
}

static inline int sys_raw_spu_get_int_stat(sys_raw_spu_t id, sys_class_id_t class_id, uint64_t *stat)
{
	return sysSpuRawGetIntStat(id, class_id, stat);
}

static inline int sys_raw_spu_read_puint_mb(sys_raw_spu_t id, uint32_t *value)
{
	return sysSpuRawReadPuintMb(id, value);
}

static inline int sys_raw_spu_set_spu_cfg(sys_raw_spu_t id, uint32_t value)
{
	return sysSpuRawSetConfiguration(id, value);
}

static inline int sys_raw_spu_get_spu_cfg(sys_raw_spu_t id, uint32_t *value)
{
	return sysSpuRawGetConfirugation(id, value);
}

static inline int sys_raw_spu_recover_page_fault(sys_raw_spu_t id)
{
	return sysSpuRawRecoverPageFault(id);
}

#ifdef __cplusplus
}
#endif

#endif /* PS3TC_SYS_RAW_SPU_H */
