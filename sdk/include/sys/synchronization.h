/*! \file sys/synchronization.h
 \brief Sony-SDK-source-compat synchronisation primitives.

  First cut: covers the event-flag surface needed to port Sony's
  reference samples that include <sys/synchronization.h> and use
  sys_event_flag_create / wait / set / clear / destroy / get / cancel /
  trywait / attribute_initialize.

  Mutex / cond / sem / lwmutex / lwcond bits are intentionally NOT
  re-exported here yet — Sony's source uses them under sys_mutex_*,
  sys_cond_*, sys_sem_* names while PSL1GHT exposes them as
  sysMutex* / sysCond* / sysSem* under <sys/mutex.h> / <sys/cond.h> /
  <sys/sem.h>.  Wrappers can be added here as the next sample needs
  them.

  Syscall numbers verified against reference/sony-sdk/.../sys/syscall.h:
    SYS_EVENT_FLAG_CREATE     82
    SYS_EVENT_FLAG_DESTROY    83
    SYS_EVENT_FLAG_WAIT       85
    SYS_EVENT_FLAG_TRYWAIT    86
    SYS_EVENT_FLAG_SET        87
    SYS_EVENT_FLAG_CLEAR     118
    SYS_EVENT_FLAG_CANCEL    132
    SYS_EVENT_FLAG_GET       139
*/

#ifndef __PSL1GHT_SYS_SYNCHRONIZATION_H__
#define __PSL1GHT_SYS_SYNCHRONIZATION_H__

#include <ppu-types.h>
#include <sys/lv2_syscall.h>
#include <sys/return_code.h>
#include <sys/mutex.h>    /* PSL1GHT: sys_mutex_t type + syscalls (100-104) */
#include <sys/cond.h>     /* PSL1GHT: sys_cond_t type + syscalls (105-109)  */

#ifdef __cplusplus
extern "C" {
#endif

/* Values for attr_protocol. */
#define SYS_SYNC_FIFO                  0x00001
#define SYS_SYNC_PRIORITY              0x00002
#define SYS_SYNC_PRIORITY_INHERIT      0x00003
#define SYS_SYNC_RETRY                 0x00004

/* Values for attr_recursive. */
#define SYS_SYNC_RECURSIVE             0x00010
#define SYS_SYNC_NOT_RECURSIVE         0x00020

/* Values for attr_pshared. */
#define SYS_SYNC_NOT_PROCESS_SHARED    0x00200

/* Values for attr_adaptive. */
#define SYS_SYNC_ADAPTIVE              0x01000
#define SYS_SYNC_NOT_ADAPTIVE          0x02000

/* Values for type (event-flag waiter cardinality). */
#define SYS_SYNC_WAITER_SINGLE         0x10000
#define SYS_SYNC_WAITER_MULTIPLE       0x20000

#define SYS_SYNC_NAME_LENGTH           7
#define SYS_SYNC_NAME_SIZE             (SYS_SYNC_NAME_LENGTH + 1)

/* Event-flag wait modes (cellOS Lv-2 semantics). */
#define SYS_EVENT_FLAG_WAIT_AND        0x000001
#define SYS_EVENT_FLAG_WAIT_OR         0x000002
#define SYS_EVENT_FLAG_WAIT_CLEAR      0x000010
#define SYS_EVENT_FLAG_WAIT_CLEAR_ALL  0x000020

#define SYS_EVENT_FLAG_ID_INVALID      0xFFFFFFFFU

/* No-timeout sentinel — passed to wait calls to block forever. */
#define SYS_NO_TIMEOUT                 0ULL

typedef u32 sys_protocol_t;
typedef u32 sys_process_shared_t;
typedef u32 sys_recursive_t;
typedef u32 sys_adaptive_t;
typedef u64 sys_ipc_key_t;

typedef u32 sys_event_flag_t;

/* Layout matches Sony's reference SDK 475.001 sys/synchronization.h
 * exactly: 32 bytes (4 + 4 + 8 + 4 + 4 + 8).  RPCS3's
 * sys_event_flag_create rejects shorter structs with EINVAL — we
 * verified empirically that the missing pshared/key/flags fields
 * caused 0x80010002 returns. */
typedef struct sys_event_flag_attribute {
	sys_protocol_t        attr_protocol;
	sys_process_shared_t  attr_pshared;
	sys_ipc_key_t         key;
	int                   flags;
	int                   type;
	char                  name[SYS_SYNC_NAME_SIZE];
} sys_event_flag_attribute_t;

/* Defaults match Sony's reference SDK macro: priority scheduling,
 * not process-shared, multiple-waiter event flag, no debug name. */
#define sys_event_flag_attribute_initialize(_attr)              \
	do {                                                        \
		(_attr).attr_protocol = SYS_SYNC_PRIORITY;              \
		(_attr).attr_pshared  = SYS_SYNC_NOT_PROCESS_SHARED;    \
		(_attr).key           = 0;                              \
		(_attr).flags         = 0;                              \
		(_attr).type          = SYS_SYNC_WAITER_MULTIPLE;       \
		(_attr).name[0]       = '\0';                           \
	} while (0)

#define sys_event_flag_attribute_name_set(_attr_name, _name)            \
	do {                                                                \
		const char *_n = (_name);                                       \
		int _i;                                                         \
		for (_i = 0; _i < SYS_SYNC_NAME_SIZE - 1 && _n[_i] != '\0'; _i++) \
			(_attr_name)[_i] = _n[_i];                                  \
		(_attr_name)[_i] = '\0';                                        \
	} while (0)

LV2_SYSCALL sys_event_flag_create(sys_event_flag_t *id,
                                  sys_event_flag_attribute_t *attr,
                                  u64 init)
{
	lv2syscall3(82, (u64)id, (u64)attr, init);
	return_to_user_prog(s32);
}

LV2_SYSCALL sys_event_flag_destroy(sys_event_flag_t id)
{
	lv2syscall1(83, id);
	return_to_user_prog(s32);
}

LV2_SYSCALL sys_event_flag_wait(sys_event_flag_t id, u64 bitptn, u32 mode,
                                u64 *result, u64 timeout_usec)
{
	lv2syscall5(85, id, bitptn, mode, (u64)result, timeout_usec);
	return_to_user_prog(s32);
}

LV2_SYSCALL sys_event_flag_trywait(sys_event_flag_t id, u64 bitptn, u32 mode,
                                   u64 *result)
{
	lv2syscall4(86, id, bitptn, mode, (u64)result);
	return_to_user_prog(s32);
}

LV2_SYSCALL sys_event_flag_set(sys_event_flag_t id, u64 bitptn)
{
	lv2syscall2(87, id, bitptn);
	return_to_user_prog(s32);
}

LV2_SYSCALL sys_event_flag_clear(sys_event_flag_t id, u64 bitptn)
{
	lv2syscall2(118, id, bitptn);
	return_to_user_prog(s32);
}

LV2_SYSCALL sys_event_flag_cancel(sys_event_flag_t id, u32 *num)
{
	lv2syscall2(132, id, (u64)num);
	return_to_user_prog(s32);
}

LV2_SYSCALL sys_event_flag_get(sys_event_flag_t id, u64 *flags)
{
	lv2syscall2(139, id, (u64)flags);
	return_to_user_prog(s32);
}

/* ------------------------------------------------------------------ *
 * Mutex - Sony-name surface over PSL1GHT sys_mutex_t + syscalls 100-104.
 * sys_mutex_attribute_t is the Sony spelling; binary-compatible with
 * PSL1GHT's sys_mutex_attr_t (same field order + sizes).
 * ------------------------------------------------------------------ */

typedef struct sys_mutex_attribute {
	sys_protocol_t        attr_protocol;
	sys_recursive_t       attr_recursive;
	sys_process_shared_t  attr_pshared;
	sys_adaptive_t        attr_adaptive;
	sys_ipc_key_t         key;
	int                   flags;
	u32                   pad;
	char                  name[SYS_SYNC_NAME_SIZE];
} sys_mutex_attribute_t;

#define sys_mutex_attribute_initialize(_a)                      \
	do {                                                        \
		(_a).attr_protocol  = SYS_SYNC_PRIORITY;                \
		(_a).attr_recursive = SYS_SYNC_NOT_RECURSIVE;           \
		(_a).attr_pshared   = SYS_SYNC_NOT_PROCESS_SHARED;      \
		(_a).attr_adaptive  = SYS_SYNC_NOT_ADAPTIVE;            \
		(_a).key            = 0;                                \
		(_a).flags          = 0;                                \
		(_a).pad            = 0;                                \
		(_a).name[0]        = '\0';                             \
	} while (0)

LV2_SYSCALL sys_mutex_create(sys_mutex_t *mutex, sys_mutex_attribute_t *attr)
{
	lv2syscall2(100, (u64)mutex, (u64)attr);
	return_to_user_prog(s32);
}

LV2_SYSCALL sys_mutex_destroy(sys_mutex_t mutex)
{
	lv2syscall1(101, mutex);
	return_to_user_prog(s32);
}

LV2_SYSCALL sys_mutex_lock(sys_mutex_t mutex, u64 timeout_usec)
{
	lv2syscall2(102, mutex, timeout_usec);
	return_to_user_prog(s32);
}

LV2_SYSCALL sys_mutex_trylock(sys_mutex_t mutex)
{
	lv2syscall1(103, mutex);
	return_to_user_prog(s32);
}

LV2_SYSCALL sys_mutex_unlock(sys_mutex_t mutex)
{
	lv2syscall1(104, mutex);
	return_to_user_prog(s32);
}

/* ------------------------------------------------------------------ *
 * Condition variable - Sony-name surface over PSL1GHT sys_cond_t +
 * syscalls 105-109.  Layout binary-compatible with PSL1GHT's
 * sys_cond_attr_t.
 * ------------------------------------------------------------------ */

typedef struct sys_cond_attribute {
	sys_process_shared_t  attr_pshared;
	int                   flags;
	sys_ipc_key_t         key;
	char                  name[SYS_SYNC_NAME_SIZE];
} sys_cond_attribute_t;

#define sys_cond_attribute_initialize(_a)                       \
	do {                                                        \
		(_a).attr_pshared = SYS_SYNC_NOT_PROCESS_SHARED;        \
		(_a).flags        = 0;                                  \
		(_a).key          = 0;                                  \
		(_a).name[0]      = '\0';                               \
	} while (0)

LV2_SYSCALL sys_cond_create(sys_cond_t *cond, sys_mutex_t mutex,
                            sys_cond_attribute_t *attr)
{
	lv2syscall3(105, (u64)cond, mutex, (u64)attr);
	return_to_user_prog(s32);
}

LV2_SYSCALL sys_cond_destroy(sys_cond_t cond)
{
	lv2syscall1(106, cond);
	return_to_user_prog(s32);
}

LV2_SYSCALL sys_cond_wait(sys_cond_t cond, u64 timeout_usec)
{
	lv2syscall2(107, cond, timeout_usec);
	return_to_user_prog(s32);
}

LV2_SYSCALL sys_cond_signal(sys_cond_t cond)
{
	lv2syscall1(108, cond);
	return_to_user_prog(s32);
}

LV2_SYSCALL sys_cond_signal_all(sys_cond_t cond)
{
	lv2syscall1(109, cond);
	return_to_user_prog(s32);
}

#ifdef __cplusplus
}
#endif

#endif /* __PSL1GHT_SYS_SYNCHRONIZATION_H__ */
