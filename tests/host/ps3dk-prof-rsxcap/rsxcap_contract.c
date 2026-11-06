/* Capture RSX frame S2: the contract harness.  The runtime's tag formatter and emitter, its draw-kind table and its
 * descriptor pool against the FROZEN S1 fixture (tools/ps3dk-prof/tests/fixtures/rsx_capture_contract.txt), read
 * by the fixture header's own rules.  The fixture is never edited here.
 *
 * usage: rsxcap_contract <case> <fixture> <out-dir>
 *   formatter_records | draw_kinds | lengths | capacity_end_494 | capacity_end_482 | capacity_begin_121 |
 *   capacity_ckpt_99 | pool_unique */
#define _GNU_SOURCE
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <ps3dk/prof.h>

#include "prof_spu.h"
#include "rsxcap.h"

int ps3dk_prof__gpu_desc(void *p, ps3dk_prof_desc *d, uint32_t *idx, uint32_t *scope);
extern void *ps3dk_prof__g_prof;

static const char *g_case;
static int g_bad;
#define CHECK(c, ...)                                                                                                  \
    do {                                                                                                               \
        if (!(c)) {                                                                                                    \
            printf("CT %s FAIL line %d: ", g_case, __LINE__);                                                         \
            printf(__VA_ARGS__);                                                                                       \
            printf("\n");                                                                                              \
            g_bad++;                                                                                                   \
        }                                                                                                              \
    } while (0)

/* ---------------------------------------------------------------- the fixture */
typedef struct rec {
    char kind[8], id[64], marker[8], expect[16], name[64];
    char tag[600];
} rec;
static rec R[256];
static int NR;

static int unescape(const char *s, char *o, size_t cap)
{
    size_t n = 0;
    for (; *s; s++) {
        char c = *s;
        if (c == '\\') {
            s++;
            c = *s == 's' ? ' ' : *s == 't' ? '\t' : *s == '\\' ? '\\' : 0;
            if (!c)
                return -1;
        }
        if (n + 1 >= cap)
            return -1;
        o[n++] = c;
    }
    o[n] = 0;
    return 0;
}

/* the first n space-separated fields, then the verbatim rest */
static int load(const char *path)
{
    FILE *f = fopen(path, "rb");
    char line[1024];
    if (!f)
        return -1;
    while (fgets(line, sizeof line, f)) {
        size_t l = strlen(line);
        if (l && line[l - 1] == '\n')
            line[--l] = 0;
        if (l && line[l - 1] == '\r')
            line[--l] = 0;
        if (!l || line[0] == '#')
            continue;
        if (NR >= 256)
            return -1;
        rec *r = &R[NR++];
        memset(r, 0, sizeof *r);
        char *p = line, *sp;
        int nf = !strncmp(line, "TAG ", 4) ? 4 : !strncmp(line, "DRAW ", 5) ? 4 : !strncmp(line, "ACK ", 4) ? 3 : -1;
        if (nf < 0) {
            printf("CT %s FAIL unknown record kind: %s\n", g_case, line);
            g_bad++;
            continue;
        }
        char *fld[4] = {r->kind, r->id, NULL, NULL};
        if (line[0] == 'T') {
            fld[2] = r->marker;
            fld[3] = r->expect;
        } else if (line[0] == 'D') {
            fld[2] = r->expect;
            fld[3] = r->name;
        } else {
            fld[2] = r->expect;
        }
        for (int i = 0; i < nf; i++) {
            sp = strchr(p, ' ');
            if (!sp) { /* the last field may end the line (an empty tag) */
                if (i != nf - 1)
                    return -1;
                strncpy(fld[i], p, 63);
                p = p + strlen(p);
                break;
            }
            *sp = 0;
            strncpy(fld[i], p, 63);
            p = sp + 1;
        }
        if (unescape(p, r->tag, sizeof r->tag) != 0)
            return -1;
    }
    fclose(f);
    return 0;
}

/* ---------------------------------------------------------------- classification of TAG records */
static const char *K_BEGIN[] = {"cap", "conn", "wire", "dom", "owner", "frame"};
static const char *K_CKPT[] = {"cap", "block", "ev", "lost", "sup", "ref"};
static const char *K_END[] = {"cap", "conn", "wire", "dom", "owner", "frame", "block", "ev", "lost", "sup", "ref",
                              "hooks", "capped", "other_ctx", "other_ctx_sat", "other_thread", "other_thread_sat",
                              "immediate", "immediate_sat", "ckpt_missing", "unsafe", "no_space", "reentrant",
                              "invalid", "mismatch", "counts_sat", "hook_fault"};

/* 1 when the tag is a FORMATTER INPUT: the version token, exactly the marker's keys in order (plus the exact
 * coverage token for END), single spaces, each value 0|[1-9][0-9]* representable in its C type; v[] the values */
static int as_input(const char *marker, const char *tag, uint64_t *v, int *nv)
{
    const char **keys = !strcmp(marker, "begin") ? K_BEGIN : !strcmp(marker, "ckpt") ? K_CKPT : K_END;
    int nk = !strcmp(marker, "begin") ? 6 : !strcmp(marker, "ckpt") ? 6 : 27, is_end = !strcmp(marker, "end");
    char t[600], *tok[64];
    int n = 0;
    strcpy(t, tag);
    for (char *p = t;;) { /* split on single spaces: an empty token is structural */
        char *sp = strchr(p, ' ');
        if (n >= 64)
            return 0;
        tok[n++] = p;
        if (!sp)
            break;
        *sp = 0;
        p = sp + 1;
    }
    if (n != 1 + nk + is_end || strcmp(tok[0], "ps3dk.rsxcap.v1"))
        return 0;
    if (is_end && strcmp(tok[n - 1], "coverage=librsx-draws-v1"))
        return 0;
    for (int i = 0; i < nk; i++) {
        size_t kl = strlen(keys[i]);
        const char *s = tok[1 + i];
        if (strncmp(s, keys[i], kl) || s[kl] != '=')
            return 0;
        s += kl + 1;
        size_t dl = strlen(s);
        if (!dl || (dl > 1 && s[0] == '0'))
            return 0;
        for (size_t j = 0; j < dl; j++)
            if (s[j] < '0' || s[j] > '9')
                return 0;
        int is_frame = !strcmp(keys[i], "frame");
        if (dl > 20)
            return 0;
        unsigned long long x = 0;
        for (size_t j = 0; j < dl; j++) {
            unsigned d = (unsigned)(s[j] - '0');
            if (x > (UINT64_MAX - d) / 10u)
                return 0; /* not representable in u64 */
            x = x * 10u + d;
        }
        if (!is_frame && x > UINT32_MAX)
            return 0;
        v[i] = x;
    }
    *nv = nk;
    return 1;
}

static int format_input(const char *marker, const uint64_t *v, char *buf, size_t cap)
{
    if (!strcmp(marker, "begin")) {
        rsxcap_ident id = {(uint32_t)v[0], (uint32_t)v[1], (uint32_t)v[2], (uint32_t)v[3], (uint32_t)v[4], 0, v[5]};
        return rsxcap_fmt_begin(buf, cap, &id);
    }
    if (!strcmp(marker, "ckpt")) {
        rsxcap_blk c = {(uint32_t)v[2], (uint32_t)v[3], (uint32_t)v[4], (uint32_t)v[5]};
        return rsxcap_fmt_ckpt(buf, cap, (uint32_t)v[0], (uint32_t)v[1], &c);
    }
    rsxcap_end e;
    memset(&e, 0, sizeof e);
    e.id = (rsxcap_ident){(uint32_t)v[0], (uint32_t)v[1], (uint32_t)v[2], (uint32_t)v[3], (uint32_t)v[4], 0, v[5]};
    e.block = (uint32_t)v[6];
    e.c = (rsxcap_blk){(uint32_t)v[7], (uint32_t)v[8], (uint32_t)v[9], (uint32_t)v[10]};
    uint32_t *f = &e.hooks; /* hooks .. hook_fault, in the grammar's order */
    for (int i = 0; i < 16; i++)
        f[i] = (uint32_t)v[11 + i];
    return rsxcap_fmt_end(buf, cap, &e);
}

static const char *STRUCTURAL[] = {
    "begin-version-v2", "begin-no-version", "begin-other-namespace", "begin-order", "begin-extra-key",
    "begin-missing-key", "begin-leading-zero", "begin-zero-zero", "begin-plus-sign", "begin-minus-sign", "begin-hex",
    "begin-empty-value", "begin-u32-overflow", "begin-u64-overflow", "begin-21-digits", "begin-double-space",
    "begin-trailing-space", "begin-leading-space", "begin-tab", "begin-upper-key", "begin-ckpt-tag", "begin-empty",
    "ckpt-end-tag", "ckpt-u32-overflow", "ckpt-extra-key", "end-coverage-old", "end-no-coverage", "end-order",
    "end-begin-tag"};

static void c_formatter_records(void)
{
    /* [marker][expect: ok value grammar][input / structural] */
    int tally[3][3][2];
    memset(tally, 0, sizeof tally);
    int ntag = 0, nstruct = 0;
    for (int i = 0; i < NR; i++) {
        rec *r = &R[i];
        if (strcmp(r->kind, "TAG"))
            continue;
        ntag++;
        int m = !strcmp(r->marker, "begin") ? 0 : !strcmp(r->marker, "ckpt") ? 1 : !strcmp(r->marker, "end") ? 2 : -1;
        int x = !strcmp(r->expect, "ok") ? 0 : !strcmp(r->expect, "value") ? 1 : !strcmp(r->expect, "grammar") ? 2 : -1;
        CHECK(m >= 0 && x >= 0, "%s: unknown marker/expect", r->id);
        if (m < 0 || x < 0)
            continue;
        uint64_t v[32];
        int nv = 0;
        int in = as_input(r->marker, r->tag, v, &nv);
        tally[m][x][in ? 0 : 1]++;
        if (!in) {
            int listed = 0;
            for (size_t k = 0; k < sizeof STRUCTURAL / sizeof *STRUCTURAL; k++)
                listed += !strcmp(STRUCTURAL[k], r->id);
            CHECK(listed == 1, "%s: structural but not in the pinned id list", r->id);
            nstruct++;
            continue;
        }
        char buf[600];
        memset(buf, 0x5A, sizeof buf);
        int n = format_input(r->marker, v, buf, sizeof buf);
        if (x == 0) {
            CHECK(n == (int)strlen(r->tag) && !strcmp(buf, r->tag), "%s: ok -> byte-identical (got %d)", r->id, n);
        } else {
            int untouched = 1;
            for (size_t k = 0; k < sizeof buf; k++)
                untouched &= (unsigned char)buf[k] == 0x5A;
            CHECK(n < 0 && untouched, "%s: %s -> refused, nothing written (rc %d)", r->id, r->expect, n);
        }
    }
    const int want[3][4] = {{4, 3, 0, 22}, {3, 4, 0, 3}, {3, 7, 11, 4}};
    for (int m = 0; m < 3; m++) {
        CHECK(tally[m][0][0] == want[m][0] && tally[m][1][0] == want[m][1] && tally[m][2][0] == want[m][2] &&
                  tally[m][2][1] == want[m][3] && tally[m][0][1] == 0 && tally[m][1][1] == 0,
              "tally marker %d: ok %d value %d grammar-input %d grammar-structural %d (ok-structural %d "
              "value-structural %d)",
              m, tally[m][0][0], tally[m][1][0], tally[m][2][0], tally[m][2][1], tally[m][0][1], tally[m][1][1]);
    }
    CHECK(ntag == 64 && nstruct == 29, "64 TAG records (%d), 29 structural (%d)", ntag, nstruct);
    printf("tally: 35 inputs, %d structural\n", nstruct);
}

static void c_draw_kinds(void)
{
    uint32_t nk;
    const ps3dk_prof_desc *k = ps3dk_prof__rsxcap_kinds(&nk);
    int ndraw = 0, matched[8] = {0};
    CHECK(nk == 5, "5 kinds");
    for (int i = 0; i < NR; i++) {
        rec *r = &R[i];
        if (strcmp(r->kind, "DRAW"))
            continue;
        int hit = -1;
        for (uint32_t j = 0; j < nk; j++)
            if (!strcmp(k[j].name, r->name) && !strcmp(k[j].logical_tag, r->tag) && k[j].scope == 2u)
                hit = (int)j;
        if (!strcmp(r->expect, "draw")) {
            ndraw++;
            CHECK(hit >= 0, "%s: the runtime has (%s, %s, 2)", r->id, r->name, r->tag);
            if (hit >= 0)
                matched[hit]++;
        } else {
            CHECK(hit < 0, "%s (%s): no runtime entry equals it", r->id, r->expect);
        }
    }
    CHECK(ndraw == 5, "5 draw records");
    for (uint32_t j = 0; j < nk; j++)
        CHECK(matched[j] == 1, "kind %u matched exactly once (set compare)", j);
}

static const rec *by_id(const char *id)
{
    for (int i = 0; i < NR; i++)
        if (!strcmp(R[i].id, id))
            return &R[i];
    return NULL;
}

/* the emitter's output for a fixture record (no validation), into `cap` bytes */
static int emit_rec(const rec *r, char *buf, size_t cap)
{
    uint64_t v[32];
    int nv;
    if (!as_input(r->marker, r->tag, v, &nv))
        return -100;
    if (!strcmp(r->marker, "begin")) {
        rsxcap_ident id = {(uint32_t)v[0], (uint32_t)v[1], (uint32_t)v[2], (uint32_t)v[3], (uint32_t)v[4], 0, v[5]};
        return rsxcap_emit_begin(buf, cap, &id);
    }
    if (!strcmp(r->marker, "ckpt")) {
        rsxcap_blk c = {(uint32_t)v[2], (uint32_t)v[3], (uint32_t)v[4], (uint32_t)v[5]};
        return rsxcap_emit_ckpt(buf, cap, (uint32_t)v[0], (uint32_t)v[1], &c);
    }
    rsxcap_end e;
    memset(&e, 0, sizeof e);
    e.id = (rsxcap_ident){(uint32_t)v[0], (uint32_t)v[1], (uint32_t)v[2], (uint32_t)v[3], (uint32_t)v[4], 0, v[5]};
    e.block = (uint32_t)v[6];
    e.c = (rsxcap_blk){(uint32_t)v[7], (uint32_t)v[8], (uint32_t)v[9], (uint32_t)v[10]};
    uint32_t *f = &e.hooks;
    for (int i = 0; i < 16; i++)
        f[i] = (uint32_t)v[11 + i];
    return rsxcap_emit_end(buf, cap, &e);
}

static void c_lengths(void)
{
    char buf[RSXCAP_END_BYTES];
    const struct {
        const char *id;
        size_t len, storage;
        int formatter_ok;
    } L[] = {{"begin-max", 121, RSXCAP_BEGIN_BYTES, 1},     {"ckpt-max-valid", 99, RSXCAP_CKPT_BYTES, 1},
             {"end-max-valid", 482, RSXCAP_END_BYTES, 1},   {"ckpt-max-width", 107, RSXCAP_CKPT_BYTES, 0},
             {"end-max-width", 490, RSXCAP_END_BYTES, 0}};
    CHECK(RSXCAP_BEGIN_BYTES == 128 && RSXCAP_END_BYTES == 496 && RSXCAP_CKPT_BYTES == 112, "storage sizes");
    for (size_t i = 0; i < sizeof L / sizeof *L; i++) {
        const rec *r = by_id(L[i].id);
        CHECK(r && strlen(r->tag) == L[i].len, "%s: %zu bytes", L[i].id, L[i].len);
        if (!r)
            continue;
        int n = emit_rec(r, buf, L[i].storage);
        CHECK(n == (int)L[i].len && !strcmp(buf, r->tag), "%s: the emitter writes it into its %zu storage", L[i].id,
              L[i].storage);
        uint64_t v[32];
        int nv;
        CHECK(as_input(r->marker, r->tag, v, &nv), "input");
        int f = format_input(r->marker, v, buf, L[i].storage);
        CHECK((f > 0) == L[i].formatter_ok, "%s: the validator %s it", L[i].id, L[i].formatter_ok ? "accepts" : "refuses");
    }
    const rec *r = by_id("end-hooks-10-digits");
    CHECK(r && strlen(r->tag) == 494 && emit_rec(r, buf, RSXCAP_END_BYTES) == 494, "494 through the emitter into 496");
}

static void c_capacity(const char *id, size_t len)
{
    char buf[RSXCAP_END_BYTES + 32];
    const rec *r = by_id(id);
    CHECK(r && strlen(r->tag) == len, "%s is %zu bytes", id, len);
    if (!r)
        return;
    memset(buf, 0x5A, sizeof buf);
    int n = emit_rec(r, buf, len + 1);
    CHECK(n == (int)len && !memcmp(buf, r->tag, len) && buf[len] == 0, "capacity %zu: the fixture bytes and a NUL",
          len + 1);
    for (size_t k = len + 1; k < sizeof buf; k++)
        CHECK((unsigned char)buf[k] == 0x5A, "canary %zu intact", k);
    memset(buf, 0x5A, sizeof buf);
    n = emit_rec(r, buf, len);
    int untouched = 1;
    for (size_t k = 0; k < sizeof buf; k++)
        untouched &= (unsigned char)buf[k] == 0x5A;
    CHECK(n == RSXCAP_FMT_ECAP && untouched, "capacity %zu: refused, nothing written (rc %d)", len, n);
}

/* every pool entry through the production registration path: distinct indices and identities */
static void c_pool_unique(const char *out)
{
    ps3dk_prof_config c;
    ps3dk_prof_config_defaults(&c);
    c.mode = PS3DK_PROF_MODE_APP;
    c.app = "rsxcap";
    c.build_id = "rsxcap";
    c.max_descriptors = 5 + RSXCAP_POOL;
    ps3dk_prof_ext_config x;
    memset(&x, 0, sizeof x);
    x.size = PS3DK_PROF_EXT_CONFIG_SIZE;
    x.gpu_domains_max = 1;
    x.gpu_report_slots_max = 16;
    CHECK(ps3dk_prof_set_sink_path(out) == 0 && ps3dk_prof_init_ex(&c, &x) == 0, "init");
    CHECK(ps3dk_prof__rsxcap_enable() == 0, "enable");
    CHECK(ps3dk_prof_thread_begin("render") == 0, "thread");
    uint32_t n;
    ps3dk_prof_desc *pool = (ps3dk_prof_desc *)ps3dk_prof__rsxcap_pool(&n);
    CHECK(n == 1056, "1056 entries");
    static uint32_t idx[1056];
    for (uint32_t i = 0; i < n; i++) {
        uint32_t scope = 0;
        CHECK(ps3dk_prof__gpu_desc(ps3dk_prof__g_prof, &pool[i], &idx[i], &scope) == 0 && scope == 2,
              "entry %u registers", i);
    }
    int dup = 0, ident = 0, tagdup = 0;
    for (uint32_t i = 0; i < n; i++)
        for (uint32_t j = i + 1; j < n; j++) {
            dup += idx[i] == idx[j];
            ident += pool[i].line == pool[j].line && !strcmp(pool[i].file, pool[j].file) &&
                     !strcmp(pool[i].name, pool[j].name);
            tagdup += pool[i].logical_tag == pool[j].logical_tag;
        }
    CHECK(dup == 0 && ident == 0 && tagdup == 0, "1056 distinct indices (%d dup), identities (%d), tags (%d)", dup,
          ident, tagdup);
    ps3dk_prof_thread_end();
    CHECK(ps3dk_prof_shutdown() == 0, "shutdown");
}

int main(int argc, char **argv)
{
    if (argc < 4)
        return 2;
    g_case = argv[1];
    if (load(argv[2]) != 0) {
        printf("CT %s FAIL the fixture could not be read\n", g_case);
        return 1;
    }
    if (!strcmp(g_case, "formatter_records"))
        c_formatter_records();
    else if (!strcmp(g_case, "draw_kinds"))
        c_draw_kinds();
    else if (!strcmp(g_case, "lengths"))
        c_lengths();
    else if (!strcmp(g_case, "capacity_end_494"))
        c_capacity("end-hooks-10-digits", 494);
    else if (!strcmp(g_case, "capacity_end_482"))
        c_capacity("end-max-valid", 482);
    else if (!strcmp(g_case, "capacity_begin_121"))
        c_capacity("begin-max", 121);
    else if (!strcmp(g_case, "capacity_ckpt_99"))
        c_capacity("ckpt-max-valid", 99);
    else if (!strcmp(g_case, "pool_unique"))
        c_pool_unique(argv[3]);
    else {
        printf("CT %s FAIL unknown case\n", g_case);
        return 2;
    }
    if (g_bad)
        return 1;
    printf("CT %s PASS\n", g_case);
    return 0;
}
