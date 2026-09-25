/*
 * PS3 Custom Toolchain — <sys/event_queue.h>
 *
 * Event queue and event port management Lv2 syscall interface.
 * Implements canonical reference-SDK prototypes, types and constants,
 * with anonymous unions in sys_event_t for transparent compatibility with
 * both reference SDK (.data1, .data2, .data3) and PSL1GHT (.data_1, .data_2, .data_3).
 */

#ifndef __SYS_EVENT_QUEUE_H__
#define __SYS_EVENT_QUEUE_H__

#include <stdint.h>
#include <ppu-types.h>
#include <ppu-lv2.h>
#include <sys/lv2_syscall.h>
#include <sys/return_code.h>

/* Event queue types */
#define SYS_EVENT_QUEUE_PPU             0x01
#define SYS_EVENT_QUEUE_SPU             0x02
#define SYS_PPU_QUEUE                   0x01
#define SYS_SPU_QUEUE                   0x02

/* Synchronize event queue policies */
#define SYS_EVENT_QUEUE_FIFO            0x01
#define SYS_EVENT_QUEUE_PRIO            0x02
#define SYS_EVENT_QUEUE_PRIO_INHERIT    0x03
#ifndef SYS_SYNC_FIFO
#define SYS_SYNC_FIFO                   0x00001
#endif
#ifndef SYS_SYNC_PRIORITY
#define SYS_SYNC_PRIORITY               0x00002
#endif
#ifndef SYS_SYNC_PRIORITY_INHERIT
#define SYS_SYNC_PRIORITY_INHERIT       0x00003
#endif

/* Event port type and flags */
#define SYS_EVENT_PORT_LOCAL            0x01
#define SYS_EVENT_PORT_NO_NAME          0x00
#define SYS_EVENT_QUEUE_LOCAL           0x00
#define SYS_EVENT_QUEUE_KEY_LOCAL       0x00
#define SYS_EVENT_QUEUE_FORCE_DESTROY   0x01
#define SYS_EVENT_QUEUE_DESTROY_FORCE   0x01

#ifdef __cplusplus
extern "C" {
#endif

/* Event queue attributes */
typedef struct sys_event_queue_attr {
    uint32_t attr_protocol;
    int32_t type;
    char name[8];
} sys_event_queue_attr_t;

typedef sys_event_queue_attr_t sys_event_queue_attribute_t;

/* Received event data structure with anonymous union compatibility */
typedef struct sys_event {
    uint64_t source;
    union {
        uint64_t data1;
        uint64_t data_1;
    };
    union {
        uint64_t data2;
        uint64_t data_2;
    };
    union {
        uint64_t data3;
        uint64_t data_3;
    };
} sys_event_t;

/* ---- Canonical Lv2 syscall inline implementations ---------------- */

static inline int sys_event_queue_create(sys_event_queue_t *eventQ,
                                         sys_event_queue_attribute_t *attr,
                                         sys_ipc_key_t key,
                                         int32_t size)
{
    lv2syscall4(128, (uint64_t)(uintptr_t)eventQ, (uint64_t)(uintptr_t)attr, (uint64_t)key, (uint64_t)size);
    return_to_user_prog(int);
}

static inline int sys_event_queue_destroy(sys_event_queue_t eventQ, int32_t mode)
{
    lv2syscall2(129, (uint64_t)eventQ, (uint64_t)mode);
    return_to_user_prog(int);
}

static inline int sys_event_queue_receive(sys_event_queue_t eventQ,
                                          sys_event_t *event,
                                          uint64_t timeout_usec)
{
    lv2syscall3(130, (uint64_t)eventQ, (uint64_t)(uintptr_t)event, timeout_usec);
#ifdef REG_PASS_SYS_EVENT_QUEUE_RECEIVE
    REG_PASS_SYS_EVENT_QUEUE_RECEIVE;
#endif
    return_to_user_prog(int);
}

static inline int sys_event_queue_drain(sys_event_queue_t eventQ)
{
    lv2syscall1(133, (uint64_t)eventQ);
    return_to_user_prog(int);
}

static inline int sys_event_port_create(sys_event_port_t *portId,
                                        int portType,
                                        uint64_t name)
{
    lv2syscall3(134, (uint64_t)(uintptr_t)portId, (uint64_t)portType, name);
    return_to_user_prog(int);
}

static inline int sys_event_port_destroy(sys_event_port_t portId)
{
    lv2syscall1(135, (uint64_t)portId);
    return_to_user_prog(int);
}

static inline int sys_event_port_send(sys_event_port_t portId,
                                      uint64_t data0,
                                      uint64_t data1,
                                      uint64_t data2)
{
    lv2syscall4(138, (uint64_t)portId, data0, data1, data2);
    return_to_user_prog(int);
}

static inline int sys_event_port_connect_local(sys_event_port_t portId,
                                               sys_event_queue_t eventQ)
{
    lv2syscall2(136, (uint64_t)portId, (uint64_t)eventQ);
    return_to_user_prog(int);
}

static inline int sys_event_port_disconnect(sys_event_port_t portId)
{
    lv2syscall1(137, (uint64_t)portId);
    return_to_user_prog(int);
}

/* ---- PSL1GHT camelCase compatibility forwarders ------------------ */

static inline int sysEventQueueCreate(sys_event_queue_t *eventQ,
                                      sys_event_queue_attr_t *attrib,
                                      sys_ipc_key_t key,
                                      int32_t size)
{
    return sys_event_queue_create(eventQ, attrib, key, size);
}

static inline int sysEventQueueDestroy(sys_event_queue_t eventQ, int32_t mode)
{
    return sys_event_queue_destroy(eventQ, mode);
}

static inline int sysEventQueueReceive(sys_event_queue_t eventQ,
                                       sys_event_t *event,
                                       uint64_t timeout_usec)
{
    return sys_event_queue_receive(eventQ, event, timeout_usec);
}

static inline int sysEventQueueDrain(sys_event_queue_t eventQ)
{
    return sys_event_queue_drain(eventQ);
}

static inline int sysEventPortCreate(sys_event_port_t *portId, int portType, uint64_t name)
{
    return sys_event_port_create(portId, portType, name);
}

static inline int sysEventPortDestroy(sys_event_port_t portId)
{
    return sys_event_port_destroy(portId);
}

static inline int sysEventPortSend(sys_event_port_t portId, uint64_t data0, uint64_t data1, uint64_t data2)
{
    return sys_event_port_send(portId, data0, data1, data2);
}

static inline int sysEventPortConnectLocal(sys_event_port_t portId, sys_event_queue_t eventQ)
{
    return sys_event_port_connect_local(portId, eventQ);
}

static inline int sysEventPortDisconnect(sys_event_port_t portId)
{
    return sys_event_port_disconnect(portId);
}

#ifdef __cplusplus
}
#endif

#endif /* __SYS_EVENT_QUEUE_H__ */
