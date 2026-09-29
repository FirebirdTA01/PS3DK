/*! \file sys/synchronization.h
 \brief Sony-SDK-source-compat synchronisation primitives.

 * Covers event flags, mutexes, condition variables, lightweight mutexes
 * (sys_lwmutex), and lightweight condition variables (sys_lwcond) under
 * their canonical names, matching the liblv2_stub.a exports,
 * while preserving PSL1GHT compatibility aliases.
 *
 * Syscall numbers verified against the PS3 firmware syscall table:
 *   SYS_EVENT_FLAG_CREATE     82
 *   SYS_EVENT_FLAG_DESTROY    83
 *   SYS_EVENT_FLAG_WAIT       85
 *   SYS_EVENT_FLAG_TRYWAIT    86
 *   SYS_EVENT_FLAG_SET        87
 *   SYS_EVENT_FLAG_CLEAR     118
 *   SYS_EVENT_FLAG_CANCEL    132
 *   SYS_EVENT_FLAG_GET       139
 */

#ifndef __PSL1GHT_SYS_SYNCHRONIZATION_H__
#define __PSL1GHT_SYS_SYNCHRONIZATION_H__

#include <stdint.h>
#include <ppu-types.h>
#include <errno.h>
/* lv2 syscalls return the kernel status CELL_EBUSY (0x8001000A), not the libc
 * POSIX EBUSY (16) that <errno.h> defines above.  Restore the lv2 value so
 * source-compatible callers that test a syscall result against EBUSY (e.g. the
 * FW flip handlers queue-full tolerance) compare correctly; other POSIX errno
 * names stay available for libc-style use. */
#undef EBUSY
#define EBUSY (-2147418102) /* 0x8001000A, lv2 CELL_EBUSY */
#include <sys/lv2_syscall.h>
#include <sys/return_code.h>
#include <sys/sys_types.h>
#include <sys/mutex.h>
#include <sys/cond.h>

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
typedef u32 _sys_sleep_queue_t;
typedef u32 _sys_lwcond_queue_t;

typedef u32 sys_event_flag_t;

/* Layout matches the reference SDK sys/synchronization.h
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
	lv2syscall3(82, (u64)(uintptr_t)id, (u64)(uintptr_t)attr, init);
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
	lv2syscall5(85, id, bitptn, mode, (u64)(uintptr_t)result, timeout_usec);
	return_to_user_prog(s32);
}

LV2_SYSCALL sys_event_flag_trywait(sys_event_flag_t id, u64 bitptn, u32 mode,
                                   u64 *result)
{
	lv2syscall4(86, id, bitptn, mode, (u64)(uintptr_t)result);
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
	lv2syscall2(132, id, (u64)(uintptr_t)num);
	return_to_user_prog(s32);
}

LV2_SYSCALL sys_event_flag_get(sys_event_flag_t id, u64 *flags)
{
	lv2syscall2(139, id, (u64)(uintptr_t)flags);
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
	lv2syscall2(100, (u64)(uintptr_t)mutex, (u64)(uintptr_t)attr);
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
	lv2syscall3(105, (u64)(uintptr_t)cond, mutex, (u64)(uintptr_t)attr);
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

LV2_SYSCALL sys_cond_signal_to(sys_cond_t cond, sys_ppu_thread_t thread)
{
	lv2syscall2(110, cond, thread);
	return_to_user_prog(s32);
}

/* ------------------------------------------------------------------ *
 * Syscall numbers, invalid ids and create-disposition values.
 * ------------------------------------------------------------------ */

#define SYS_MUTEX_CREATE            100
#define SYS_MUTEX_DESTROY           101
#define SYS_MUTEX_LOCK              102
#define SYS_MUTEX_TRYLOCK           103
#define SYS_MUTEX_UNLOCK            104
#define SYS_COND_CREATE             105
#define SYS_COND_DESTROY            106
#define SYS_COND_WAIT               107
#define SYS_COND_SIGNAL             108
#define SYS_COND_SIGNAL_ALL         109
#define SYS_COND_SIGNAL_TO          110
#define SYS_SEMAPHORE_CREATE        90
#define SYS_SEMAPHORE_DESTROY       91
#define SYS_SEMAPHORE_WAIT          92
#define SYS_SEMAPHORE_TRYWAIT       93
#define SYS_SEMAPHORE_POST          94
#define SYS_SEMAPHORE_GET_VALUE     114
#define SYS_RWLOCK_CREATE           120
#define SYS_RWLOCK_DESTROY          121
#define SYS_RWLOCK_RLOCK            122
#define SYS_RWLOCK_TRYRLOCK         123
#define SYS_RWLOCK_RUNLOCK          124
#define SYS_RWLOCK_WLOCK            125
#define SYS_RWLOCK_WUNLOCK          127
#define SYS_RWLOCK_TRYWLOCK         148

#define SYS_MUTEX_ID_INVALID        0xFFFFFFFFU
#define SYS_COND_ID_INVALID         0xFFFFFFFFU
#define SYS_SEMAPHORE_ID_INVALID    0xFFFFFFFFU
#define SYS_RWLOCK_ID_INVALID       0xFFFFFFFFU

/* Values for the flags member of a process-shared attribute: create the
   object, attach to an existing one, or either. */
#define SYS_SYNC_NEWLY_CREATED      0x1
#define SYS_SYNC_NOT_CREATE         0x2
#define SYS_SYNC_NOT_CARE           0x3

/* Copy a debugging name into an attribute's name[]: at most
   SYS_SYNC_NAME_LENGTH characters, always NUL-terminated. */
static inline void __sys_sync_name_set(char attr_name[], const char *name)
{
	int i = 0;
	if (name)
		for (; i < SYS_SYNC_NAME_LENGTH && name[i] != '\0'; ++i)
			attr_name[i] = name[i];
	attr_name[i] = '\0';
}
static inline void sys_mutex_attribute_name_set(char attr_name[], const char *name)
{
	__sys_sync_name_set(attr_name, name);
}
static inline void sys_cond_attribute_name_set(char attr_name[], const char *name)
{
	__sys_sync_name_set(attr_name, name);
}

/* ------------------------------------------------------------------ *
 * Reader/writer lock - syscalls 120-127 and 148.
 * ------------------------------------------------------------------ */

#ifndef __SYS_RWLOCK_T_DEFINED
#define __SYS_RWLOCK_T_DEFINED
typedef uint32_t sys_rwlock_t;
#endif

typedef struct sys_rwlock_attribute {
	sys_protocol_t        attr_protocol;
	sys_process_shared_t  attr_pshared;
	sys_ipc_key_t         key;
	int                   flags;
	u32                   pad;
	char                  name[SYS_SYNC_NAME_SIZE];
} sys_rwlock_attribute_t;

#define sys_rwlock_attribute_initialize(_a)                     \
	do {                                                        \
		(_a).attr_protocol = SYS_SYNC_PRIORITY;                 \
		(_a).attr_pshared  = SYS_SYNC_NOT_PROCESS_SHARED;       \
		(_a).key           = 0;                                 \
		(_a).flags         = 0;                                 \
		(_a).name[0]       = '\0';                              \
	} while (0)

static inline void sys_rwlock_attribute_name_set(char attr_name[], const char *name)
{
	__sys_sync_name_set(attr_name, name);
}

LV2_SYSCALL sys_rwlock_create(sys_rwlock_t *rw_lock_id, sys_rwlock_attribute_t *attr)
{
	lv2syscall2(120, (u64)(uintptr_t)rw_lock_id, (u64)(uintptr_t)attr);
	return_to_user_prog(s32);
}

LV2_SYSCALL sys_rwlock_destroy(sys_rwlock_t rw_lock_id)
{
	lv2syscall1(121, rw_lock_id);
	return_to_user_prog(s32);
}

LV2_SYSCALL sys_rwlock_rlock(sys_rwlock_t rw_lock_id, usecond_t timeout)
{
	lv2syscall2(122, rw_lock_id, timeout);
	return_to_user_prog(s32);
}

LV2_SYSCALL sys_rwlock_tryrlock(sys_rwlock_t rw_lock_id)
{
	lv2syscall1(123, rw_lock_id);
	return_to_user_prog(s32);
}

LV2_SYSCALL sys_rwlock_runlock(sys_rwlock_t rw_lock_id)
{
	lv2syscall1(124, rw_lock_id);
	return_to_user_prog(s32);
}

LV2_SYSCALL sys_rwlock_wlock(sys_rwlock_t rw_lock_id, usecond_t timeout)
{
	lv2syscall2(125, rw_lock_id, timeout);
	return_to_user_prog(s32);
}

LV2_SYSCALL sys_rwlock_trywlock(sys_rwlock_t rw_lock_id)
{
	lv2syscall1(148, rw_lock_id);
	return_to_user_prog(s32);
}

LV2_SYSCALL sys_rwlock_wunlock(sys_rwlock_t rw_lock_id)
{
	lv2syscall1(127, rw_lock_id);
	return_to_user_prog(s32);
}

/* ------------------------------------------------------------------ *
 * Counting semaphore - syscalls 90-94 and 114.
 * ------------------------------------------------------------------ */

#ifndef __SYS_SEMAPHORE_T_DEFINED
#define __SYS_SEMAPHORE_T_DEFINED
typedef uint32_t sys_semaphore_t;
#endif

typedef int32_t sys_semaphore_value_t;

typedef struct sys_semaphore_attribute {
	sys_protocol_t        attr_protocol;
	sys_process_shared_t  attr_pshared;
	sys_ipc_key_t         key;
	int                   flags;
	u32                   pad;
	char                  name[SYS_SYNC_NAME_SIZE];
} sys_semaphore_attribute_t;

#define sys_semaphore_attribute_initialize(_a)                  \
	do {                                                        \
		(_a).attr_protocol = SYS_SYNC_PRIORITY;                 \
		(_a).attr_pshared  = SYS_SYNC_NOT_PROCESS_SHARED;       \
		(_a).key           = 0;                                 \
		(_a).flags         = 0;                                 \
		(_a).name[0]       = '\0';                              \
	} while (0)

static inline void sys_semaphore_attribute_name_set(char attr_name[], const char *name)
{
	__sys_sync_name_set(attr_name, name);
}

LV2_SYSCALL sys_semaphore_create(sys_semaphore_t *sem, sys_semaphore_attribute_t *attr,
                                 sys_semaphore_value_t initial_val,
                                 sys_semaphore_value_t max_val)
{
	lv2syscall4(90, (u64)(uintptr_t)sem, (u64)(uintptr_t)attr,
	            (u64)(s64)initial_val, (u64)(s64)max_val);
	return_to_user_prog(s32);
}

LV2_SYSCALL sys_semaphore_destroy(sys_semaphore_t sem)
{
	lv2syscall1(91, sem);
	return_to_user_prog(s32);
}

LV2_SYSCALL sys_semaphore_wait(sys_semaphore_t sem, usecond_t timeout)
{
	lv2syscall2(92, sem, timeout);
	return_to_user_prog(s32);
}

LV2_SYSCALL sys_semaphore_trywait(sys_semaphore_t sem)
{
	lv2syscall1(93, sem);
	return_to_user_prog(s32);
}

LV2_SYSCALL sys_semaphore_post(sys_semaphore_t sem, sys_semaphore_value_t val)
{
	lv2syscall2(94, sem, (u64)(s64)val);
	return_to_user_prog(s32);
}

LV2_SYSCALL sys_semaphore_get_value(sys_semaphore_t sem, sys_semaphore_value_t *val)
{
	lv2syscall2(114, sem, (u64)(uintptr_t)val);
	return_to_user_prog(s32);
}

/* ------------------------------------------------------------------ *
 * Lightweight Mutex (sys_lwmutex) and Condition Variable (sys_lwcond)
 * ------------------------------------------------------------------ */

#define SYS_LWMUTEX_ATTR_PROTOCOL		0x0002
#define SYS_LWMUTEX_ATTR_RECURSIVE		0x0010

#define SYS_LWMUTEX_PROTOCOL_FIFO		1
#define SYS_LWMUTEX_PROTOCOL_PRIO		2
#define SYS_LWMUTEX_PROTOCOL_PRIO_INHERIT	3

#define SYS_LWMUTEX_ATTR_NOT_RECURSIVE		0x0020

typedef struct sys_lwmutex_lock_info {
	uint32_t owner;
	uint32_t waiter;
} sys_lwmutex_lock_info_t;

typedef union sys_lwmutex_variable {
	sys_lwmutex_lock_info_t info;
	uint64_t all_info;
} sys_lwmutex_variable_t;

typedef struct sys_lwmutex {
	sys_lwmutex_variable_t lock_var;
	uint32_t               attribute;
	uint32_t               recursive_count;
	uint32_t               sleep_queue;
	union {
		uint32_t pad;
		uint32_t _pad;
	};
} sys_lwmutex_t;

typedef struct lwmutex_attr {
	sys_protocol_t   attr_protocol;
	sys_recursive_t  attr_recursive;
	char             name[SYS_SYNC_NAME_SIZE];
} sys_lwmutex_attribute_t;

typedef struct lwmutex_attr sys_lwmutex_attribute;
typedef struct lwmutex_attr sys_lwmutex_attr_t;

typedef struct sys_lwcond {
	sys_lwmutex_t       *lwmutex;
	_sys_lwcond_queue_t  lwcond_queue;
} sys_lwcond_t;

typedef struct sys_lwcond_attribute {
	char name[SYS_SYNC_NAME_SIZE];
} sys_lwcond_attribute_t;

typedef struct sys_lwcond_attribute sys_lwcond_attribute;
typedef struct sys_lwcond_attribute sys_lwcond_attr_t;

#define sys_lwmutex_attribute_initialize(_a)                    \
	do {                                                    \
		(_a).attr_protocol  = SYS_SYNC_PRIORITY;        \
		(_a).attr_recursive = SYS_SYNC_NOT_RECURSIVE;   \
		(_a).name[0]        = '\0';                     \
	} while (0)

#define sys_lwmutex_attr_initialize sys_lwmutex_attribute_initialize

static inline void sys_lwmutex_attribute_name_set(char *attr_name, const char *name)
{
	int _i = 0;
	if (name != NULL) {
		for (_i = 0; _i < SYS_SYNC_NAME_SIZE - 1 && name[_i] != '\0'; _i++)
			attr_name[_i] = name[_i];
	}
	attr_name[_i] = '\0';
}

int sys_lwmutex_create(sys_lwmutex_t *mutex_id, sys_lwmutex_attribute_t *attr);
int sys_lwmutex_destroy(sys_lwmutex_t *lwmutex_id);
int sys_lwmutex_lock(sys_lwmutex_t *lwmutex_id, usecond_t timeout);
int sys_lwmutex_trylock(sys_lwmutex_t *lwmutex_id);
int sys_lwmutex_unlock(sys_lwmutex_t *lwmutex_id);

#define sys_lwcond_attribute_initialize(_a)                     \
	do {                                                    \
		(_a).name[0] = '\0';                            \
	} while (0)

#define sys_lwcond_attr_initialize sys_lwcond_attribute_initialize

static inline void sys_lwcond_attribute_name_set(char *attr_name, const char *name)
{
	int _i = 0;
	if (name != NULL) {
		for (_i = 0; _i < SYS_SYNC_NAME_SIZE - 1 && name[_i] != '\0'; _i++)
			attr_name[_i] = name[_i];
	}
	attr_name[_i] = '\0';
}

int sys_lwcond_create(sys_lwcond_t *lwcond, sys_lwmutex_t *lwmutex,
                      sys_lwcond_attribute_t *attr);
int sys_lwcond_destroy(sys_lwcond_t *lwcond);
int sys_lwcond_wait(sys_lwcond_t *lwcond, usecond_t timeout);
int sys_lwcond_signal(sys_lwcond_t *lwcond);
int sys_lwcond_signal_all(sys_lwcond_t *lwcond);
int sys_lwcond_signal_to(sys_lwcond_t *lwcond, sys_ppu_thread_t thr);

/* PSL1GHT camelCase compatibility forwarders */
static inline s32 sysLwMutexCreate(sys_lwmutex_t *mutex, const sys_lwmutex_attr_t *attr)
{
	return (s32)sys_lwmutex_create(mutex, (sys_lwmutex_attribute_t *)attr);
}

static inline s32 sysLwMutexDestroy(sys_lwmutex_t *mutex)
{
	return (s32)sys_lwmutex_destroy(mutex);
}

static inline s32 sysLwMutexLock(sys_lwmutex_t *mutex, u64 timeout)
{
	return (s32)sys_lwmutex_lock(mutex, (usecond_t)timeout);
}

static inline s32 sysLwMutexTryLock(sys_lwmutex_t *mutex)
{
	return (s32)sys_lwmutex_trylock(mutex);
}

static inline s32 sysLwMutexUnlock(sys_lwmutex_t *mutex)
{
	return (s32)sys_lwmutex_unlock(mutex);
}

static inline s32 sysLwCondCreate(sys_lwcond_t *cond, sys_lwmutex_t *mutex, const sys_lwcond_attr_t *attr)
{
	return (s32)sys_lwcond_create(cond, mutex, (sys_lwcond_attribute_t *)attr);
}

static inline s32 sysLwCondDestroy(sys_lwcond_t *cond)
{
	return (s32)sys_lwcond_destroy(cond);
}

static inline s32 sysLwCondWait(sys_lwcond_t *cond, u64 timeout)
{
	return (s32)sys_lwcond_wait(cond, (usecond_t)timeout);
}

static inline s32 sysLwCondSignal(sys_lwcond_t *cond)
{
	return (s32)sys_lwcond_signal(cond);
}

static inline s32 sysLwCondSignalAll(sys_lwcond_t *cond)
{
	return (s32)sys_lwcond_signal_all(cond);
}

static inline s32 sysLwCondSignalTo(sys_lwcond_t *cond, sys_ppu_thread_t thr)
{
	return (s32)sys_lwcond_signal_to(cond, thr);
}

#ifdef __cplusplus
}
#endif

#endif /* __PSL1GHT_SYS_SYNCHRONIZATION_H__ */
