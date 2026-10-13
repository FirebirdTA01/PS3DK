/*
 * PS3 Custom Toolchain — <sys/timer.h> LV2 timers.
 *
 * Sleeps: sys_timer_usleep / sys_timer_sleep (syscalls 141 / 142).
 *
 * Kernel timers (syscalls 70-76): a timer is created stopped, connected to
 * an event queue, then started.  Each expiry sends one event to the queue:
 * source = the `name` given to sys_timer_connect_event_queue, data1 / data2
 * as given there, data3 = the expiry time.  hello-ppu-lv2-timer uses the
 * order create, connect, start, stop, disconnect, destroy.
 *
 *   _sys_timer_start(id, base_time, period)
 *     period == 0                 one expiry at the absolute time base_time
 *     base_time == 0, period > 0  every `period` microseconds from now
 *     both non-zero               first at base_time, then every `period`
 *
 * The three sys_timer_start_* helpers name those three cases.  Times are
 * system time in microseconds (sys_time_get_system_time()).
 */

#ifndef PS3TC_COMPAT_SYS_TIMER_H
#define PS3TC_COMPAT_SYS_TIMER_H

#include <stdint.h>
#include <ppu-types.h>
#include <sys/lv2_syscall.h>
#include <sys/sys_types.h>

#ifndef SYS_TIMER_USLEEP
#define SYS_TIMER_USLEEP 141
#endif

#ifndef SYS_TIMER_SLEEP
#define SYS_TIMER_SLEEP 142
#endif

#define SYS_TIMER_CREATE                   70
#define SYS_TIMER_DESTROY                  71
#define SYS_TIMER_GET_INFORMATION          72
#define SYS_TIMER_START                    73
#define SYS_TIMER_STOP                     74
#define SYS_TIMER_CONNECT_EVENT_QUEUE      75
#define SYS_TIMER_DISCONNECT_EVENT_QUEUE   76

#define SYS_TIMER_ID_INVALID     0xFFFFFFFFU

#define SYS_TIMER_STATE_STOP     0x00U
#define SYS_TIMER_STATE_RUN      0x01U

#define SYS_TIMER_EVENT_NO_NAME  0x00U

typedef uint32_t sys_timer_t;

typedef struct {
    system_time_t next_expiration_time;
    usecond_t     period;
    uint32_t      timer_state;   /* SYS_TIMER_STATE_* */
    uint32_t      pad;
} sys_timer_information_t;

#ifdef __cplusplus
extern "C" {
#endif

LV2_SYSCALL sys_timer_usleep(usecond_t sleep_time)
{
    lv2syscall1(SYS_TIMER_USLEEP, (u64)sleep_time);
    return_to_user_prog(int);
}

LV2_SYSCALL sys_timer_sleep(second_t sleep_time)
{
    lv2syscall1(SYS_TIMER_SLEEP, (u64)sleep_time);
    return_to_user_prog(int);
}

LV2_SYSCALL sys_timer_create(sys_timer_t *timer_id)
{
    lv2syscall1(SYS_TIMER_CREATE, (u64)(uintptr_t)timer_id);
    return_to_user_prog(int);
}

LV2_SYSCALL sys_timer_destroy(sys_timer_t timer_id)
{
    lv2syscall1(SYS_TIMER_DESTROY, (u64)timer_id);
    return_to_user_prog(int);
}

LV2_SYSCALL sys_timer_get_information(sys_timer_t timer_id,
                                      sys_timer_information_t *info)
{
    lv2syscall2(SYS_TIMER_GET_INFORMATION, (u64)timer_id, (u64)(uintptr_t)info);
    return_to_user_prog(int);
}

LV2_SYSCALL _sys_timer_start(sys_timer_t timer_id, system_time_t base_time,
                             usecond_t period)
{
    lv2syscall3(SYS_TIMER_START, (u64)timer_id, (u64)base_time, (u64)period);
    return_to_user_prog(int);
}

LV2_SYSCALL sys_timer_stop(sys_timer_t timer_id)
{
    lv2syscall1(SYS_TIMER_STOP, (u64)timer_id);
    return_to_user_prog(int);
}

LV2_SYSCALL sys_timer_connect_event_queue(sys_timer_t timer_id,
                                          sys_event_queue_t queue_id,
                                          uint64_t name, uint64_t data1,
                                          uint64_t data2)
{
    lv2syscall5(SYS_TIMER_CONNECT_EVENT_QUEUE, (u64)timer_id, (u64)queue_id,
                name, data1, data2);
    return_to_user_prog(int);
}

LV2_SYSCALL sys_timer_disconnect_event_queue(sys_timer_t timer_id)
{
    lv2syscall1(SYS_TIMER_DISCONNECT_EVENT_QUEUE, (u64)timer_id);
    return_to_user_prog(int);
}

/* One expiry at the absolute system time `expiration_time`. */
static inline int sys_timer_start_oneshot(sys_timer_t timer_id,
                                          system_time_t expiration_time)
{
    return _sys_timer_start(timer_id, expiration_time, 0);
}

/* An expiry every `period` microseconds, the first one `period` from now. */
static inline int sys_timer_start_periodic(sys_timer_t timer_id,
                                           usecond_t period)
{
    return _sys_timer_start(timer_id, 0, period);
}

/* First expiry at `exp_time`, then every `period` microseconds. */
static inline int sys_timer_start_periodic_absolute(sys_timer_t timer_id,
                                                    system_time_t exp_time,
                                                    usecond_t period)
{
    return _sys_timer_start(timer_id, exp_time, period);
}

#ifdef __cplusplus
}
#endif

#endif  /* PS3TC_COMPAT_SYS_TIMER_H */
