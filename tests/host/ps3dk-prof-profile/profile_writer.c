/* The PROFILE writer on the REAL runtime (host PAL): profile-format-spec.md 6 and 8 "Runtime".
 *
 * usage: profile_writer OUT_DIR MODE   MODE: profile (built with PS3DK_PROF_WITH_PROFILE) or plain (built without)
 * Writes one capture (APP mode, one GPU domain enabled, no attach), then checks its bytes: file_flags byte 12 is 3
 * (profile) or 1 (plain); chunk 0 HEADER, chunk 1 CLOCKS; in profile mode chunk 2 is a PROFILE whose payload equals,
 * byte for byte, the encoding computed here from the linked identity and the expected field values (the expected
 * LOADER_HOOK / LIVE_LINKED values come from the build: -DEXPECT_HOOK=0/1 -DEXPECT_LIVE=0/1), and no other chunk is
 * a PROFILE; in plain mode no chunk 17 exists.  Prints "PW <mode> PASS <file>" or FAIL lines. */
#include <dirent.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <ps3dk/prof.h>

#include "prof_spu.h"
#ifdef PS3DK_PROF_WITH_PROFILE
#include "prof_identity.h"
#endif

static int g_bad;
#define CHECK(c, ...)                                                                                                  \
    do {                                                                                                               \
        if (!(c)) {                                                                                                    \
            printf("PW FAIL line %d: ", __LINE__);                                                                    \
            printf(__VA_ARGS__);                                                                                       \
            printf("\n");                                                                                              \
            g_bad++;                                                                                                   \
        }                                                                                                              \
    } while (0)

__attribute__((unused)) static void le16(uint8_t *b, size_t *n, uint16_t v) { b[(*n)++] = (uint8_t)v, b[(*n)++] = (uint8_t)(v >> 8); }
__attribute__((unused)) static void le32(uint8_t *b, size_t *n, uint32_t v)
{
    for (int k = 0; k < 4; k++)
        b[(*n)++] = (uint8_t)(v >> (8 * k));
}
__attribute__((unused)) static void le64(uint8_t *b, size_t *n, uint64_t v)
{
    for (int k = 0; k < 8; k++)
        b[(*n)++] = (uint8_t)(v >> (8 * k));
}
static uint32_t rd32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
__attribute__((unused)) static uint64_t rd64(const uint8_t *p) { return (uint64_t)rd32(p) | (uint64_t)rd32(p + 4) << 32; }

int main(int argc, char **argv)
{
    if (argc != 3)
        return 2;
    const char *dir = argv[1];
    int profile = strcmp(argv[2], "profile") == 0;
    ps3dk_prof_config c;
    ps3dk_prof_config_defaults(&c);
    c.mode = PS3DK_PROF_MODE_APP;
    c.app = "pw";
    c.build_id = "pw";
    ps3dk_prof_ext_config x;
    memset(&x, 0, sizeof x);
    x.size = PS3DK_PROF_EXT_CONFIG_SIZE;
    x.gpu_domains_max = 1;
    x.gpu_report_slots_max = 256;
    CHECK(ps3dk_prof_set_sink_path(dir) == 0, "sink");
    CHECK(ps3dk_prof_init_ex(&c, &x) == 0, "init_ex");
    CHECK(ps3dk_prof_thread_begin("main") == 0, "thread_begin");
    CHECK(ps3dk_prof_drain() == 0, "drain");
    ps3dk_prof_thread_end();
    CHECK(ps3dk_prof_shutdown() == 0, "shutdown");
    /* the one capture in OUT_DIR */
    char path[1024] = {0};
    DIR *d = opendir(dir);
    struct dirent *e;
    int n_files = 0;
    while (d && (e = readdir(d))) {
        size_t l = strlen(e->d_name);
        if (l > 5 && strcmp(e->d_name + l - 5, ".prof") == 0) {
            snprintf(path, sizeof path, "%s/%s", dir, e->d_name);
            n_files++;
        }
    }
    if (d)
        closedir(d);
    CHECK(n_files == 1, "one capture in the sink dir (%d)", n_files);
    FILE *f = fopen(path, "rb");
    static uint8_t buf[1 << 20];
    size_t len = f ? fread(buf, 1, sizeof buf, f) : 0;
    if (f)
        fclose(f);
    CHECK(len > 16 && memcmp(buf, "PS3DKPRF", 8) == 0, "magic");
    CHECK(buf[12] == (profile ? 3 : 1) && buf[13] == 0 && buf[14] == 0 && buf[15] == 0, "file_flags byte 12 = %u",
          buf[12]);
    /* walk the chunks */
    size_t pos = 16, idx = 0, profile_at = (size_t)-1, profiles = 0;
    uint64_t freq = 0;
    const uint8_t *prof_payload = NULL;
    uint32_t prof_len = 0;
    while (pos + 16 <= len) {
        uint32_t ty = rd32(buf + pos), l = rd32(buf + pos + 4);
        if (idx == 1)
            freq = rd64(buf + pos + 16 + 12);
        if (ty == 17) {
            profiles++;
            profile_at = idx;
            prof_payload = buf + pos + 16;
            prof_len = l;
        }
        pos += 16 + (size_t)l + 4;
        idx++;
    }
    CHECK(pos == len, "the chunks tile the file");
    if (!profile) {
        CHECK(profiles == 0, "a build without PS3DK_PROF_WITH_PROFILE writes no PROFILE chunk");
        (void)freq, (void)prof_payload, (void)prof_len, (void)profile_at;
    } else {
#ifdef PS3DK_PROF_WITH_PROFILE
        CHECK(profiles == 1 && profile_at == 2, "exactly one PROFILE, at chunk 2 (count %zu at %zu)", profiles,
              profile_at);
        const ps3dk_prof__identity_t *id = &ps3dk_prof__identity;
        uint8_t want[512];
        size_t n = 0;
        le16(want, &n, 1);
        le16(want, &n, 0);
        want[n++] = id->id_len;
        memcpy(want + n, id->id, id->id_len);
        n += id->id_len;
        le32(want, &n, 1);
        memcpy(want + n, id->digest, 32);
        n += 32;
        le32(want, &n, id->premise_bits);
        le32(want, &n, id->kinds);
        le16(want, &n, 7);
        /* 1 SYS_SW_VERSION, 2 HW_MODEL_CLASS: NOT_READABLE */
        for (uint16_t fid = 1; fid <= 2; fid++) {
            le16(want, &n, fid);
            want[n++] = 3, want[n++] = 4;
            le32(want, &n, 0);
        }
        le16(want, &n, 3);
        want[n++] = 1, want[n++] = 8;
        le64(want, &n, freq);
        le16(want, &n, 4);
        want[n++] = 3, want[n++] = 4;
        le32(want, &n, 0);
        le16(want, &n, 5);
        want[n++] = 2, want[n++] = 1, want[n++] = EXPECT_HOOK;
        le16(want, &n, 6);
        want[n++] = 2, want[n++] = 1, want[n++] = EXPECT_LIVE;
        le16(want, &n, 7);
        want[n++] = 1, want[n++] = 40;
        le32(want, &n, 40);                 /* size */
        le32(want, &n, 0);                  /* spu_lanes_max: SPU off */
        le32(want, &n, 0);                  /* spu_ring_slots */
        le32(want, &n, 0);                  /* spu_image_bytes_max */
        le64(want, &n, 16ull * 1024 * 1024); /* spu_mem_max default */
        le32(want, &n, 1);                  /* gpu_domains_max */
        le32(want, &n, 256);                /* gpu_report_slots_max */
        le32(want, &n, 0);
        le32(want, &n, 0);
        CHECK(prof_payload && prof_len == n && memcmp(prof_payload, want, n) == 0,
              "PROFILE payload equals the expected %zu bytes (got %u)", n, prof_len);
#endif
    }
    printf("PW %s %s %s\n", argv[2], g_bad ? "FAIL" : "PASS", path);
    return g_bad ? 1 : 0;
}
