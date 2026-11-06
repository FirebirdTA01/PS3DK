/* The capture record (live link, target checkpoint 1): published once by a successful init, immutable afterwards,
 * copied out by ps3dk_prof_get_capture_record.  A failed init (retryable or terminal) publishes nothing; a callback
 * sink has no record; on every error the caller's struct is left untouched. */
#include "rt.h"

#include <errno.h>
#include <string.h>

static int gate(void) { return (int)RT_STAT(GATE); }

static uint64_t clk_const(void) { return 0x1234; }

/* A struct full of a marker byte: any write by a failed call shows. */
static void poison(ps3dk_prof_capture_record *r) { memset(r, 0xA5, sizeof *r); }

static int untouched(const ps3dk_prof_capture_record *r)
{
    const unsigned char *b = (const unsigned char *)r;
    for (size_t i = 0; i < sizeof *r; i++)
        if (b[i] != 0xA5)
            return 0;
    return 1;
}

/* The record names exactly the capture in dir, this app and build id. */
static int record_matches(const ps3dk_prof_capture_record *r, const char *dir, const char *build_id)
{
    char path[512];
    CHECK_EQ(rt_capture_path(dir, path, sizeof path), 0);
    CHECK(strlen(r->path) < sizeof r->path);
    CHECK_EQ(strcmp(r->path, path), 0);
    CHECK_EQ(r->session_id, 0x1234ull);
    CHECK_EQ(strcmp(r->app, "rt"), 0);
    CHECK_EQ(r->app_len, 2u);
    CHECK_EQ(r->build_id_len, (uint32_t)strlen(build_id));
    CHECK_EQ(memcmp(r->build_id, build_id, r->build_id_len), 0);
    return 0;
}

int rt_capture_record_null_and_before_init(const char *out)
{
    ps3dk_prof_capture_record r;
    ps3dk_prof_config c;
    (void)out;
    CHECK_EQ(ps3dk_prof_get_capture_record(NULL), PS3DK_PROF_EINVAL);
    poison(&r);
    CHECK_EQ(ps3dk_prof_get_capture_record(&r), PS3DK_PROF_ENOTINIT);
    CHECK(untouched(&r));
    /* A configured but not initialized profiler has no record either. */
    rt_config(&c);
    CHECK_EQ(rt_set_dir(out), PS3DK_PROF_OK);
    CHECK_EQ(ps3dk_prof_get_capture_record(&r), PS3DK_PROF_ENOTINIT);
    CHECK(untouched(&r));
    return 0;
}

int rt_capture_record_failed_init_then_retry(const char *out)
{
    ps3dk_prof_capture_record r;
    ps3dk_prof_config c;
    rt_config(&c);
    ps3dk_prof_test_hooks.clock = clk_const;
    CHECK_EQ(rt_set_dir(out), PS3DK_PROF_OK);
    ps3dk_prof_test_hooks.open_errno = EACCES;
    CHECK_EQ(ps3dk_prof_init(&c), PS3DK_PROF_ESINKFAIL);   /* retryable: nothing created */
    CHECK_EQ(gate(), RT_G_UNINIT);
    poison(&r);
    CHECK_EQ(ps3dk_prof_get_capture_record(&r), PS3DK_PROF_ENOTINIT);
    CHECK(untouched(&r));
    ps3dk_prof_test_hooks.open_errno = 0;
    CHECK_EQ(ps3dk_prof_init(&c), PS3DK_PROF_OK);
    CHECK_EQ(ps3dk_prof_get_capture_record(&r), PS3DK_PROF_OK);
    CHECK_EQ(record_matches(&r, out, c.build_id), 0);
    CHECK_EQ(ps3dk_prof_thread_begin("main"), PS3DK_PROF_OK);
    CHECK_EQ(ps3dk_prof_shutdown(), PS3DK_PROF_OK);
    return rt_expect(out, 0);
}

int rt_capture_record_terminal_init_publishes_nothing(const char *out)
{
    ps3dk_prof_capture_record r;
    ps3dk_prof_config c;
    char path[512];
    rt_config(&c);
    c.mode = PS3DK_PROF_MODE_THREAD;
    CHECK_EQ(rt_set_dir(out), PS3DK_PROF_OK);
    ps3dk_prof_test_hooks.thread_create_fail = 1;
    CHECK_EQ(ps3dk_prof_init(&c), PS3DK_PROF_ETHREAD);
    CHECK_EQ(gate(), RT_G_TERMINAL);                 /* the file was created and kept */
    CHECK_EQ(rt_capture_path(out, path, sizeof path), 0);
    poison(&r);
    CHECK_EQ(ps3dk_prof_get_capture_record(&r), PS3DK_PROF_ENOTINIT);
    CHECK(untouched(&r));
    ps3dk_prof_test_hooks.thread_create_fail = 0;
    rt_expect_path(path, 1);
    return 0;
}

int rt_capture_record_callback_sink_enotsup(const char *out)
{
    ps3dk_prof_capture_record r;
    ps3dk_prof_config c;
    char path[512];
    rt_config(&c);
    CHECK_EQ(rt_use_mem_sink(0), 0);
    CHECK_EQ(ps3dk_prof_init(&c), PS3DK_PROF_OK);
    poison(&r);
    CHECK_EQ(ps3dk_prof_get_capture_record(&r), PS3DK_PROF_ENOTSUP);
    CHECK(untouched(&r));
    CHECK_EQ(ps3dk_prof_get_capture_record(NULL), PS3DK_PROF_EINVAL);
    CHECK_EQ(ps3dk_prof_thread_begin("main"), PS3DK_PROF_OK);
    CHECK_EQ(ps3dk_prof_shutdown(), PS3DK_PROF_OK);
    CHECK_EQ(ps3dk_prof_get_capture_record(&r), PS3DK_PROF_ENOTSUP);
    CHECK(untouched(&r));
    snprintf(path, sizeof path, "%s/cb.prof", out);
    CHECK_EQ(rt_mem_dump(path), 0);
    rt_expect_path(path, 0);
    return 0;
}

int rt_capture_record_file_init_and_after_shutdown(const char *out)
{
    ps3dk_prof_capture_record r, again;
    ps3dk_prof_config c;
    /* A 512-byte build id (the limit): length-delimited, no NUL inside the record. */
    static char bid[513];
    for (int i = 0; i < 512; i++)
        bid[i] = (char)('a' + i % 26);
    bid[512] = 0;
    rt_config(&c);
    c.build_id = bid;
    ps3dk_prof_test_hooks.clock = clk_const;
    CHECK_EQ(rt_set_dir(out), PS3DK_PROF_OK);
    CHECK_EQ(ps3dk_prof_init(&c), PS3DK_PROF_OK);
    CHECK_EQ(ps3dk_prof_get_capture_record(&r), PS3DK_PROF_OK);
    CHECK_EQ(record_matches(&r, out, bid), 0);
    CHECK_EQ(r.build_id_len, 512u);
    ps3dk_prof_capture_record copy = r;
    CHECK_EQ(ps3dk_prof_thread_begin("main"), PS3DK_PROF_OK);
    {
        PS3DK_PROF_ZONE("z");
    }
    CHECK_EQ(ps3dk_prof_shutdown(), PS3DK_PROF_OK);
    /* Readable after shutdown, unchanged; the caller's earlier copy is its own. */
    CHECK_EQ(ps3dk_prof_get_capture_record(&again), PS3DK_PROF_OK);
    CHECK_EQ(memcmp(&again, &copy, sizeof again), 0);
    CHECK_EQ(memcmp(&r, &copy, sizeof r), 0);
    CHECK_EQ(PS3DK_PROF_GET_CAPTURE_RECORD(&again), PS3DK_PROF_OK);   /* the macro is the call in an enabled build */
    return rt_expect(out, 0);
}

/* ---- publication ordering ------------------------------------------------------------------------------------------ */

static rt_event g_in_publish, g_reader_checked;
static int g_reader_saw_ok_early;

/* Runs inside init after the record is filled and before it is published: a reader now must still see ENOTINIT. */
static void hold_publish(void)
{
    rt_event_set(&g_in_publish);
    (void)rt_event_wait(&g_reader_checked, 5000);
}

static void *early_reader(void *arg)
{
    ps3dk_prof_capture_record r;
    (void)arg;
    WCHECK(rt_event_wait(&g_in_publish, 5000) == 0);
    poison(&r);
    int rc = ps3dk_prof_get_capture_record(&r);
    if (rc != PS3DK_PROF_ENOTINIT || !untouched(&r))
        AST(g_reader_saw_ok_early, 1);
    rt_event_set(&g_reader_checked);
    return NULL;
}

/* A reader that spins until it gets the record must find every field complete on its first success. */
static int g_spin_ok;
static const char *g_spin_build_id;
static void *spinning_reader(void *arg)
{
    const char *dir = (const char *)arg;
    ps3dk_prof_capture_record r;
    uint64_t until = rt_now_ms() + 5000;   /* time-bounded, not count-bounded: sanitizers slow the loop */
    while (rt_now_ms() < until) {
        poison(&r);
        int rc = ps3dk_prof_get_capture_record(&r);
        if (rc == PS3DK_PROF_ENOTINIT) {
            WCHECK(untouched(&r));
            continue;
        }
        WCHECK(rc == PS3DK_PROF_OK);
        WCHECK(record_matches(&r, dir, g_spin_build_id) == 0);
        AST(g_spin_ok, 1);
        return NULL;
    }
    return NULL;
}

int rt_capture_record_publication_ordering(const char *out)
{
    ps3dk_prof_config c;
    rt_thread early, spin;
    rt_config(&c);
    ps3dk_prof_test_hooks.clock = clk_const;
    CHECK_EQ(rt_set_dir(out), PS3DK_PROF_OK);
    rt_event_init(&g_in_publish);
    rt_event_init(&g_reader_checked);
    ps3dk_prof_test_hooks.before_capture_publish = hold_publish;
    g_spin_build_id = c.build_id;
    rt_spawn(&early, early_reader, NULL);
    rt_spawn(&spin, spinning_reader, (void *)out);
    CHECK_EQ(ps3dk_prof_init(&c), PS3DK_PROF_OK);
    rt_join(&early);
    rt_join(&spin);
    ps3dk_prof_test_hooks.before_capture_publish = NULL;
    CHECK(!rt_worker_failed());
    CHECK_EQ(ALD(g_reader_saw_ok_early), 0);   /* filled but unpublished: still ENOTINIT, untouched */
    CHECK_EQ(ALD(g_spin_ok), 1);               /* the first success was a complete record */
    CHECK_EQ(ps3dk_prof_thread_begin("main"), PS3DK_PROF_OK);
    CHECK_EQ(ps3dk_prof_shutdown(), PS3DK_PROF_OK);
    return rt_expect(out, 0);
}
