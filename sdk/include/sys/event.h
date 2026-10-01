/*! \file sys/event.h
 \brief Reference-SDK source-compat umbrella for the LV2 event-queue
        / event-flag / event-port / condition-variable surface.

  Sony's <sys/event.h> bundles the entire LV2 sync syscall surface
  in one header.  Our PSL1GHT-shape SDK splits the same calls across
  <sys/event_queue.h>, <sys/cond.h>, <sys/sem.h>, <sys/mutex.h>, and
  <sys/synchronization.h>; PSL1GHT also names the entry points camelCase
  (sysEventQueueCreate) instead of Sony's snake_case
  (sys_event_queue_create).  This umbrella:

    1. Pulls in the split headers so the entire surface is reachable
       through a single <sys/event.h>.
    2. Aliases the Sony-named struct types (sys_event_queue_attribute_t)
       onto our PSL1GHT-named ones (sys_event_queue_attr_t).
    3. Provides static-inline forwarders for the snake_case Sony names.
    4. Defines the SYS_EVENT_QUEUE_LOCAL / SYS_EVENT_PORT_LOCAL
       enumerator names the reference SDK uses.

  Source written against Sony's monolithic <sys/event.h> compiles
  unchanged against this umbrella.
*/

#ifndef PS3TC_SYS_EVENT_H
#define PS3TC_SYS_EVENT_H

#include <errno.h>
#include <sys/event_queue.h>
#include <sys/cond.h>
#include <sys/sem.h>
#include <sys/mutex.h>
#include <sys/synchronization.h>
#include <sys/process.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- type aliases ------------------------------------------------ */
typedef sys_event_queue_attr_t  sys_event_queue_attribute_t;

/* ---- constants -------------------------------------------------- */
/* The IPC key of a queue private to the process (not a queue type:
 * SYS_PPU_QUEUE / SYS_SPU_QUEUE go in the attribute). */
#ifndef SYS_EVENT_QUEUE_LOCAL
# define SYS_EVENT_QUEUE_LOCAL  0x00
#endif

/* ---- snake_case forwarders -------------------------------------- */
static inline int sys_event_queue_create(sys_event_queue_t *eventQ,
                                         sys_event_queue_attribute_t *attr,
                                         sys_ipc_key_t key, int32_t size)
{
    return (int)sysEventQueueCreate(eventQ, attr, key, (s32)size);
}

static inline int sys_event_queue_destroy(sys_event_queue_t eventQ, int32_t mode)
{
    return (int)sysEventQueueDestroy(eventQ, (s32)mode);
}

static inline int sys_event_queue_receive(sys_event_queue_t eventQ,
                                          sys_event_t *event,
                                          uint64_t timeout_usec)
{
    return (int)sysEventQueueReceive(eventQ, event, timeout_usec);
}

static inline int sys_event_queue_drain(sys_event_queue_t eventQ)
{
    return (int)sysEventQueueDrain(eventQ);
}

static inline int sys_event_port_create(sys_event_port_t *portId,
                                        int portType, uint64_t name)
{
    return (int)sysEventPortCreate(portId, portType, name);
}

static inline int sys_event_port_destroy(sys_event_port_t portId)
{
    return (int)sysEventPortDestroy(portId);
}

static inline int sys_event_port_send(sys_event_port_t portId,
                                      uint64_t data0, uint64_t data1,
                                      uint64_t data2)
{
    return (int)sysEventPortSend(portId, data0, data1, data2);
}

static inline int sys_event_port_connect_local(sys_event_port_t portId,
                                               sys_event_queue_t eventQ)
{
    return (int)sysEventPortConnectLocal(portId, eventQ);
}

static inline int sys_event_port_disconnect(sys_event_port_t portId)
{
    return (int)sysEventPortDisconnect(portId);
}

/* Non-blocking receive: copies up to `size` pending events into
 * event_array and stores how many in *number (syscall 131). */
static inline int sys_event_queue_tryreceive(sys_event_queue_t equeue_id,
                                             sys_event_t *event_array,
                                             int size, int *number)
{
    lv2syscall4(131, equeue_id, (u64)(uintptr_t)event_array, (u64)(s64)size,
                (u64)(uintptr_t)number);
    return_to_user_prog(int);
}

static inline void sys_event_queue_attribute_name_set(char attr_name[],
                                                      const char *name)
{
    __sys_sync_name_set(attr_name, name);
}

/* Syscall numbers and invalid ids. */
#define SYS_EVENT_QUEUE_CREATE          128
#define SYS_EVENT_QUEUE_DESTROY         129
#define SYS_EVENT_QUEUE_RECEIVE         130
#define SYS_EVENT_QUEUE_TRYRECEIVE      131
#define SYS_EVENT_QUEUE_DRAIN           133
#define SYS_EVENT_PORT_CREATE           134
#define SYS_EVENT_PORT_DESTROY          135
#define SYS_EVENT_PORT_CONNECT_LOCAL    136
#define SYS_EVENT_PORT_DISCONNECT       137
#define SYS_EVENT_PORT_SEND             138
#define SYS_EVENT_QUEUE_ID_INVALID      0xFFFFFFFFU
#define SYS_EVENT_PORT_ID_INVALID       0xFFFFFFFFU
/* sys_event_queue_destroy() mode: destroy even with waiting threads. */
#define SYS_EVENT_QUEUE_DESTROY_FORCE   1

/* sys_event_queue_attribute_initialize: macro form matching the
 * reference SDK's <sys/event.h> shape — takes the struct by value
 * and sets default attr_protocol = SYS_SYNC_PRIORITY, type = SYS_PPU_QUEUE,
 * name = "". */
#ifndef SYS_SYNC_PRIORITY
# define SYS_SYNC_PRIORITY  1
#endif
#ifndef SYS_PPU_QUEUE
# define SYS_PPU_QUEUE      SYS_EVENT_QUEUE_PPU
#endif

#define sys_event_queue_attribute_initialize(x)        \
    do {                                               \
        (x).attr_protocol = SYS_SYNC_PRIORITY;         \
        (x).type          = SYS_PPU_QUEUE;             \
        (x).name[0]       = '\0';                      \
    } while (0)

#ifdef __cplusplus
}
#endif

#endif  /* PS3TC_SYS_EVENT_H */
