/* Bracketed clock anchors with host supplied device facts (not evidence of RSX ordering or timer rate).
 *
 * The owner issues anchor k (label 2k-1, timestamp, label 2k) and returns; the drainer completes it later. The fake
 * device keeps the "RSX position" in `rsx` (how many of the issued label releases have executed) and writes the
 * report when the RSX passes the anchor's timestamp; the test moves it between drain polls. */
#include <assert.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "auto_clock.h"

static auto_clock clock_state;
static struct {
    uint64_t tick, raw, written_at;
    uint32_t mode, emits, flushes, k, rsx, reads, live_off, regress, future;
} fake;
enum { NORMAL, UNSTABLE, POISON, UNPUBLISHED, NO_ROOM, PARTIAL, THREADED };
static uint32_t threaded_rsx; /* THREADED: the label position, published by the owner thread */
static uint64_t thread_tick;
static uint64_t tb(void *u) {(void)u;return fake.mode == THREADED ? __atomic_fetch_add(&thread_tick, 1, __ATOMIC_ACQ_REL) : fake.tick++;}
static int live(void *u) {(void)u;return !fake.live_off;}
static int emit(void *u, uint32_t slot, uint32_t k)
{
    (void)u;
    assert(slot == 1024 + (k - 1) % 16);
    if (fake.mode == THREADED) return 1;
    if (fake.mode == NO_ROOM) return 0;
    fake.emits++;fake.k = k;
    return fake.mode == PARTIAL ? 2 : 1;
}
static void flush(void *u)
{
    (void)u;
    uint32_t serial = auto_clock_before(&clock_state);
    if (fake.mode == THREADED) {assert(auto_clock_after(&clock_state, serial));return;}
    fake.flushes++;
    assert(serial == fake.k);
    if (fake.mode != UNPUBLISHED) assert(auto_clock_after(&clock_state, serial));
}
/* the RSX executes up to label value `rsx`; passing 2k-1 writes anchor k's timestamp */
static void rsx_to(uint32_t value)
{
    if (value >= 2u * fake.k - 1u && fake.rsx < 2u * fake.k - 1u) {fake.written_at = fake.tick;fake.raw = 9000 + fake.k;}
    fake.rsx = value;
}
/* scripted label reads (each read takes the next value) for the second-read checks */
static uint32_t script[8], script_n, script_i;
static uint32_t label(void *u)
{
    (void)u;
    if (script_i < script_n) return script[script_i++];
    if (fake.mode == THREADED) return __atomic_load_n(&threaded_rsx, __ATOMIC_ACQUIRE);
    if (fake.regress) return fake.rsx - 2u;
    if (fake.future) return fake.rsx + 5u;
    return fake.rsx;
}
static uint64_t timer(void *u, uint32_t slot)
{
    (void)u;
    if (fake.mode == THREADED) return 5000u + slot;
    if (!script_n) assert(slot == 1024 + (fake.k - 1) % 16);   /* scripted cases complete older anchors too */
    fake.reads++;
    if (fake.mode == POISON) return UINT64_MAX;
    return fake.raw + (fake.mode == UNSTABLE ? fake.reads : 0);
}
static void sync_device(void *u) {(void)u;}
static const auto_clock_port port = {NULL, tb, live, emit, flush, label, timer, sync_device};

static void setup(uint32_t mode)
{
    assert(auto_clock_init(&clock_state, 1, 1, 1024, 100)); /* 2 s bound = 200 ticks */
    memset(&fake, 0, sizeof fake);fake.tick = 100;fake.mode = mode;
}
static const auto_wire_clock *done(void) {return auto_clock_peek(&clock_state);}

/* The RSX is far behind: many drain slices see it before the anchor (each one raises the lower bound), one sees it
 * between the two labels, then one after: VALID with the tightest bracket around the timestamp's write. */
static void far_behind(void)
{
    setup(NORMAL);
    assert(auto_clock_sample(&clock_state, &port, 42, 0) == 1);
    assert(!done() && fake.emits == 1 && fake.flushes == 1);   /* issued, nothing waited for */
    for (int i = 0; i < 50; i++) {assert(auto_clock_poll(&clock_state, &port, 0));assert(!done());}
    uint64_t lo_before = fake.tick;
    assert(auto_clock_poll(&clock_state, &port, 0));             /* last poll below 2k-1 */
    rsx_to(1);                                                 /* label 2k-1 executed, timestamp written */
    assert(auto_clock_poll(&clock_state, &port, 0) && !done()); /* between the labels: keep polling */
    rsx_to(2);
    assert(auto_clock_poll(&clock_state, &port, 0));
    const auto_wire_clock *r = done();
    assert(r && r->flags == 1 && r->serial == 1 && r->slot == 1024 && r->raw == 9001 && r->repeated == 9001);
    assert(r->label_first == 0 && r->label_second >= 2);
    assert(r->tb_before >= lo_before && r->tb_before <= fake.written_at && r->tb_after >= fake.written_at);
    assert(r->elapsed == r->tb_after - r->tb_before && r->polls == 53);
    assert(!auto_clock_ack(&clock_state, 1, 0) && auto_clock_ack(&clock_state, 1, 1) && !done());
}
/* The first drain poll already sees the anchor done: the issue time is the lower bound. */
static void first_poll_done(void)
{
    setup(NORMAL);
    assert(auto_clock_sample(&clock_state, &port, 42, 0) == 1);
    uint64_t issued = clock_state.issue[0];
    rsx_to(2);
    assert(auto_clock_poll(&clock_state, &port, 0));
    const auto_wire_clock *r = done();
    assert(r && r->flags == 1 && r->tb_before == issued && r->label_first == 0 && r->polls == 1);
    assert(auto_clock_ack(&clock_state, 1, 1));
}
/* No completion within 2 s of the issue: TIMEOUT, the slot quarantined; a late completion changes nothing. */
static void timeout_then_late(void)
{
    setup(NORMAL);
    assert(auto_clock_sample(&clock_state, &port, 42, 0) == 1);
    int n = 0;
    while (!done()) {assert(auto_clock_poll(&clock_state, &port, 0));assert(++n < 1000);}
    const auto_wire_clock *r = done();
    assert(r->flags == 2 && !r->raw && r->tb_after - clock_state.issue[0] >= 200);
    assert(auto_clock_ack(&clock_state, 1, 1) && clock_state.slots[0] == AUTO_CLOCK_QUARANTINED);
    rsx_to(2);                                                 /* the late write lands on the quarantined slot */
    assert(auto_clock_poll(&clock_state, &port, 0) && !done());
}
/* Seal ends anchors still PENDING as TIMEOUT after one last poll. */
static void seal(void)
{
    setup(NORMAL);
    assert(auto_clock_sample(&clock_state, &port, 42, 0) == 1);
    assert(auto_clock_poll(&clock_state, &port, 1));
    const auto_wire_clock *r = done();
    assert(r && r->flags == 2);
    assert(auto_clock_ack(&clock_state, 1, 1));
    setup(NORMAL);
    assert(auto_clock_sample(&clock_state, &port, 42, 0) == 1);
    rsx_to(2);
    assert(auto_clock_poll(&clock_state, &port, 1) && done()->flags == 1); /* completed at seal: still VALID */
}
/* A label below the last value seen, or above anything issued, invalidates the instance. */
static void regression_and_future(void)
{
    setup(NORMAL);
    assert(auto_clock_sample(&clock_state, &port, 42, 0) == 1);
    rsx_to(2);assert(auto_clock_poll(&clock_state, &port, 0));assert(auto_clock_ack(&clock_state, 1, 1));
    assert(auto_clock_sample(&clock_state, &port, 42, 8) == 1);
    fake.regress = 1;
    assert(!auto_clock_poll(&clock_state, &port, 0) && clock_state.invalid);
    assert(done() && done()->flags == 8);
    assert(auto_clock_ack(&clock_state, 2, 1) && clock_state.slots[1] == AUTO_CLOCK_QUARANTINED);
    setup(NORMAL);
    assert(auto_clock_sample(&clock_state, &port, 42, 0) == 1);
    fake.future = 1;
    assert(!auto_clock_poll(&clock_state, &port, 0) && clock_state.invalid && done()->flags == 8);
}
/* Every label read is checked, the second read of a completion included, and the highest value seen is kept:
 * one prepared anchor reading 2 then 99 is a future write; two anchors reading 2 then 4, then 3, is a regression. */
static void second_read_checked(void)
{
    setup(NORMAL);script_i = 0;
    assert(auto_clock_sample(&clock_state, &port, 42, 0) == 1);
    script[0] = 2;script[1] = 99;script_n = 2;
    assert(!auto_clock_poll(&clock_state, &port, 0) && clock_state.invalid);
    setup(NORMAL);script_i = 0;script_n = 0;
    assert(auto_clock_sample(&clock_state, &port, 42, 0) == 1);
    assert(auto_clock_sample(&clock_state, &port, 42, 8) == 1);
    script[0] = 2;script[1] = 4;script_n = 2;
    assert(auto_clock_poll(&clock_state, &port, 0) && !clock_state.invalid && clock_state.last_label == 4);
    script[2] = 3;script_n = 3;
    assert(!auto_clock_poll(&clock_state, &port, 0) && clock_state.invalid);
    script_i = script_n = 0;
}
/* Failures that never reach the RSX in full are immediate facts; nothing is polled. */
static void immediate(uint32_t mode)
{
    setup(mode);
    assert(auto_clock_sample(&clock_state, &port, 42, 0) == 1);
    const auto_wire_clock *r = done();
    assert(r && r->flags == 8 && !r->polls && !fake.reads);
    assert(auto_clock_ack(&clock_state, 1, 1));
    /* nothing written frees the slot; a written command quarantines it */
    assert(clock_state.slots[0] == (mode == NO_ROOM ? AUTO_CLOCK_FREE : AUTO_CLOCK_QUARANTINED));
}
/* An unstable or poisoned timer completes as a refused read, never VALID. */
static void bad_timer(uint32_t mode)
{
    setup(mode);
    assert(auto_clock_sample(&clock_state, &port, 42, 0) == 1);
    rsx_to(2);
    assert(auto_clock_poll(&clock_state, &port, 0));
    assert(done() && done()->flags == 8 && !done()->raw);
}
/* Owner and drainer on separate threads: the owner issues as room allows, the drainer polls, peeks and acks. */
static void *produce(void *u)
{
    (void)u;
    for (unsigned i = 0; i < 200; i++) {
        while (!auto_clock_room(&clock_state)) {}
        assert(auto_clock_sample(&clock_state, &port, 42, i) == 1);
        __atomic_store_n(&threaded_rsx, 2u * (i + 1u), __ATOMIC_RELEASE); /* the RSX runs it right away */
    }
    return NULL;
}
static void *consume(void *u)
{
    (void)u;
    for (unsigned i = 0; i < 200; i++) {
        const auto_wire_clock *r;
        while (!(r = auto_clock_peek(&clock_state))) assert(auto_clock_poll(&clock_state, &port, 0));
        assert(r->id == i + 1 && r->serial == i + 1 && r->flags == 1 && r->raw == 5000u + r->slot);
        assert(auto_clock_ack(&clock_state, i + 1, 1));
    }
    return NULL;
}
int main(void)
{
    far_behind();first_poll_done();timeout_then_late();seal();regression_and_future();second_read_checked();
    immediate(NO_ROOM);immediate(PARTIAL);immediate(UNPUBLISHED);
    bad_timer(UNSTABLE);bad_timer(POISON);
    setup(THREADED);thread_tick = 1000;
    assert(!auto_clock_after(&clock_state, 1));
    pthread_t p, c;
    assert(!pthread_create(&p, NULL, produce, NULL));assert(!pthread_create(&c, NULL, consume, NULL));
    assert(!pthread_join(p, NULL));assert(!pthread_join(c, NULL));
    assert(clock_state.attempted == 200 && !clock_state.lost && !clock_state.quarantined);
    clock_state.serial = UINT32_MAX / 2u - 1u;
    assert(auto_clock_sample(&clock_state, &port, 42, 300) == 2 && clock_state.lost == 1);
    puts("AUTO clock anchors: issue without waiting, drainer brackets, timeout, seal, regression and handoff PASS");
}
