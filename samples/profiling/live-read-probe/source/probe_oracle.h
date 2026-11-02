/* The live-read probe's reading and verdict, with the file and clock passed
 * in, so the host tests can drive them with fakes. */
#ifndef LIVE_READ_PROBE_ORACLE_H
#define LIVE_READ_PROBE_ORACLE_H
#include <stdint.h>
#include <string.h>
#include <sys/types.h>

#define PROBE_MAX_OBS 512

/* Reads up to n bytes at off; returns bytes read, 0 at EOF, -errno-like < 0. */
typedef ssize_t (*probe_read_fn)(void *ctx, void *buf, size_t n, uint64_t off);

struct probe_obs {
    uint64_t t_us;
    uint64_t size;
    int eligible; /* the read completed while observation was still eligible (before shutdown began) */
};

/* Bytes read so far into a caller buffer of cap + 1 bytes: one spare byte
 * detects "more than cap" without silently stopping at the cap. */
struct probe_buf {
    unsigned char *buf;
    uint64_t cap, len;
    int cap_hit; /* more than cap bytes exist */
    int err;     /* the first read error (nonzero) */
};

/* Reads every byte available now; returns 1 at EOF (0 if it stopped on the
 * cap or an error, which are latched). */
static inline int probe_read_available(struct probe_buf *b, probe_read_fn rd, void *ctx)
{
    while (!b->err && !b->cap_hit) {
        uint64_t room = b->cap + 1 - b->len; /* the spare byte included */
        size_t want = room > 65536 ? 65536 : (size_t)room;
        ssize_t r = rd(ctx, b->buf + b->len, want, b->len);
        if (r < 0) {
            b->err = (int)(r == -1 ? -1 : r);
            return 0;
        }
        if (r == 0)
            return 1;
        b->len += (uint64_t)r;
        if (b->len > b->cap) {
            b->cap_hit = 1;
            return 0;
        }
    }
    return 0;
}

struct probe_reader {
    struct probe_buf kept;
    struct probe_obs obs[PROBE_MAX_OBS];
    int nobs;
    int obs_overflow;
};

/* Records the size read so far. `eligible` must be sampled AFTER the read
 * that produced this size completed: the caller reads, then samples the
 * flag, then notes (never reads again in between). */
static inline void probe_note(struct probe_reader *r, uint64_t now_us, int eligible)
{
    if (r->nobs > 0 && r->obs[r->nobs - 1].size == r->kept.len)
        return;
    if (r->nobs == PROBE_MAX_OBS) {
        r->obs_overflow = 1;
        return;
    }
    r->obs[r->nobs++] = (struct probe_obs){now_us, r->kept.len, eligible};
}

/* Distinct growing sizes seen while observation was eligible. */
static inline int probe_growth_eligible(const struct probe_reader *r)
{
    int n = 0;
    uint64_t last = 0;
    for (int i = 0; i < r->nobs; ++i)
        if (r->obs[i].eligible && r->obs[i].size > last) {
            ++n;
            last = r->obs[i].size;
        }
    return n;
}

struct probe_verdict {
    int pass;
    const char *why; /* the first failed condition, or "ok" */
};

/* final: the fresh read of the closed file (EOF required). */
static inline struct probe_verdict probe_judge(const struct probe_reader *r, const struct probe_buf *final,
                                               int final_eof, int shutdown_rc, int drain_failures,
                                               int reader_joined)
{
    struct probe_verdict v = {0, "ok"};
    if (!reader_joined)
        v.why = "reader thread not joined";
    else if (shutdown_rc != 0)
        v.why = "profiler shutdown failed";
    else if (drain_failures)
        v.why = "a drain failed";
    else if (r->kept.err)
        v.why = "reader read error";
    else if (r->kept.cap_hit)
        v.why = "reader buffer cap exceeded";
    else if (r->obs_overflow)
        v.why = "too many observations";
    else if (final->err)
        v.why = "final read error";
    else if (final->cap_hit)
        v.why = "final read cap exceeded";
    else if (!final_eof)
        v.why = "final read did not reach EOF";
    else if (final->len == 0)
        v.why = "final file empty";
    else if (r->kept.len != final->len)
        v.why = "reader did not reach the final length";
    else if (memcmp(r->kept.buf, final->buf, (size_t)final->len) != 0)
        v.why = "reader bytes differ from the final file";
    else if (probe_growth_eligible(r) < 2)
        v.why = "fewer than two growing sizes before shutdown began";
    else
        v.pass = 1;
    return v;
}

#endif
