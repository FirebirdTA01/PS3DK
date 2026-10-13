/* <sys/timer.h> surface: every LV2 timer call, as the reference SDK spells it.
 * sys-timer-surface-test.sh compiles this in C and C++, ILP32 and LP64, and
 * checks each wrapper's syscall number in the object code. */
#include <sys/timer.h>
#include <sys/sys_time.h>

#ifdef __cplusplus
#define STATIC_ASSERT static_assert
#else
#define STATIC_ASSERT _Static_assert
#endif
#define NOINLINE __attribute__((noinline))

NOINLINE int t_create(sys_timer_t *id) { return sys_timer_create(id); }
NOINLINE int t_destroy(sys_timer_t id) { return sys_timer_destroy(id); }
NOINLINE int t_info(sys_timer_t id, sys_timer_information_t *i) { return sys_timer_get_information(id, i); }
NOINLINE int t_start(sys_timer_t id, system_time_t b, usecond_t p) { return _sys_timer_start(id, b, p); }
NOINLINE int t_stop(sys_timer_t id) { return sys_timer_stop(id); }
NOINLINE int t_connect(sys_timer_t id, sys_event_queue_t q) { return sys_timer_connect_event_queue(id, q, SYS_TIMER_EVENT_NO_NAME, 1, 2); }
NOINLINE int t_disconnect(sys_timer_t id) { return sys_timer_disconnect_event_queue(id); }
NOINLINE int t_usleep(usecond_t u) { return sys_timer_usleep(u); }
NOINLINE int t_sleep(second_t s) { return sys_timer_sleep(s); }
/* LV2 system time is the sysPrxForUser import, never an inline syscall. */
NOINLINE system_time_t t_systime(void) { return sys_time_get_system_time(); }

int t_helpers(sys_timer_t id)
{
    int r = sys_timer_start_oneshot(id, 1000);
    r |= sys_timer_start_periodic(id, 16666);
    r |= sys_timer_start_periodic_absolute(id, 2000, 16666);
    return r;
}

STATIC_ASSERT(sizeof(sys_timer_t) == 4, "sys_timer_t is a 32-bit id");
STATIC_ASSERT(sizeof(sys_timer_information_t) == 24, "info = 8 + 8 + 4 + 4");
STATIC_ASSERT(SYS_TIMER_ID_INVALID == 0xFFFFFFFFU, "invalid id");
STATIC_ASSERT(SYS_TIMER_STATE_STOP == 0 && SYS_TIMER_STATE_RUN == 1, "states");
