/*
 * The canonical sys_ synchronisation surface in <sys/synchronization.h> and
 * <sys/event.h>: reader/writer locks, counting semaphores, cond signal-to,
 * event-queue try-receive, the attribute name setters, and the syscall-number,
 * invalid-id and create-disposition constants.  Layouts and values are the
 * firmware ABI's; prototypes are pinned through typed function pointers, so a
 * changed parameter or return type fails to compile.  <sys/dbg.h> declares
 * sys_rwlock_t and sys_semaphore_t too and must coexist in either order.
 */
#ifdef DBG_FIRST
#include <sys/dbg.h>
#endif
#include <stddef.h>
#include <sys/synchronization.h>
#include <sys/event.h>
#ifndef DBG_FIRST
#include <sys/dbg.h>
#endif

#ifdef __cplusplus
#define SA(c, m) static_assert(c, m)
#else
#define SA(c, m) _Static_assert(c, m)
#endif

/* attribute layouts (identical in ILP32 and LP64) */
SA(sizeof(sys_rwlock_attribute_t) == 32, "rwlock attribute size");
SA(offsetof(sys_rwlock_attribute_t, attr_protocol) == 0, "rwlock protocol");
SA(offsetof(sys_rwlock_attribute_t, attr_pshared) == 4, "rwlock pshared");
SA(offsetof(sys_rwlock_attribute_t, key) == 8, "rwlock key");
SA(offsetof(sys_rwlock_attribute_t, flags) == 16, "rwlock flags");
SA(offsetof(sys_rwlock_attribute_t, name) == 24, "rwlock name");
SA(sizeof(sys_semaphore_attribute_t) == 32, "semaphore attribute size");
SA(offsetof(sys_semaphore_attribute_t, attr_protocol) == 0, "semaphore protocol");
SA(offsetof(sys_semaphore_attribute_t, attr_pshared) == 4, "semaphore pshared");
SA(offsetof(sys_semaphore_attribute_t, key) == 8, "semaphore key");
SA(offsetof(sys_semaphore_attribute_t, flags) == 16, "semaphore flags");
SA(offsetof(sys_semaphore_attribute_t, name) == 24, "semaphore name");
SA(sizeof(sys_mutex_attribute_t) == 40, "mutex attribute size");
SA(sizeof(sys_cond_attribute_t) == 24, "cond attribute size");
SA(sizeof(sys_rwlock_t) == 4 && sizeof(sys_semaphore_t) == 4, "object ids are 32-bit");
SA(sizeof(sys_semaphore_value_t) == 4 && (sys_semaphore_value_t)-1 < 0, "semaphore value is int32");

/* event record: canonical data1..data3 with the PSL1GHT data_1..data_3 aliases */
SA(sizeof(sys_event_t) == 32, "event size");
SA(offsetof(sys_event_t, source) == 0 && offsetof(sys_event_t, data1) == 8
   && offsetof(sys_event_t, data2) == 16 && offsetof(sys_event_t, data3) == 24, "event layout");
SA(offsetof(sys_event_t, data_1) == 8 && offsetof(sys_event_t, data_2) == 16
   && offsetof(sys_event_t, data_3) == 24, "event PSL1GHT aliases");

/* constants */
SA(SYS_MUTEX_CREATE == 100 && SYS_MUTEX_DESTROY == 101 && SYS_MUTEX_LOCK == 102
   && SYS_MUTEX_TRYLOCK == 103 && SYS_MUTEX_UNLOCK == 104, "mutex syscalls");
SA(SYS_COND_CREATE == 105 && SYS_COND_DESTROY == 106 && SYS_COND_WAIT == 107
   && SYS_COND_SIGNAL == 108 && SYS_COND_SIGNAL_ALL == 109 && SYS_COND_SIGNAL_TO == 110, "cond syscalls");
SA(SYS_SEMAPHORE_CREATE == 90 && SYS_SEMAPHORE_DESTROY == 91 && SYS_SEMAPHORE_WAIT == 92
   && SYS_SEMAPHORE_TRYWAIT == 93 && SYS_SEMAPHORE_POST == 94 && SYS_SEMAPHORE_GET_VALUE == 114, "semaphore syscalls");
SA(SYS_RWLOCK_CREATE == 120 && SYS_RWLOCK_DESTROY == 121 && SYS_RWLOCK_RLOCK == 122
   && SYS_RWLOCK_TRYRLOCK == 123 && SYS_RWLOCK_RUNLOCK == 124 && SYS_RWLOCK_WLOCK == 125
   && SYS_RWLOCK_WUNLOCK == 127 && SYS_RWLOCK_TRYWLOCK == 148, "rwlock syscalls");
SA(SYS_EVENT_QUEUE_CREATE == 128 && SYS_EVENT_QUEUE_DESTROY == 129 && SYS_EVENT_QUEUE_RECEIVE == 130
   && SYS_EVENT_QUEUE_TRYRECEIVE == 131 && SYS_EVENT_QUEUE_DRAIN == 133, "event queue syscalls");
SA(SYS_EVENT_PORT_CREATE == 134 && SYS_EVENT_PORT_DESTROY == 135 && SYS_EVENT_PORT_CONNECT_LOCAL == 136
   && SYS_EVENT_PORT_DISCONNECT == 137 && SYS_EVENT_PORT_SEND == 138, "event port syscalls");
SA(SYS_MUTEX_ID_INVALID == 0xFFFFFFFFu && SYS_COND_ID_INVALID == 0xFFFFFFFFu
   && SYS_SEMAPHORE_ID_INVALID == 0xFFFFFFFFu && SYS_RWLOCK_ID_INVALID == 0xFFFFFFFFu
   && SYS_EVENT_QUEUE_ID_INVALID == 0xFFFFFFFFu && SYS_EVENT_PORT_ID_INVALID == 0xFFFFFFFFu, "invalid ids");
SA(SYS_SYNC_NEWLY_CREATED == 1 && SYS_SYNC_NOT_CREATE == 2 && SYS_SYNC_NOT_CARE == 3, "create disposition");
SA(SYS_EVENT_QUEUE_DESTROY_FORCE == 1, "destroy force");

/* prototypes */
int (*const p_rw_create)(sys_rwlock_t *, sys_rwlock_attribute_t *) = sys_rwlock_create;
int (*const p_rw_destroy)(sys_rwlock_t) = sys_rwlock_destroy;
int (*const p_rw_rlock)(sys_rwlock_t, usecond_t) = sys_rwlock_rlock;
int (*const p_rw_tryrlock)(sys_rwlock_t) = sys_rwlock_tryrlock;
int (*const p_rw_runlock)(sys_rwlock_t) = sys_rwlock_runlock;
int (*const p_rw_wlock)(sys_rwlock_t, usecond_t) = sys_rwlock_wlock;
int (*const p_rw_trywlock)(sys_rwlock_t) = sys_rwlock_trywlock;
int (*const p_rw_wunlock)(sys_rwlock_t) = sys_rwlock_wunlock;
int (*const p_sem_create)(sys_semaphore_t *, sys_semaphore_attribute_t *,
                          sys_semaphore_value_t, sys_semaphore_value_t) = sys_semaphore_create;
int (*const p_sem_destroy)(sys_semaphore_t) = sys_semaphore_destroy;
int (*const p_sem_wait)(sys_semaphore_t, usecond_t) = sys_semaphore_wait;
int (*const p_sem_trywait)(sys_semaphore_t) = sys_semaphore_trywait;
int (*const p_sem_post)(sys_semaphore_t, sys_semaphore_value_t) = sys_semaphore_post;
int (*const p_sem_get)(sys_semaphore_t, sys_semaphore_value_t *) = sys_semaphore_get_value;
int (*const p_cond_to)(sys_cond_t, sys_ppu_thread_t) = sys_cond_signal_to;
int (*const p_eq_try)(sys_event_queue_t, sys_event_t *, int, int *) = sys_event_queue_tryreceive;
void (*const p_names[])(char[], const char *) = {
    sys_mutex_attribute_name_set, sys_cond_attribute_name_set, sys_rwlock_attribute_name_set,
    sys_semaphore_attribute_name_set, sys_event_queue_attribute_name_set };

/* initialisers and the name setter's truncation */
int probe(void)
{
    sys_rwlock_attribute_t rw;
    sys_semaphore_attribute_t sem;
    sys_rwlock_attribute_initialize(rw);
    sys_semaphore_attribute_initialize(sem);
    sys_rwlock_attribute_name_set(rw.name, "longer-than-seven");
    sys_semaphore_attribute_name_set(sem.name, "sem");
    sys_event_t ev;
    ev.source = 1; ev.data1 = 2; ev.data_2 = 5; ev.data3 = 4;   /* either spelling, same storage */
    if (ev.data1 != 2 || ev.data2 != 5 || ev.data3 != 4)
        return 0;
    return rw.attr_protocol == SYS_SYNC_PRIORITY && sem.attr_pshared == SYS_SYNC_NOT_PROCESS_SHARED
        && rw.name[7] == '\0' && rw.name[6] == 'n' && sem.name[3] == '\0';
}
