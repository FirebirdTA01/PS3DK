/*
 * sys-event-compat-test.c - Static assertion and aggregate initialization probe
 * for sys_event_t anonymous union compatibility.
 *
 * Verifies:
 * 1. sizeof(sys_event_t) == 32
 * 2. Exact member offsets (source=0, data1=data_1=8, data2=data_2=16, data3=data_3=24)
 * 3. Positional aggregate initialization {src, a, b, c} populates data1..3
 * 4. ppu-lv2.h REG_PASS_SYS_EVENT_QUEUE_RECEIVE compiles and operates on event
 */

#include <stddef.h>
#include <stdint.h>
#include <sys/event_queue.h>
#include <ppu-lv2.h>



#if defined(__cplusplus)
static_assert(sizeof(sys_event_t) == 32, "sizeof(sys_event_t) must be 32");
static_assert(offsetof(sys_event_t, source) == 0, "offset of source must be 0");
static_assert(offsetof(sys_event_t, data1) == 8, "offset of data1 must be 8");
static_assert(offsetof(sys_event_t, data_1) == 8, "offset of data_1 must be 8");
static_assert(offsetof(sys_event_t, data2) == 16, "offset of data2 must be 16");
static_assert(offsetof(sys_event_t, data_2) == 16, "offset of data_2 must be 16");
static_assert(offsetof(sys_event_t, data3) == 24, "offset of data3 must be 24");
static_assert(offsetof(sys_event_t, data_3) == 24, "offset of data_3 must be 24");
#elif defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
_Static_assert(sizeof(sys_event_t) == 32, "sizeof(sys_event_t) must be 32");
_Static_assert(offsetof(sys_event_t, source) == 0, "offset of source must be 0");
_Static_assert(offsetof(sys_event_t, data1) == 8, "offset of data1 must be 8");
_Static_assert(offsetof(sys_event_t, data_1) == 8, "offset of data_1 must be 8");
_Static_assert(offsetof(sys_event_t, data2) == 16, "offset of data2 must be 16");
_Static_assert(offsetof(sys_event_t, data_2) == 16, "offset of data_2 must be 16");
_Static_assert(offsetof(sys_event_t, data3) == 24, "offset of data3 must be 24");
_Static_assert(offsetof(sys_event_t, data_3) == 24, "offset of data_3 must be 24");
#else
typedef char assert_sizeof[(sizeof(sys_event_t) == 32) ? 1 : -1];
typedef char assert_offset_source[(offsetof(sys_event_t, source) == 0) ? 1 : -1];
typedef char assert_offset_data1[(offsetof(sys_event_t, data1) == 8) ? 1 : -1];
typedef char assert_offset_data_1[(offsetof(sys_event_t, data_1) == 8) ? 1 : -1];
typedef char assert_offset_data2[(offsetof(sys_event_t, data2) == 16) ? 1 : -1];
typedef char assert_offset_data_2[(offsetof(sys_event_t, data_2) == 16) ? 1 : -1];
typedef char assert_offset_data3[(offsetof(sys_event_t, data3) == 24) ? 1 : -1];
typedef char assert_offset_data_3[(offsetof(sys_event_t, data_3) == 24) ? 1 : -1];
#endif

#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-braces"
#endif
int test_aggregate_init(void)
{
    sys_event_t ev = { 0x1111ULL, 0x2222ULL, 0x3333ULL, 0x4444ULL };
    if (ev.source != 0x1111ULL) return 1;
    if (ev.data1 != 0x2222ULL) return 2;
    if (ev.data_1 != 0x2222ULL) return 3;
    if (ev.data2 != 0x3333ULL) return 4;
    if (ev.data_2 != 0x3333ULL) return 5;
    if (ev.data3 != 0x4444ULL) return 6;
    if (ev.data_3 != 0x4444ULL) return 7;
    return 0;
}
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

int test_macro_and_forwarders(sys_event_t *event)
{
    /* Exercise ppu-lv2.h REG_PASS_SYS_EVENT_QUEUE_RECEIVE compilation */
    u64 p1 = 0x10, p2 = 0x20, p3 = 0x30, p4 = 0x40, p5 = 0x50;
    (void)p1;
#ifdef REG_PASS_SYS_EVENT_QUEUE_RECEIVE
    REG_PASS_SYS_EVENT_QUEUE_RECEIVE;
#endif
    if (event->source != 0x20) return 1;
    if (event->data1 != 0x30 || event->data_1 != 0x30) return 2;
    if (event->data2 != 0x40 || event->data_2 != 0x40) return 3;
    if (event->data3 != 0x50 || event->data_3 != 0x50) return 4;

    /* Verify sysEventQueueReceive and sys_event_queue_receive compile */
    (void)sys_event_queue_receive;
    (void)sysEventQueueReceive;
    return 0;
}

int main(void)
{
    sys_event_t ev;
    if (test_aggregate_init() != 0) return 1;
    if (test_macro_and_forwarders(&ev) != 0) return 2;
    return 0;
}
