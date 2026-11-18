/* Test-only physical writer for Rust-produced genuine PPU checkpoint/payload
 * blobs. Uses the real C store and a bounded memory adapter; this is not a
 * production file adapter or evidence of filesystem power-loss durability. */
#include "roll_store.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { CHECKPOINT_CAP = 8192, PAYLOAD_CAP = 4096, FILE_CAP = 77824, MAX_PAIRS = 64 };
typedef struct { unsigned char *bytes; size_t length, capacity; } memory;
static int64_t read_at(void *ctx, uint64_t off, void *out, size_t n)
{
    memory *m = ctx;
    if (off > m->length || n > m->length - (size_t)off) return -1;
    memcpy(out, m->bytes + (size_t)off, n); return (int64_t)n;
}
static int64_t write_at(void *ctx, uint64_t off, const void *in, size_t n)
{
    memory *m = ctx;
    if (off > m->length || n > m->length - (size_t)off) return -1;
    memcpy(m->bytes + (size_t)off, in, n); return (int64_t)n;
}
static int resize_memory(void *ctx, uint64_t n)
{
    memory *m = ctx;
    if (n > m->capacity) return -1;
    memset(m->bytes, 0, (size_t)n); m->length = (size_t)n; return 0;
}
static int get_size(void *ctx, uint64_t *n) { *n = ((memory *)ctx)->length; return 0; }
static int sync_memory(void *ctx) { (void)ctx; return 0; }

/* Establish the bound before allocation/read. Exact transfer plus EOF catches
 * truncation or growth between the size query and the read. */
static int load_blob(const char *path, const char *kind, size_t cap, unsigned char **out, size_t *n)
{
    FILE *f = fopen(path, "rb");
    unsigned char *bytes = NULL;
    long size = 0;
    int ok = 0;
    if (!f) { fprintf(stderr, "pack_ppu: cannot open %s: %s\n", kind, path); return 0; }
    if (fseek(f, 0, SEEK_END) || (size = ftell(f)) < 0 || (uint64_t)size > cap || fseek(f, 0, SEEK_SET)) {
        fprintf(stderr, "pack_ppu: invalid size or over-cap %s: %s\n", kind, path); goto done;
    }
    bytes = malloc(size ? (size_t)size : 1);
    if (!bytes) { fprintf(stderr, "pack_ppu: allocation failed for %s: %s\n", kind, path); goto done; }
    if (fread(bytes, 1, (size_t)size, f) != (size_t)size || fgetc(f) != EOF || ferror(f)) {
        fprintf(stderr, "pack_ppu: short read or trailing bytes in %s: %s\n", kind, path); goto done;
    }
    ok = 1;
done:
    if (fclose(f)) { fprintf(stderr, "pack_ppu: close failed for %s: %s\n", kind, path); ok = 0; }
    if (!ok) { free(bytes); return 0; }
    *out = bytes; *n = (size_t)size; return 1;
}
static int write_result(const char *path, const memory *m)
{
    /* Never overwrite an existing shared artifact; only our newly created
     * output is removed on failure. Rust owns the containing unique directory. */
    FILE *f = fopen(path, "wbx");
    int ok;
    if (!f) { fprintf(stderr, "pack_ppu: cannot exclusively create output: %s\n", path); return 0; }
    ok = fwrite(m->bytes, 1, m->length, f) == m->length;
    if (fclose(f)) ok = 0;
    if (!ok) { fprintf(stderr, "pack_ppu: output write failed: %s\n", path); (void)remove(path); }
    return ok;
}
int main(int argc, char **argv)
{
    pfr_config config;
    pfr_layout layout;
    pfr_view view;
    memory m = {NULL, 0, 0};
    pfr_io io = {&m, read_at, write_at, resize_memory, get_size, sync_memory};
    pfr_store *store = NULL;
    void *scratch = NULL;
    int rc = 1, status, i;
    if (argc < 4 || (argc - 2) % 2 || (argc - 2) / 2 > MAX_PAIRS) {
        fprintf(stderr, "usage: pack_ppu OUTPUT CHECKPOINT PAYLOAD [CHECKPOINT PAYLOAD ...] (1..%d pairs)\n", MAX_PAIRS);
        return 2;
    }
    memset(&config, 0, sizeof config);
    config.max_bytes = FILE_CAP; config.retained_slots = 3;
    config.checkpoint_bytes = CHECKPOINT_CAP; config.payload_bytes = PAYLOAD_CAP;
    memset(config.session, 0x71, sizeof config.session);
    if (pfr_plan(&config, &layout) != PFR_OK || layout.file_bytes != FILE_CAP) {
        fputs("pack_ppu: fixed test layout mismatch\n", stderr); return 1;
    }
    store = malloc(pfr_state_bytes()); scratch = malloc(layout.scratch_bytes);
    m.capacity = (size_t)layout.file_bytes; m.bytes = malloc(m.capacity);
    if (!store || !scratch || !m.bytes) { fputs("pack_ppu: store allocation failed\n", stderr); goto done; }
    status = pfr_format(store, &io, &config, scratch, layout.scratch_bytes);
    if (status != PFR_OK) { fprintf(stderr, "pack_ppu: format failed (%d)\n", status); goto done; }
    for (i = 2; i < argc; i += 2) {
        unsigned char *cp = NULL, *payload = NULL;
        size_t cn = 0, pn = 0;
        if (!load_blob(argv[i], "checkpoint", CHECKPOINT_CAP, &cp, &cn)
            || !load_blob(argv[i + 1], "payload", PAYLOAD_CAP, &payload, &pn)) {
            free(payload); free(cp); goto done;
        }
        status = pfr_commit(store, cp, cn, payload, pn);
        free(payload); free(cp);
        if (status != PFR_OK) { fprintf(stderr, "pack_ppu: segment %d commit failed (%d)\n", (i - 2) / 2, status); goto done; }
    }
    if (pfr_get_view(store, &view) != PFR_OK) { fputs("pack_ppu: committed view unavailable\n", stderr); goto done; }
    if (!write_result(argv[1], &m)) goto done;
    printf("PASS C packer: %d segments, generation %llu, %zu bytes\n", (argc - 2) / 2,
        (unsigned long long)view.generation, m.length);
    rc = 0;
done:
    free(m.bytes); free(scratch); free(store); return rc;
}
