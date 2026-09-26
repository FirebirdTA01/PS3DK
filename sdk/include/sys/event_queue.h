/* Legacy event queues and ports. Pointer operands retain either PPU ABI width. */
#ifndef __SYS_EVENT_QUEUE_H__
#define __SYS_EVENT_QUEUE_H__
#include <stdint.h>
#include <ppu-lv2.h>

#define SYS_EVENT_QUEUE_PPU 1
#define SYS_EVENT_QUEUE_SPU 2
#define SYS_EVENT_QUEUE_FIFO 1
#define SYS_EVENT_QUEUE_PRIO 2
#define SYS_EVENT_QUEUE_PRIO_INHERIT 3
#define SYS_EVENT_PORT_LOCAL 1
#define SYS_EVENT_PORT_NO_NAME 0
#define SYS_EVENT_QUEUE_KEY_LOCAL 0
#define SYS_EVENT_QUEUE_FORCE_DESTROY 1

#ifdef __cplusplus
extern "C" {
#endif
typedef struct sys_event_queue_attr {
    u32 attr_protocol;
    s32 type;
    char name[8];
} sys_event_queue_attr_t;

typedef struct sys_event {
    u64 source;
    u64 data_1;
    u64 data_2;
    u64 data_3;
} sys_event_t;

LV2_SYSCALL sysEventQueueCreate(sys_event_queue_t *eventQ, sys_event_queue_attr_t *attrib,
                                sys_ipc_key_t key, s32 size)
{
    lv2syscall4(128, (u64)(uintptr_t)eventQ, (u64)(uintptr_t)attrib, key, size);
    return_to_user_prog(s32);
}
LV2_SYSCALL sysEventQueueDestroy(sys_event_queue_t eventQ, s32 mode)
{
    lv2syscall2(129, eventQ, mode);
    return_to_user_prog(s32);
}
LV2_SYSCALL sysEventQueueReceive(sys_event_queue_t eventQ, sys_event_t *event, u64 timeout_usec)
{
    lv2syscall3(130, eventQ, (u64)(uintptr_t)event, timeout_usec);
#ifdef REG_PASS_SYS_EVENT_QUEUE_RECEIVE
    REG_PASS_SYS_EVENT_QUEUE_RECEIVE;
#endif
    return_to_user_prog(s32);
}
LV2_SYSCALL sysEventQueueDrain(sys_event_queue_t eventQ)
{
    lv2syscall1(133, eventQ);
    return_to_user_prog(s32);
}
LV2_SYSCALL sysEventPortCreate(sys_event_port_t *portId, int portType, u64 name)
{
    lv2syscall3(134, (u64)(uintptr_t)portId, portType, name);
    return_to_user_prog(s32);
}
LV2_SYSCALL sysEventPortDestroy(sys_event_port_t portId)
{
    lv2syscall1(135, portId);
    return_to_user_prog(s32);
}
LV2_SYSCALL sysEventPortSend(sys_event_port_t portId, u64 data0, u64 data1, u64 data2)
{
    lv2syscall4(138, portId, data0, data1, data2);
    return_to_user_prog(s32);
}
LV2_SYSCALL sysEventPortConnectLocal(sys_event_port_t portId, sys_event_queue_t eventQ)
{
    lv2syscall2(136, portId, eventQ);
    return_to_user_prog(s32);
}
LV2_SYSCALL sysEventPortDisconnect(sys_event_port_t portId)
{
    lv2syscall1(137, portId);
    return_to_user_prog(s32);
}
#ifdef __cplusplus
}
#endif
#endif
