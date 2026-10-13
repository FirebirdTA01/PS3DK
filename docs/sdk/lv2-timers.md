# LV2 timers (sys/timer.h)

| Property | Value |
|---|---|
| Header | `<sys/timer.h>` |
| Syscalls | 70 71 72 73 74 75 76 (timers), 141 142 (sleeps) |
| Sample | `samples/lv2/hello-ppu-lv2-timer/` |

## What a kernel timer is

An LV2 kernel timer is a kernel-managed expiry counter that fires a
`sys_event` on a user thread.  The timer is created in the stopped state,
attached to an event queue, started in one of three modes (oneshot,
periodic, periodic-absolute), and torn down in reverse order.

Times are **LV2 system time** in microseconds, obtained from
`sys_time_get_system_time()` — not wall-clock time.  (Prior to v0.24 the
headers used wall-clock semantics; v0.24 corrected this to match the
kernel ABI.)

## Call order

    sys_timer_create          -> sys_timer_connect_event_queue
    -> sys_timer_start_*      -> sys_timer_stop
    -> sys_timer_disconnect_event_queue
    -> sys_timer_destroy

| Call | Syscall | Description |
|---|---|---|
| `sys_timer_create` | 70 | Allocate a timer (returned stopped); writes the id |
| `sys_timer_destroy` | 71 | Release the timer |
| `sys_timer_get_information` | 72 | Read `next_expiration_time`, `period`, `timer_state` |
| `_sys_timer_start` | 73 | Start one of three modes (see below) |
| `sys_timer_stop` | 74 | Stop; no expiries delivered until next start |
| `sys_timer_connect_event_queue` | 75 | Bind to event queue; sets `source`/`data1`/`data2` on every expiry |
| `sys_timer_disconnect_event_queue` | 76 | Unbind from the event queue |
| `sys_timer_usleep` | 141 | Sleep for `period` microseconds |
| `sys_timer_sleep` | 142 | Sleep for `seconds` seconds |

## The three start modes

| Helper | `base_time` | `period` | Behaviour |
|---|---|---|---|
| `sys_timer_start_oneshot(id, t)` | `t` | 0 | One expiry at absolute system time `t` |
| `sys_timer_start_periodic(id, p)` | 0 | `p` | Every `p` µs, first expiry `p` µs from now |
| `sys_timer_start_periodic_absolute(id, t, p)` | `t` | `p` | First at `t`, then every `p` µs |

## What each event carries

Each expiry delivers one event to the connected queue:

| Field | Value |
|---|---|
| `source` | the `name` argument to `sys_timer_connect_event_queue` |
| `data1`  | the `data1` argument to `sys_timer_connect_event_queue` |
| `data2`  | the `data2` argument to `sys_timer_connect_event_queue` |
| `data3`  | the expiry time (system time, µs) |

## Minimal example

    #include <sys/timer.h>
    #include <sys/event.h>
    #include <cell/error.h>

    int main(void)
    {
        sys_event_queue_t q;
        sys_timer_t       t;

        sys_event_queueCreate(&q, 16);
        sys_timer_create(&t);
        sys_timer_connect_event_queue(t, q, 42 /*name*/, 1 /*data1*/, 2 /*data2*/);

        /* Fire every 1 000 000 µs (1 s), first expiry 1 s from now. */
        sys_timer_start_periodic(t, 1000000);

        for (int i = 0; i < 5; i++) {
            sys_event_t ev;
            sys_eventQueueReceive(q, &ev, 0);   /* block until next expiry */
            /* ev.source == 42, ev.data1 == 1, ev.data2 == 2,
               ev.data3 == expiry time (µs, system clock)            */
        }

        sys_timer_stop(t);
        sys_timer_disconnect_event_queue(t);
        sys_timer_destroy(t);
        sys_event_queueDestroy(q);
        return CELL_OK;
    }

## States

| Constant | Value | Meaning |
|---|---|---|
| `SYS_TIMER_STATE_STOP` | 0x00 | Timer is stopped |
| `SYS_TIMER_STATE_RUN`  | 0x01 | Timer is running |

## Sample

`samples/lv2/hello-ppu-lv2-timer/` — a PPU program using the call order
above.
