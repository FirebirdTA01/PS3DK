/* Host test utilities: setup, events, threads, the memory sink and a mini capture reader. */
#define _POSIX_C_SOURCE 200809L
#include "rt.h"

#include <dirent.h>
#include <errno.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

void rt_config(ps3dk_prof_config *c)
{
    ps3dk_prof_config_defaults(c);
    c->app = "rt";
    c->ring_records = 512;
    c->max_threads = 8;
    c->max_descriptors = 64;
    c->quiesce_timeout_us = 2000000;
    c->build_id = "rt-build";
    c->flags = "-O1";
    c->workload_tag = "host";
}

int rt_set_dir(const char *out)
{
    int rc = ps3dk_prof_set_sink_path(out);
    if (rc != PS3DK_PROF_OK)
        fprintf(stderr, "set_sink_path(%s) = %d\n", out, rc);
    return rc;
}

int rt_start(const char *out, ps3dk_prof_config *c, int register_main)
{
    int rc;
    if (out && rt_set_dir(out) != PS3DK_PROF_OK)
        return 1;
    rc = ps3dk_prof_init(c);
    if (rc != PS3DK_PROF_OK) {
        fprintf(stderr, "init = %d\n", rc);
        return 1;
    }
    if (register_main) {
        rc = ps3dk_prof_thread_begin("main");
        if (rc != PS3DK_PROF_OK) {
            fprintf(stderr, "thread_begin(main) = %d\n", rc);
            return 1;
        }
    }
    return 0;
}

int rt_capture_path(const char *dir, char *buf, size_t n)
{
    DIR *d = opendir(dir);
    struct dirent *e;
    int found = 0;
    if (!d)
        return -1;
    while ((e = readdir(d)) != NULL) {
        size_t l = strlen(e->d_name);
        if (l > 5 && strcmp(e->d_name + l - 5, ".prof") == 0) {
            found++;
            snprintf(buf, n, "%s/%s", dir, e->d_name);
        }
    }
    closedir(d);
    return found == 1 ? 0 : -1;
}

void rt_expect_path(const char *path, int code)
{
    printf("CAPTURE %s EXPECT %d\n", path, code);
    fflush(stdout);
}

void rt_report_line(const char *path, const char *mode, const char *fmt, ...)
{
    va_list ap;
    printf("REPORT %s %s ", path, mode);
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    printf("\n");
    fflush(stdout);
}

int rt_expect(const char *dir, int code)
{
    char path[512];
    if (rt_capture_path(dir, path, sizeof path) != 0) {
        fprintf(stderr, "no single capture in %s\n", dir);
        return 1;
    }
    rt_expect_path(path, code);
    return 0;
}

uint64_t rt_file_hash(const char *path)
{
    FILE *f = fopen(path, "rb");
    uint64_t h = 0xcbf29ce484222325ull;
    int ch;
    if (!f)
        return 0;
    while ((ch = fgetc(f)) != EOF) {
        h ^= (uint8_t)ch;
        h *= 0x100000001b3ull;
    }
    fclose(f);
    return h;
}

long rt_file_size(const char *path)
{
    struct stat st;
    if (stat(path, &st) != 0)
        return -1;
    return (long)st.st_size;
}

void rt_sleep_ms(unsigned ms)
{
    struct timespec ts;
    ts.tv_sec = ms / 1000;
    ts.tv_nsec = (long)(ms % 1000) * 1000000L;
    while (nanosleep(&ts, &ts) != 0 && errno == EINTR) {
    }
}

uint64_t rt_now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

int rt_wait_latch(void)
{
    uint64_t end = rt_now_ms() + 5000;
    while (ps3dk_prof_test_stat(PS3DK_PROF_TEST_STAT_LATCH) == 0) {
        if (rt_now_ms() > end) {
            fprintf(stderr, "latch never set\n");
            return -1;
        }
        rt_sleep_ms(1);
    }
    return 0;
}

/* ---- events and threads ------------------------------------------------------------------------------------------- */

typedef struct ev_impl {
    pthread_mutex_t m;
    pthread_cond_t c;
    int set;
} ev_impl;

void rt_event_init(rt_event *e)
{
    ev_impl *i = (ev_impl *)calloc(1, sizeof *i);
    pthread_mutex_init(&i->m, NULL);
    pthread_cond_init(&i->c, NULL);
    e->impl = i;
}

void rt_event_set(rt_event *e)
{
    ev_impl *i = (ev_impl *)e->impl;
    pthread_mutex_lock(&i->m);
    i->set = 1;
    pthread_cond_broadcast(&i->c);
    pthread_mutex_unlock(&i->m);
}

void rt_event_reset(rt_event *e)
{
    ev_impl *i = (ev_impl *)e->impl;
    pthread_mutex_lock(&i->m);
    i->set = 0;
    pthread_mutex_unlock(&i->m);
}

int rt_event_wait(rt_event *e, unsigned timeout_ms)
{
    ev_impl *i = (ev_impl *)e->impl;
    struct timespec ts;
    int rc = 0;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec += timeout_ms / 1000;
    ts.tv_nsec += (long)(timeout_ms % 1000) * 1000000L;
    if (ts.tv_nsec >= 1000000000L) {
        ts.tv_sec++;
        ts.tv_nsec -= 1000000000L;
    }
    pthread_mutex_lock(&i->m);
    while (!i->set && rc == 0)
        rc = pthread_cond_timedwait(&i->c, &i->m, &ts);
    rc = i->set ? 0 : -1;
    pthread_mutex_unlock(&i->m);
    return rc;
}

void rt_spawn(rt_thread *t, void *(*fn)(void *), void *arg)
{
    pthread_t *p = (pthread_t *)malloc(sizeof *p);
    if (pthread_create(p, NULL, fn, arg) != 0) {
        fprintf(stderr, "pthread_create failed\n");
        exit(99);
    }
    t->impl = p;
}

void *rt_join(rt_thread *t)
{
    void *r = NULL;
    pthread_join(*(pthread_t *)t->impl, &r);
    free(t->impl);
    t->impl = NULL;
    return r;
}

/* ---- memory sink -------------------------------------------------------------------------------------------------- */

rt_mem g_mem;

static void order_add(char ch)
{
    size_t l = strlen(g_mem.order);
    if (l + 1 < sizeof g_mem.order)
        g_mem.order[l] = ch;
}

static long mem_write(void *ctx, const void *buf, unsigned long n)
{
    (void)ctx;
    g_mem.writes++;
    if (g_mem.writes == 1)
        order_add('w');
    {
        void (*fn)(void) = __atomic_load_n(&g_mem.in_write, __ATOMIC_SEQ_CST);   /* tests change it while running */
        if (fn)
            fn();
    }
    if (g_mem.block_ms && ps3dk_prof_test_stat(PS3DK_PROF_TEST_STAT_LATCH))
        rt_sleep_ms(g_mem.block_ms);
    if (g_mem.neg_after && g_mem.writes >= (unsigned)g_mem.neg_after)
        return -5;
    if (g_mem.over_n)
        return (long)n + 1;
    if (g_mem.len + n > g_mem.cap) {
        size_t nc = (g_mem.cap ? g_mem.cap * 2 : 65536);
        while (nc < g_mem.len + n)
            nc *= 2;
        g_mem.buf = (uint8_t *)realloc(g_mem.buf, nc);
        g_mem.cap = nc;
    }
    memcpy(g_mem.buf + g_mem.len, buf, n);
    g_mem.len += n;
    return (long)n;
}

static int mem_flush(void *ctx)
{
    (void)ctx;
    g_mem.flushes++;
    order_add('f');
    return g_mem.flush_ret;
}

static int mem_close(void *ctx)
{
    (void)ctx;
    g_mem.closes++;
    order_add('c');
    return g_mem.close_ret;
}

int rt_use_mem_sink(int with_flush)
{
    ps3dk_prof_sink_callbacks cb;
    int rc;
    memset(&cb, 0, sizeof cb);
    cb.write = mem_write;
    cb.flush = with_flush ? mem_flush : NULL;
    cb.close = mem_close;
    rc = ps3dk_prof_set_sink_callbacks(&cb);
    if (rc != PS3DK_PROF_OK)
        fprintf(stderr, "set_sink_callbacks = %d\n", rc);
    return rc;
}

int rt_mem_dump(const char *path)
{
    FILE *f = fopen(path, "wb");
    if (!f)
        return -1;
    if (g_mem.len && fwrite(g_mem.buf, 1, g_mem.len, f) != g_mem.len) {
        fclose(f);
        return -1;
    }
    return fclose(f) == 0 ? 0 : -1;
}

void rt_desc(ps3dk_prof_desc *d, const char *name, uint32_t line)
{
    memset(d, 0, sizeof *d);
    d->name = name;
    d->logical_tag = "";
    d->file = "rt_runtime.c";
    d->line = line;
    d->scope = 1;
}

/* ---- mini reader -------------------------------------------------------------------------------------------------- */

static uint32_t r32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static uint64_t r64(const uint8_t *p) { return (uint64_t)r32(p) | (uint64_t)r32(p + 4) << 32; }
static uint16_t r16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }

static uint32_t crc32_rt(const uint8_t *b, size_t n)
{
    uint32_t c = 0xFFFFFFFFu;
    size_t i;
    int k;
    for (i = 0; i < n; i++) {
        c ^= b[i];
        for (k = 0; k < 8; k++)
            c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1u)));
    }
    return ~c;
}

static void final_at(rt_cap *c, const uint8_t *p, int in_retired)
{
    rt_final *f;
    if (c->nfinal >= 2048)
        return;
    f = &c->finals[c->nfinal++];
    f->inc = r32(p);
    f->state = p[4];
    f->limit = p[5];
    f->produced = r64(p + 8);
    f->written = r64(p + 16);
    f->dropped = r64(p + 24);
    f->refused = r64(p + 32);
    f->regerr = r64(p + 40);
    f->open_spans = r64(p + 48);
    f->in_retired = in_retired;
}

int rt_cap_load(const char *path, rt_cap *c)
{
    FILE *f = fopen(path, "rb");
    uint8_t *b;
    long sz;
    size_t pos = 16, cap = 0;
    memset(c, 0, sizeof *c);
    if (!f)
        return -1;
    fseek(f, 0, SEEK_END);
    sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    b = (uint8_t *)malloc((size_t)sz + 1);
    if (fread(b, 1, (size_t)sz, f) != (size_t)sz) {
        fclose(f);
        free(b);
        return -1;
    }
    fclose(f);
    if (sz < 16 || memcmp(b, "PS3DKPRF", 8) != 0) {
        free(b);
        return sz == 0 ? 0 : -1;
    }
    while (pos + 20 <= (size_t)sz) {
        uint32_t type = r32(b + pos), len = r32(b + pos + 4), i, n;
        const uint8_t *pl = b + pos + 16, *q;
        if (pos + 20 + len > (size_t)sz)
            break;   /* torn */
        if (crc32_rt(b + pos, 16 + len) != r32(pl + len)) {
            free(b);
            return -1;
        }
        switch (type) {
        case 3:
            c->ndesc += r32(pl);
            break;
        case 4:
            n = r32(pl);
            q = pl + 4;
            for (i = 0; i < n; i++) {
                if (c->nthreads < 2048) {
                    c->threads_inc[c->nthreads] = r32(q);
                    c->threads_slot[c->nthreads] = r32(q + 4);
                    c->nthreads++;
                }
                q += 16;
                q += 2 + r16(q);
            }
            break;
        case 5:
            n = r32(pl + 4);
            for (i = 0; i < n; i++) {
                const uint8_t *r = pl + 8 + (size_t)i * 32;
                rt_rec *x;
                if (c->nrecs == cap) {
                    cap = cap ? cap * 2 : 1024;
                    c->recs = (rt_rec *)realloc(c->recs, cap * sizeof *c->recs);
                }
                x = &c->recs[c->nrecs++];
                x->inc = r32(pl);
                x->kind = r[0];
                x->depth = r[1];
                x->flags = r16(r + 2);
                x->a = r32(r + 4);
                x->seq = r64(r + 8);
                x->b = r64(r + 16);
                x->c = r64(r + 24);
                x->chunk = c->nrecchunks;
            }
            if (c->nrecchunks < 4096)
                c->recchunk_n[c->nrecchunks] = n;
            c->nrecchunks++;
            break;
        case 6:
            c->has_trailer = 1;
            c->end_flags = r32(pl);
            n = r32(pl + 4);
            for (i = 0; i < n; i++)
                final_at(c, pl + 8 + (size_t)i * 56, 0);
            q = pl + 8 + (size_t)n * 56;
            c->sink_error_count = r64(q);
            c->first_error = (int32_t)r32(q + 8);
            c->unreg = r64(q + 16);
            c->desc_count = r64(q + 24);
            break;
        case 7:
            n = r32(pl);
            for (i = 0; i < n; i++)
                final_at(c, pl + 4 + (size_t)i * 56, 1);
            c->nretired_chunks++;
            break;
        default:
            break;
        }
        pos += 20 + len;
    }
    free(b);
    return 0;
}

void rt_cap_free(rt_cap *c)
{
    free(c->recs);
    c->recs = NULL;
}

const rt_final *rt_final_of(const rt_cap *c, uint32_t inc)
{
    uint32_t i;
    for (i = 0; i < c->nfinal; i++)
        if (c->finals[i].inc == inc)
            return &c->finals[i];
    return NULL;
}

size_t rt_count(const rt_cap *c, uint32_t inc, int kind, int flags)
{
    size_t i, n = 0;
    for (i = 0; i < c->nrecs; i++) {
        const rt_rec *r = &c->recs[i];
        if ((inc == 0 || r->inc == inc) && r->kind == kind && (flags < 0 || r->flags == (uint16_t)flags))
            n++;
    }
    return n;
}

/* ---- worker failures and the step observer ------------------------------------------------------------------------ */

static int g_worker_fail;
int rt_worker_failed(void) { return ALD(g_worker_fail); }
void rt_worker_fail_set(void) { AST(g_worker_fail, 1); }

rt_step g_steps[64];
int g_nsteps;
static int g_steps_on;

static void step_observer(int step, uint64_t value)
{
    if (!ALD(g_steps_on) || g_nsteps >= 64)
        return;
    g_steps[g_nsteps].step = step;
    g_steps[g_nsteps].value = value;
    g_steps[g_nsteps].next_seq = ps3dk_prof_test_read(PS3DK_PROF_TEST_NEXT_SEQ);
    g_nsteps++;
}

void rt_steps_install(void) { ps3dk_prof_test_hooks.step_observer = step_observer; }
void rt_steps_on(void)
{
    g_nsteps = 0;
    AST(g_steps_on, 1);
}
void rt_steps_off(void) { AST(g_steps_on, 0); }
