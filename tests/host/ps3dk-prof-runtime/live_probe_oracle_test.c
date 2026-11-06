/* The live-read probe's oracle (samples/profiling/live-read-probe/source/
 * probe_oracle.h) with a fake file: growth seen before and after shutdown
 * began, the buffer cap on both reads, read errors, EOF and byte mismatch. */
#include "probe_oracle.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(c)                                                                   \
    do {                                                                           \
        if (!(c)) {                                                                \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);           \
            exit(1);                                                               \
        }                                                                          \
    } while (0)

#define CAP 100u

/* A file of `len` bytes (byte i = i * 7 + 1), with an optional error. */
struct fake {
    uint64_t len;
    int fail_at_off; /* -1: never */
    unsigned char flip; /* XOR applied to every byte (a different file) */
};

static ssize_t fake_read(void *ctx, void *buf, size_t n, uint64_t off)
{
    struct fake *f = ctx;
    if (f->fail_at_off >= 0 && off >= (uint64_t)f->fail_at_off)
        return -5;
    if (off >= f->len)
        return 0;
    size_t k = (size_t)(f->len - off) < n ? (size_t)(f->len - off) : n;
    for (size_t i = 0; i < k; ++i)
        ((unsigned char *)buf)[i] = (unsigned char)((off + i) * 7 + 1) ^ f->flip;
    return (ssize_t)k;
}

static unsigned char kept_buf[CAP + 1], final_buf[CAP + 1];

static struct probe_reader reader(void)
{
    struct probe_reader r;
    memset(&r, 0, sizeof r);
    r.kept = (struct probe_buf){.buf = kept_buf, .cap = CAP};
    return r;
}

/* Grows the file through `sizes`, reading at each with the eligibility given. */
static void grow(struct probe_reader *r, struct fake *f, const uint64_t *sizes, const int *eligible, int n)
{
    for (int i = 0; i < n; ++i) {
        f->len = sizes[i];
        probe_read_available(&r->kept, fake_read, f);
        probe_note(r, (uint64_t)i * 100000, eligible[i]);
    }
}

static struct probe_verdict judge_final(struct probe_reader *r, struct fake *final_file)
{
    struct probe_buf fin = {.buf = final_buf, .cap = CAP};
    int eof = probe_read_available(&fin, fake_read, final_file);
    return probe_judge(r, &fin, eof, 0, 0, 1);
}

static void expect(struct probe_verdict v, int pass, const char *why)
{
    if (v.pass != pass || strcmp(v.why, why) != 0) {
        fprintf(stderr, "FAIL expected %d '%s', got %d '%s'\n", pass, why, v.pass, v.why);
        exit(1);
    }
}

int main(void)
{
    const uint64_t s3[] = {20, 40, 60};
    const int open3[] = {1, 1, 1};
    struct fake f = {0, -1, 0};

    /* Growth seen while eligible, full equality at EOF: PASS. */
    {
        struct probe_reader r = reader();
        f = (struct fake){0, -1, 0};
        grow(&r, &f, s3, open3, 3);
        f.len = 80; /* the rest, after close */
        probe_read_available(&r.kept, fake_read, &f);
        probe_note(&r, 400000, 0);
        CHECK(probe_growth_eligible(&r) == 3);
        expect(judge_final(&r, &f), 1, "ok");
    }
    /* Growth only after eligibility was cleared: not seen while open. */
    {
        struct probe_reader r = reader();
        const int late[] = {1, 0, 0};
        const uint64_t s[] = {0, 40, 60};
        f = (struct fake){0, -1, 0};
        grow(&r, &f, s, late, 3);
        expect(judge_final(&r, &f), 0, "fewer than two growing sizes before shutdown began");
    }
    /* One eligible growth only. */
    {
        struct probe_reader r = reader();
        const int one[] = {1, 0, 0};
        f = (struct fake){0, -1, 0};
        grow(&r, &f, s3, one, 3);
        expect(judge_final(&r, &f), 0, "fewer than two growing sizes before shutdown began");
    }
    /* Exactly the cap is fine; one byte more is detected on both reads. */
    {
        struct probe_reader r = reader();
        const uint64_t s[] = {50, CAP};
        f = (struct fake){0, -1, 0};
        grow(&r, &f, s, open3, 2);
        CHECK(!r.kept.cap_hit);
        expect(judge_final(&r, &f), 1, "ok");
    }
    {
        struct probe_reader r = reader();
        const uint64_t s[] = {50, CAP + 1};
        f = (struct fake){0, -1, 0};
        grow(&r, &f, s, open3, 2);
        CHECK(r.kept.cap_hit);
        expect(judge_final(&r, &f), 0, "reader buffer cap exceeded");
    }
    {
        /* The final read alone over the cap (the reader stopped short). */
        struct probe_reader r = reader();
        f = (struct fake){0, -1, 0};
        grow(&r, &f, s3, open3, 3);
        struct fake big = {CAP + 50, -1, 0};
        expect(judge_final(&r, &big), 0, "final read cap exceeded");
    }
    /* Read errors latch, on either side. */
    {
        struct probe_reader r = reader();
        f = (struct fake){0, 30, 0};
        grow(&r, &f, s3, open3, 3);
        CHECK(r.kept.err != 0);
        struct fake ok = {60, -1, 0};
        expect(judge_final(&r, &ok), 0, "reader read error");
    }
    {
        struct probe_reader r = reader();
        f = (struct fake){0, -1, 0};
        grow(&r, &f, s3, open3, 3);
        struct fake bad = {60, 10, 0};
        expect(judge_final(&r, &bad), 0, "final read error");
    }
    /* The reader stopped short, or read other bytes. */
    {
        struct probe_reader r = reader();
        f = (struct fake){0, -1, 0};
        grow(&r, &f, s3, open3, 3);
        struct fake longer = {70, -1, 0};
        expect(judge_final(&r, &longer), 0, "reader did not reach the final length");
        struct fake other = {60, -1, 0x55};
        expect(judge_final(&r, &other), 0, "reader bytes differ from the final file");
    }
    /* Shutdown, drain, join and observation-count failures. */
    {
        struct probe_reader r = reader();
        f = (struct fake){0, -1, 0};
        grow(&r, &f, s3, open3, 3);
        struct probe_buf fin = {.buf = final_buf, .cap = CAP};
        int eof = probe_read_available(&fin, fake_read, &f);
        expect(probe_judge(&r, &fin, eof, -10, 0, 1), 0, "profiler shutdown failed");
        expect(probe_judge(&r, &fin, eof, 0, 2, 1), 0, "a drain failed");
        expect(probe_judge(&r, &fin, eof, 0, 0, 0), 0, "reader thread not joined");
        expect(probe_judge(&r, &fin, 0, 0, 0, 1), 0, "final read did not reach EOF");
        struct probe_buf empty = {.buf = final_buf, .cap = CAP};
        struct probe_reader r0 = reader();
        expect(probe_judge(&r0, &empty, 1, 0, 0, 1), 0, "final file empty");
    }
    {
        /* More distinct sizes than the observation log holds. */
        struct probe_reader r = reader();
        for (int i = 0; i <= PROBE_MAX_OBS; ++i) {
            r.kept.len = (uint64_t)i + 1;
            probe_note(&r, (uint64_t)i, 1);
        }
        CHECK(r.obs_overflow && r.nobs == PROBE_MAX_OBS);
        struct probe_buf fin = {.buf = final_buf, .cap = CAP};
        expect(probe_judge(&r, &fin, 1, 0, 0, 1), 0, "too many observations");
    }
    puts("live-probe-oracle: PASS");
    return 0;
}
