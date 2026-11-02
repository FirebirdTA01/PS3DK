/* P3.2 I3b host harness: the SPU ELF parser and image registration (design I3b r2).  Host logic only.
 *
 *   i3b parse LIST       for each ELF path in LIST: the parser's verdict in i3b_elf.py's oracle format
 *   i3b register LIST    the same, but each accepted image goes through psp_image_add_src (the staged copy) into a
 *                        bare psp_ppu and the STORED image is printed (it must equal the parse output)
 *   i3b table            the table path through the new wrapper vs the committed psp_image_add (linked in with its
 *                        symbols renamed old_*): identical arena bytes, image fields and results, 8 tables
 *   i3b api ELF OUTDIR   runtime cases on one valid fixture: lock-free refusals, late getter failure at the first,
 *                        a middle and the last record of the copy pass then a retry, the post-parse shutdown race,
 *                        sink-callback reentry, and the end-to-end capture: its SPU_IMAGES entry printed in the
 *                        checker format (prefix "e2e") */
#define _GNU_SOURCE
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <ps3dk/prof.h>

#include "prof_spu.h"
#include "prof_test_hooks.h"
#include "psp_elf.h"
#include "psp_ppu.h"

static int g_bad;
#define CHECK(c, ...)                                                                                                  \
    do {                                                                                                               \
        if (!(c)) {                                                                                                    \
            printf("I3B FAIL line %d: ", __LINE__);                                                                   \
            printf(__VA_ARGS__);                                                                                       \
            printf("\n");                                                                                              \
            g_bad++;                                                                                                   \
        }                                                                                                              \
    } while (0)

static unsigned char *slurp(const char *path, size_t *n)
{
    FILE *f = fopen(path, "rb");
    unsigned char *b;
    long len;
    if (!f)
        return NULL;
    fseek(f, 0, SEEK_END);
    len = ftell(f);
    fseek(f, 0, SEEK_SET);
    b = malloc(len > 0 ? (size_t)len : 1);
    if (len > 0 && fread(b, 1, (size_t)len, f) != (size_t)len) {
        fclose(f);
        free(b);
        return NULL;
    }
    fclose(f);
    *n = (size_t)(len > 0 ? len : 0);
    return b;
}

static void hex(const void *p, size_t n)
{
    for (size_t i = 0; i < n; i++)
        printf("%02x", ((const unsigned char *)p)[i]);
}

static const char *base(const char *path)
{
    const char *s = strrchr(path, '/');
    return s ? s + 1 : path;
}

static void name_of(const char *path, char *out, size_t cap)
{
    snprintf(out, cap, "%s", base(path));
    char *dot = strstr(out, ".elf");
    if (dot)
        *dot = 0;
}

static void print_image(const char *name, const uint8_t *bid, uint32_t blen, const psp_desc *d, uint32_t n)
{
    printf("%s OK bid=", name);
    hex(bid, blen);
    printf(" n=%u\n", n);
    for (uint32_t i = 0; i < n; i++) {
        printf("%s R %u %u %u ", name, i, d[i].ls_addr, d[i].line);
        hex(d[i].name, strlen(d[i].name));
        printf(" ");
        hex(d[i].file, strlen(d[i].file));
        printf("\n");
    }
}

/* ---- a bare psp_ppu for the register/table modes ---- */
static psp_limits limits(void)
{
    psp_limits l = {64, 64, 32, 256, 65536, 65536, 262144};
    return l;
}
static int bare_init(psp_ppu *p, uint8_t *arena, size_t arena_len)
{
    static uint8_t scratch[PSP_SCRATCH_BYTES], copy[8 * 64];
    static psp_mem m;
    static psp_out o;
    psp_cfg cfg = {64, 1, 1u << 20, 1024};
    psp_limits l = limits();
    psp_storage st = {scratch, sizeof scratch, copy, sizeof copy, arena, arena_len};
    return psp_ppu_init(p, &cfg, &l, &m, &o, &st);
}

static int each(const char *list, int reg)
{
    FILE *f = fopen(list, "r");
    char path[4096], name[4096];
    static uint8_t arena[1u << 22];
    if (!f)
        return 2;
    while (fgets(path, sizeof path, f)) {
        size_t n = 0;
        unsigned char *b;
        psp_elf_view v;
        path[strcspn(path, "\r\n")] = 0;
        if (!*path)
            continue;
        name_of(path, name, sizeof name);
        b = slurp(path, &n);
        if (!b) {
            printf("%s UNREADABLE\n", name);
            continue;
        }
        if (psp_elf_parse(b, n, &v) != 0) {
            printf("%s EINVAL\n", name);
        } else if (!reg) {
            printf("%s OK bid=", name);
            hex(v.build_id, v.build_len);
            printf(" n=%u\n", v.n);
            for (uint32_t i = 0; i < v.n; i++) {
                psp_desc_rec r;
                if (psp_elf_get(&v, i, &r) != 0) {
                    printf("%s GETFAIL %u\n", name, i);
                    break;
                }
                printf("%s R %u %u %u ", name, i, r.ls_addr, r.line);
                hex(r.name, r.name_len);
                printf(" ");
                hex(r.file, r.file_len);
                printf("\n");
            }
        } else {
            psp_ppu p;
            psp_desc_src src = {&v, psp_elf_get};
            uint32_t idx = 77;
            int rc;
            memset(arena, 0xA5, sizeof arena);
            if (bare_init(&p, arena, sizeof arena) != 0) {
                printf("%s INITFAIL\n", name);
            } else if ((rc = psp_image_add_src(&p, v.build_id, v.build_len, &src, v.n, &idx)) != 0) {
                printf("%s REGFAIL %d\n", name, rc);
            } else {
                psp_image *im = &p.images[idx];
                print_image(name, im->build_id, im->build_len, im->descs, im->ndesc);
            }
        }
        free(b);
    }
    fclose(f);
    return 0;
}

/* ---- table: new wrapper vs the committed function (renamed old_*) ---- */
int old_ps3dk_prof__psp_ppu_init(psp_ppu *p, const psp_cfg *cfg, const psp_limits *lim, const psp_mem *mem,
                                 const psp_out *out, const psp_storage *st);
int old_ps3dk_prof__psp_image_add(psp_ppu *p, const uint8_t *build_id, uint32_t build_len, const psp_desc *d,
                                  uint32_t n, uint32_t *index);
static int table_mode(void)
{
    static uint8_t a1[1u << 22], a2[1u << 22];
    static char big[600], names[4096][16];
    static psp_desc many[4096];
    psp_desc t0[] = {{0x100, 1, "a", "f.c"}, {0x110, 2, "b", NULL}, {0x120, 3, "c", ""}};
    psp_desc t1[] = {{0, 0, big, big}};
    psp_desc t2[] = {{0, 0, "", "x"}};          /* EINVAL: empty name */
    psp_desc t3[] = {{0, 0, "a\x80", "x"}};     /* EINVAL: byte >= 0x80 */
    psp_desc t4[] = {{0, 0, "ctl\x01ok", "x"}}; /* accepted by the table policy (0x01..0x7F) */
    struct {
        const psp_desc *d;
        uint32_t n;
        uint32_t blen;
    } cases[] = {{t0, 3, 3}, {t1, 1, 64}, {t2, 1, 2}, {t3, 1, 2}, {t4, 1, 1}, {many, 4096, 5}, {t0, 3, 0}, {t0, 3, 65}};
    memset(big, 'b', 512);
    for (int i = 0; i < 4096; i++) {
        snprintf(names[i], sizeof names[i], "z%u", (unsigned)i);
        many[i] = (psp_desc){(uint32_t)(16 * i), (uint32_t)i, names[i], "m.c"};
    }
    for (unsigned k = 0; k < sizeof cases / sizeof cases[0]; k++) {
        psp_ppu p1, p2;
        psp_cfg cfg = {64, 1, 1u << 20, 1024};
        psp_limits l = limits();
        static uint8_t s1[PSP_SCRATCH_BYTES], s2[PSP_SCRATCH_BYTES], c1[512], c2[512];
        psp_mem m = {0};
        psp_out o = {0};
        psp_storage st1 = {s1, sizeof s1, c1, sizeof c1, a1, sizeof a1}, st2 = {s2, sizeof s2, c2, sizeof c2, a2, sizeof a2};
        uint32_t i1 = 9, i2 = 9;
        uint8_t bid[65];
        int r1, r2;
        memset(bid, 0x3c, sizeof bid);
        memset(a1, 0x5a, sizeof a1);
        memset(a2, 0x5a, sizeof a2);
        CHECK(psp_ppu_init(&p1, &cfg, &l, &m, &o, &st1) == 0 && old_ps3dk_prof__psp_ppu_init(&p2, &cfg, &l, &m, &o, &st2) == 0,
              "init");
        r1 = psp_image_add(&p1, bid, cases[k].blen, cases[k].d, cases[k].n, &i1);
        r2 = old_ps3dk_prof__psp_image_add(&p2, bid, cases[k].blen, cases[k].d, cases[k].n, &i2);
        CHECK(r1 == r2 && i1 == i2, "case %u: rc %d vs old %d, index %u vs %u", k, r1, r2, i1, i2);
        CHECK(p1.arena_used == p2.arena_used && p1.nimages == p2.nimages, "case %u: arena_used %zu vs %zu", k,
              p1.arena_used, p2.arena_used);
        if (r1 != 0)
            CHECK(!memcmp(a1, a2, sizeof a1), "case %u: a refused registration changed the arena", k);
        if (r1 == 0) {
            psp_image *m1 = &p1.images[i1], *m2 = &p2.images[i2];
            /* every arena byte except the descriptors' absolute name/file pointers (compared as offsets below):
             * the bytes before the descriptor array, and the string region after it, up to the whole arena */
            size_t d0 = (size_t)((uint8_t *)m1->descs - a1), dn = sizeof(psp_desc) * m1->ndesc;
            CHECK(!memcmp(a1, a2, d0) && !memcmp(a1 + d0 + dn, a2 + d0 + dn, sizeof a1 - d0 - dn),
                  "case %u: arena bytes outside the descriptor pointers differ", k);
            int same = m1->build_len == m2->build_len && m1->ndesc == m2->ndesc &&
                       !memcmp(m1->build_id, m2->build_id, m1->build_len) &&
                       (uint8_t *)m1->descs - a1 == (uint8_t *)m2->descs - a2;
            for (uint32_t i = 0; same && i < m1->ndesc; i++)
                same = m1->descs[i].ls_addr == m2->descs[i].ls_addr && m1->descs[i].line == m2->descs[i].line &&
                       m1->descs[i].name - (char *)a1 == m2->descs[i].name - (char *)a2 &&
                       m1->descs[i].file - (char *)a1 == m2->descs[i].file - (char *)a2;
            CHECK(same, "case %u: image fields or pointer offsets differ", k);
        }
    }
    printf("TABLE cases %u\n", (unsigned)(sizeof cases / sizeof cases[0]));
    return 0;
}

/* ---- api: runtime cases ---- */
static unsigned char *g_cap;
static size_t g_caplen;
static atomic_int g_reenter, g_reenter_rc = 1;
static const unsigned char *g_elf;
static size_t g_elflen;
static long cb_write(void *ctx, const void *b, unsigned long n)
{
    (void)ctx;
    if (atomic_load(&g_reenter) && atomic_exchange(&g_reenter, 0)) {
        uint32_t img = 55;
        atomic_store(&g_reenter_rc, ps3dk_prof_spu_image_register_elf(g_elf, g_elflen, &img));
    }
    g_cap = realloc(g_cap, g_caplen + n);
    memcpy(g_cap + g_caplen, b, n);
    g_caplen += n;
    return (long)n;
}
static int cb_flush(void *c) { (void)c; return 0; }
static int cb_close(void *c) { (void)c; return 0; }

/* the post-parse race: a thread starts shutdown and is held inside the seal (holding spu_mutex) until released */
static pthread_mutex_t g_m = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_c = PTHREAD_COND_INITIALIZER;
static int g_in_seal, g_go;
static void hold_seal(void)
{
    pthread_mutex_lock(&g_m);
    g_in_seal = 1;
    pthread_cond_broadcast(&g_c);
    while (!g_go)
        pthread_cond_wait(&g_c, &g_m);
    pthread_mutex_unlock(&g_m);
}
static void *shut(void *a)
{
    (void)a;
    ps3dk_prof_shutdown();
    return NULL;
}
static void *release_later(void *a)
{
    (void)a;
    usleep(50000);
    pthread_mutex_lock(&g_m);
    g_go = 1;
    pthread_cond_broadcast(&g_c);
    pthread_mutex_unlock(&g_m);
    return NULL;
}
static pthread_t g_shut, g_rel;
static void after_parse(void)
{
    ps3dk_prof_test_hooks.spu_elf_after_parse = NULL;
    ps3dk_prof_test_hooks.spu_before_seal = hold_seal;
    pthread_create(&g_shut, NULL, shut, NULL);
    pthread_mutex_lock(&g_m);
    while (!g_in_seal)
        pthread_cond_wait(&g_c, &g_m);
    pthread_mutex_unlock(&g_m);
    pthread_create(&g_rel, NULL, release_later, NULL); /* the seal holds spu_mutex until then */
}

/* print the capture's SPU_IMAGES entry with image_index `want` in the checker format (prefix "e2e") */
static void print_capture_images(uint32_t want)
{
    for (size_t o = 16; o + 20 <= g_caplen;) {
        const unsigned char *h = g_cap + o, *pl = h + 16;
        uint32_t type = h[0] | h[1] << 8 | h[2] << 16 | (uint32_t)h[3] << 24;
        uint32_t len = h[4] | h[5] << 8 | h[6] << 16 | (uint32_t)h[7] << 24;
        if (type == 11) {
            uint32_t cnt = pl[0] | pl[1] << 8 | pl[2] << 16 | (uint32_t)pl[3] << 24, at = 4;
            for (uint32_t k = 0; k < cnt; k++) {
                uint32_t idx, bl, nd, im;
                int show;
                im = pl[at] | pl[at + 1] << 8 | pl[at + 2] << 16 | (uint32_t)pl[at + 3] << 24;
                show = im == want;
                at += 4;
                bl = pl[at] | pl[at + 1] << 8;
                if (show) {
                    printf("e2e OK bid=");
                    hex(pl + at + 2, bl);
                }
                at += 2 + bl;
                nd = pl[at] | pl[at + 1] << 8 | pl[at + 2] << 16 | (uint32_t)pl[at + 3] << 24;
                at += 4;
                if (show)
                    printf(" n=%u\n", nd);
                for (uint32_t j = 0; j < nd; j++) {
                    uint32_t ls, line, nl, fl;
                    idx = pl[at] | pl[at + 1] << 8 | pl[at + 2] << 16 | (uint32_t)pl[at + 3] << 24;
                    ls = pl[at + 4] | pl[at + 5] << 8 | pl[at + 6] << 16 | (uint32_t)pl[at + 7] << 24;
                    line = pl[at + 8] | pl[at + 9] << 8 | pl[at + 10] << 16 | (uint32_t)pl[at + 11] << 24;
                    at += 12;
                    nl = pl[at] | pl[at + 1] << 8;
                    if (show) {
                        printf("e2e R %u %u %u ", idx, ls, line);
                        hex(pl + at + 2, nl);
                    }
                    at += 2 + nl;
                    fl = pl[at] | pl[at + 1] << 8;
                    if (show) {
                        printf(" ");
                        hex(pl + at + 2, fl);
                        printf("\n");
                    }
                    at += 2 + fl;
                }
            }
        }
        o += 16 + len + 4;
    }
}

size_t ps3dk_prof__test_spu_arena_used(void);
size_t ps3dk_prof__test_spu_arena_len(void);
unsigned ps3dk_prof__test_spu_nimages(void);
int ps3dk_prof__test_spu_image_slot(unsigned i, void *out, size_t n);

/* the committed arena state: used, nimages, and the bytes of the next (unused) image slot */
typedef struct arena_state {
    size_t used;
    unsigned nimages;
    psp_image slot;
} arena_state;
static arena_state arena_now(void)
{
    arena_state a;
    memset(&a, 0, sizeof a);
    a.used = ps3dk_prof__test_spu_arena_used();
    a.nimages = ps3dk_prof__test_spu_nimages();
    CHECK(ps3dk_prof__test_spu_image_slot(a.nimages, &a.slot, sizeof a.slot) == 0, "the slot accessor");
    return a;
}
static int arena_same(const arena_state *x, const arena_state *y)
{
    return x->used == y->used && x->nimages == y->nimages && !memcmp(&x->slot, &y->slot, sizeof x->slot);
}

/* a table image of 4 descriptors consuming EXACTLY `bytes` of arena from an 8-aligned start */
static char g_fill[8][520];
static int fill_exact(size_t bytes, uint32_t *idx)
{
    ps3dk_prof_spu_desc d[4];
    size_t chars;
    if (bytes < 4 * sizeof(psp_desc) + 8 + 4)
        return -1;
    chars = bytes - 4 * sizeof(psp_desc) - 8; /* 8 NULs: a name and a file per descriptor */
    for (int i = 0; i < 8; i++) {
        size_t l = chars / 8 + ((size_t)i < chars % 8 ? 1 : 0);
        if (l < 1 || l > 512)
            return -1;
        memset(g_fill[i], 'f', l);
        g_fill[i][l] = 0;
    }
    for (int i = 0; i < 4; i++)
        d[i] = (ps3dk_prof_spu_desc){(uint32_t)(16 * i), (uint32_t)i, g_fill[2 * i], g_fill[2 * i + 1]};
    return ps3dk_prof_spu_image_register_table((const uint8_t *)"\x01", 1, d, 4, idx);
}

static int start(uint32_t image_bytes, uint32_t mode)
{
    ps3dk_prof_config c;
    ps3dk_prof_ext_config x;
    ps3dk_prof_sink_callbacks cb = {cb_write, cb_flush, cb_close, NULL};
    ps3dk_prof_config_defaults(&c);
    c.mode = mode; /* THREAD: any thread may shut down (the post-parse race); APP: no concurrent drains */
    c.app = "i3b";
    c.build_id = "i3b";
    memset(&x, 0, sizeof x);
    x.size = PS3DK_PROF_EXT_CONFIG_SIZE;
    x.spu_lanes_max = 1;
    x.spu_ring_slots = 64;
    x.spu_image_bytes_max = image_bytes;
    if (ps3dk_prof_set_sink_callbacks(&cb) != 0)
        return -1;
    return ps3dk_prof_init_ex(&c, &x);
}

static int api_mode(const char *elf_path)
{
    size_t n = 0, need = 0, len;
    unsigned char *b = slurp(elf_path, &n);
    psp_elf_view v;
    uint32_t img = 1234, fill_idx = 99;
    int rc;
    arena_state a0, a1;
    if (!b || psp_elf_parse(b, n, &v) != 0) {
        printf("I3B FAIL the api fixture is not a valid image\n");
        return 1;
    }
    g_elf = b;
    g_elflen = n;
    for (uint32_t i = 0; i < v.n; i++) {
        psp_desc_rec r;
        if (psp_elf_get(&v, i, &r) != 0)
            return 1;
        need += r.name_len + 1 + r.file_len + 1;
    }
    need += sizeof(psp_desc) * v.n; /* the arena bytes this image needs from an 8-aligned start */
    /* before init: ENOTINIT, out untouched */
    CHECK(ps3dk_prof_spu_image_register_elf(b, n, &img) == PS3DK_PROF_ENOTINIT && img == 1234, "ENOTINIT");
    CHECK(start(4096, PS3DK_PROF_MODE_THREAD) == 0, "init");
    len = ps3dk_prof__test_spu_arena_len();
    CHECK(len == 4096 && (len - need) % 8 == 0, "arena %zu, need %zu: an exact fit is possible", len, need);
    CHECK(fill_exact(len - need, &fill_idx) == 0 && fill_idx == 0 && ps3dk_prof__test_spu_arena_used() == len - need,
          "a table image leaves exactly %zu bytes (used %zu)", need, ps3dk_prof__test_spu_arena_used());
    CHECK(ps3dk_prof_spu_image_register_elf(NULL, n, &img) == PS3DK_PROF_EINVAL && img == 1234, "NULL elf");
    CHECK(ps3dk_prof_spu_image_register_elf(b, n, NULL) == PS3DK_PROF_EINVAL, "NULL out");
    CHECK(ps3dk_prof_spu_image_register_elf(b, 51, &img) == PS3DK_PROF_EINVAL && img == 1234, "size under header");
    /* late getter failures in the copy pass (gets n+1 .. 2n): EINVAL, *image, arena_used, nimages and the next image
     * slot unchanged (unused-tail bytes may be dirtied by design) */
    {
        uint32_t at[3] = {v.n + 1, v.n + (v.n + 1) / 2, 2 * v.n};
        CHECK(at[0] < at[1] && at[1] < at[2], "distinct first/middle/last copy-pass gets (%u %u %u)", at[0], at[1], at[2]);
        for (int k = 0; k < 3; k++) {
            img = 1234;
            a0 = arena_now();
            ps3dk_prof_test_hooks.spu_src_fail_at = at[k];
            rc = ps3dk_prof_spu_image_register_elf(b, n, &img);
            a1 = arena_now();
            CHECK(rc == PS3DK_PROF_EINVAL && img == 1234, "late failure at get %u: rc %d img %u", at[k], rc, img);
            CHECK(ps3dk_prof_test_hooks.spu_src_fail_at == 0, "the hook fired");
            CHECK(arena_same(&a0, &a1), "late failure at get %u: arena_used %zu->%zu, nimages %u->%u or the slot changed",
                  at[k], a0.used, a1.used, a0.nimages, a1.nimages);
        }
        rc = ps3dk_prof_spu_image_register_elf(b, n, &img);
        CHECK(rc == 0 && img == 1 && ps3dk_prof__test_spu_arena_used() == len,
              "the retry fits the EXACT remaining capacity: rc %d img %u used %zu of %zu", rc, img,
              ps3dk_prof__test_spu_arena_used(), len);
    }
    /* sink-callback reentry: EBUSY, nothing registered */
    atomic_store(&g_reenter, 1); /* the drainer thread's next sink write calls register_elf from the callback */
    for (int t = 0; t < 400 && atomic_load(&g_reenter); t++)
        usleep(5000);
    CHECK(!atomic_load(&g_reenter), "the drainer wrote within 2 s");
    CHECK(atomic_load(&g_reenter_rc) == PS3DK_PROF_EBUSY, "reentry rc %d", atomic_load(&g_reenter_rc));
    /* the post-parse race: shutdown claimed (and the seal holding spu_mutex) between the parse and enter */
    img = 4321;
    ps3dk_prof_test_hooks.spu_elf_after_parse = after_parse;
    rc = ps3dk_prof_spu_image_register_elf(b, n, &img);
    CHECK(rc == PS3DK_PROF_ESTOPPED && img == 4321, "post-parse race: rc %d img %u", rc, img);
    pthread_join(g_shut, NULL);
    pthread_join(g_rel, NULL);
    ps3dk_prof_test_hooks.spu_before_seal = NULL;
    print_capture_images(1);
    printf("API done\n");
    free(b);
    return 0;
}

/* the unlock-failure rule (the hook performs the REAL unlock, then reports failure): the image exists and its index is
 * returned with ESTOPPED; the fault latch then refuses a retry, so nothing is registered twice */
static int api_unlock_mode(const char *elf_path)
{
    size_t n = 0;
    unsigned char *b = slurp(elf_path, &n);
    uint32_t img = 1234, img2 = 5678;
    int rc;
    if (!b)
        return 1;
    CHECK(start(0, PS3DK_PROF_MODE_APP) == 0, "init (APP mode: no drainer thread shares the unlock hooks)");
    ps3dk_prof_test_hooks.spu_ops = 0;
    ps3dk_prof_test_hooks.spu_unlock_fail_at = 1; /* the next spu_mutex unlock: register_elf's leave() */
    rc = ps3dk_prof_spu_image_register_elf(b, n, &img);
    ps3dk_prof_test_hooks.spu_unlock_fail_at = 0;
    CHECK(rc == PS3DK_PROF_ESTOPPED && img == 0 && ps3dk_prof__test_spu_nimages() == 1,
          "unlock failure after the commit: ESTOPPED with the index (rc %d img %u nimages %u)", rc, img,
          ps3dk_prof__test_spu_nimages());
    rc = ps3dk_prof_spu_image_register_elf(b, n, &img2);
    CHECK(rc == PS3DK_PROF_ESTOPPED && img2 == 5678 && ps3dk_prof__test_spu_nimages() == 1,
          "the latch: a retry is ESTOPPED, output untouched, no duplicate (rc %d nimages %u)", rc,
          ps3dk_prof__test_spu_nimages());
    CHECK(ps3dk_prof_shutdown() == PS3DK_PROF_ESINKFAIL, "shutdown fails closed after the fault");
    printf("API-UNLOCK done\n");
    free(b);
    return 0;
}

int main(int argc, char **argv)
{
    int rc;
    if (argc >= 3 && !strcmp(argv[1], "parse"))
        rc = each(argv[2], 0);
    else if (argc >= 3 && !strcmp(argv[1], "register"))
        rc = each(argv[2], 1);
    else if (argc >= 2 && !strcmp(argv[1], "table"))
        rc = table_mode();
    else if (argc >= 3 && !strcmp(argv[1], "api"))
        rc = api_mode(argv[2]);
    else if (argc >= 3 && !strcmp(argv[1], "api-unlock"))
        rc = api_unlock_mode(argv[2]);
    else
        return 2;
    if (rc)
        return rc;
    if (g_bad)
        return 1;
    printf("I3B PASS\n");
    return 0;
}
