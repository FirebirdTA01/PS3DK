/*
 * hello-ppu-lv2-timer - an event loop driven by an LV2 kernel timer.
 *
 * A periodic timer posts one event per expiry to an event queue; the main
 * loop blocks on the queue and handles each tick.  Then a one-shot timer
 * fires once at an absolute time.  Every step is checked, and the last TTY
 * line is "hello-ppu-lv2-timer: PASS (<n> checks)" or names the failures.
 */
#include <stdint.h>
#include <stdio.h>
#include <sys/process.h>
#include <sys/event.h>
#include <sys/sys_time.h>
#include <sys/timer.h>
#include <sys/ppu_thread.h>

SYS_PROCESS_PARAM(1001, 0x10000);

#define TICK_NAME    0x7469636bULL   /* "tick" */
#define TICK_DATA1   0x1111ULL
#define TICK_DATA2   0x2222ULL
#define PERIOD_US    50000ULL        /* 20 Hz */
#define TICKS        5

static int checks, failures;
static int setup_once = SYS_PPU_THREAD_ONCE_INIT;
static int setup_runs;

static void setup(void) { setup_runs++; }

static void check(int ok, const char *what, unsigned long long got)
{
	checks++;
	if (!ok)
		failures++;
	printf("  %s %s (0x%llx)\n", ok ? "ok  " : "FAIL", what, got);
}

int main(void)
{
	sys_event_queue_t queue;
	sys_event_queue_attribute_t attr;
	sys_timer_t timer = SYS_TIMER_ID_INVALID;
	sys_timer_information_t info;
	sys_event_t ev;
	int rc;

	printf("hello-ppu-lv2-timer: periodic and one-shot LV2 timers\n");

	/* The event loop runs on the main thread: name it, and set the loop
	 * up exactly once however often setup is called. */
	sys_ppu_thread_t self;
	rc = sys_ppu_thread_get_id(&self);
	check(rc == 0, "sys_ppu_thread_get_id", (unsigned)rc);
	rc = sys_ppu_thread_rename(self, "timer-loop");
	check(rc == 0, "sys_ppu_thread_rename(\"timer-loop\")", (unsigned)rc);
	int joinable = -1;
	sys_ppu_thread_get_join_state(&joinable);
	check(joinable == 0 || joinable == 1, "sys_ppu_thread_get_join_state", (unsigned)joinable);
	sys_ppu_thread_once(&setup_once, setup);
	sys_ppu_thread_once(&setup_once, setup);
	check(setup_runs == 1 && setup_once == SYS_PPU_THREAD_DONE_INIT,
	      "sys_ppu_thread_once runs setup once", (unsigned)setup_runs);

	sys_event_queue_attribute_initialize(attr);
	rc = sys_event_queue_create(&queue, &attr, SYS_EVENT_QUEUE_LOCAL, 16);
	check(rc == 0, "sys_event_queue_create", (unsigned)rc);

	rc = sys_timer_create(&timer);
	check(rc == 0 && timer != SYS_TIMER_ID_INVALID, "sys_timer_create", timer);

	rc = sys_timer_get_information(timer, &info);
	check(rc == 0 && info.timer_state == SYS_TIMER_STATE_STOP,
	      "a new timer is stopped", info.timer_state);

	rc = sys_timer_connect_event_queue(timer, queue, TICK_NAME, TICK_DATA1, TICK_DATA2);
	check(rc == 0, "sys_timer_connect_event_queue", (unsigned)rc);

	/* Periodic: the loop handles TICKS expiries. */
	system_time_t started = sys_time_get_system_time();
	rc = sys_timer_start_periodic(timer, PERIOD_US);
	check(rc == 0, "sys_timer_start_periodic(50 ms)", (unsigned)rc);

	rc = sys_timer_get_information(timer, &info);
	check(rc == 0 && info.timer_state == SYS_TIMER_STATE_RUN && info.period == PERIOD_US,
	      "a started timer runs with its period", info.period);

	uint64_t last = 0;
	for (int i = 0; i < TICKS; i++) {
		rc = sys_event_queue_receive(queue, &ev, 1000000);
		if (rc != 0) {
			check(0, "tick received within 1 s", (unsigned)rc);
			break;
		}
		int ok = ev.source == TICK_NAME && ev.data1 == TICK_DATA1 &&
		         ev.data2 == TICK_DATA2 && ev.data3 > last;
		printf("  tick %d: source 0x%llx data1 0x%llx data2 0x%llx time %llu\n", i,
		       (unsigned long long)ev.source, (unsigned long long)ev.data1,
		       (unsigned long long)ev.data2, (unsigned long long)ev.data3);
		check(ok, "tick carries the connected name/data and a later time", ev.data3);
		last = ev.data3;
	}
	system_time_t elapsed = sys_time_get_system_time() - started;
	/* Five 50 ms periods; the bounds only catch a timer that ignores the
	 * period (fires at once, or far too late). */
	check(elapsed >= (system_time_t)(TICKS * PERIOD_US * 8 / 10) &&
	      elapsed <= (system_time_t)(TICKS * PERIOD_US * 4),
	      "five ticks take about 250 ms", (unsigned long long)elapsed);

	rc = sys_timer_stop(timer);
	check(rc == 0, "sys_timer_stop", (unsigned)rc);
	rc = sys_timer_get_information(timer, &info);
	check(rc == 0 && info.timer_state == SYS_TIMER_STATE_STOP,
	      "a stopped timer reports STOP", info.timer_state);

	/* Drain at most one tick that expired before the stop, then expect silence. */
	int drained = 0;
	while (sys_event_queue_receive(queue, &ev, 200000) == 0 && drained < 2)
		drained++;
	check(drained <= 1, "no ticks after stop", (unsigned)drained);

	/* One-shot at an absolute time 100 ms from now. */
	system_time_t at = sys_time_get_system_time() + 100000;
	rc = sys_timer_start_oneshot(timer, at);
	check(rc == 0, "sys_timer_start_oneshot(now + 100 ms)", (unsigned)rc);
	rc = sys_event_queue_receive(queue, &ev, 1000000);
	check(rc == 0 && ev.source == TICK_NAME, "the one-shot fires", (unsigned)rc);
	check(sys_time_get_system_time() >= at, "not before its time",
	      (unsigned long long)at);
	rc = sys_event_queue_receive(queue, &ev, 300000);
	check(rc != 0, "and fires only once", (unsigned)rc);

	rc = sys_timer_disconnect_event_queue(timer);
	check(rc == 0, "sys_timer_disconnect_event_queue", (unsigned)rc);
	rc = sys_timer_destroy(timer);
	check(rc == 0, "sys_timer_destroy", (unsigned)rc);
	rc = sys_timer_destroy(timer);
	check(rc != 0, "a destroyed timer id is refused", (unsigned)rc);
	rc = sys_event_queue_destroy(queue, 0);
	check(rc == 0, "sys_event_queue_destroy", (unsigned)rc);

	if (failures)
		printf("hello-ppu-lv2-timer: FAIL (%d of %d checks)\n", failures, checks);
	else
		printf("hello-ppu-lv2-timer: PASS (%d checks)\n", checks);
	return failures ? 1 : 0;
}
