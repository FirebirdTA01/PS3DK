/* The production allocator is included below. Only allocation, mapping and
 * GPU completion are replaced: queued reads happen when completion is waited
 * on, and freeing mapped or queued storage is recorded as a lifetime error. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct { void *begin, *end; } CellGcmContextData;
typedef CellGcmContextData gcmContextData;
static CellGcmContextData context;
#define CELL_GCM_CURRENT (&context)
#define PSGL_BUFFER_ALIGNMENT 8u
#define PSGL_CLIENT_ARRAY_ALIGNMENT 16u
#define PSGL_CLIENT_ARRAY_SCRATCH_SIZE 64u
#define GL_OUT_OF_MEMORY 0x0505u
static void *g_psgl_client_array_scratch;
static uint32_t g_psgl_client_array_scratch_offset;
static uint32_t g_psgl_client_array_scratch_size;
static uint32_t g_psgl_client_array_scratch_cursor;
static unsigned error, waits, unmaps, lifetime_errors, checks, failures;
static int fail_wait, fail_unmap, fail_alloc, fail_map;
struct block { void *ptr; unsigned mapped, freed; };
static struct block blocks[64];
static unsigned block_count;
static unsigned char *queued;
static unsigned char queued_value;

static struct block *block(void *p)
{
    for (unsigned i = 0; i < block_count; ++i)
        if (blocks[i].ptr == p) return &blocks[i];
    abort();
}
static void *fake_alloc(unsigned align, unsigned size)
{
    (void)align;
    if (fail_alloc) return NULL;
    if (block_count == 64) abort();
    void *p = malloc(size);
    if (!p) abort();
    blocks[block_count++] = (struct block){p,0,0};
    return p;
}
static void fake_free(void *p)
{
    struct block *b = block(p);
    if (b->mapped || queued) ++lifetime_errors;
    b->freed = 1;
    /* Keep the host allocation readable to diagnose baseline use-after-free. */
}
static void psgl_zero(void *p, unsigned size) { memset(p, 0, size); }
static uint32_t psgl_align_u32(uint32_t n, uint32_t a) { return (n+a-1u)&~(a-1u); }
static void psgl_gl_error(unsigned value) { error = value; }
static int cellGcmMapMainMemory(void *p, uint32_t size, uint32_t *offset)
{
    (void)size;
    if (fail_map) return -1;
    block(p)->mapped = 1;
    *offset = block_count * 256u;
    return 0;
}
static void complete(void)
{
    if (queued && *queued != queued_value) ++lifetime_errors;
    queued = NULL;
}
static int psgl_context_wait_idle_bounded(CellGcmContextData *gcm, const char *what)
{
    (void)gcm; (void)what;
    ++waits;
    if (fail_wait) return 0;
    complete();
    return 1;
}
static int cellGcmUnmapEaIoAddress(void *p)
{
    ++unmaps;
    if (fail_unmap) return -1;
    if (queued) ++lifetime_errors;
    block(p)->mapped = 0;
    return 0;
}
#define memalign fake_alloc
#define free fake_free
#include "allocator.h"
#undef free

static void check(int ok, const char *name)
{
    ++checks;
    if (!ok) { ++failures; printf("FAIL %s\n", name); }
}
static void reset(void)
{
    for (unsigned i = 0; i < block_count; ++i) free(blocks[i].ptr);
    block_count = 0; queued = NULL;
    g_psgl_client_array_scratch = NULL;
    g_psgl_client_array_scratch_size = g_psgl_client_array_scratch_cursor = 0;
    g_psgl_client_array_scratch_offset = 0;
    error = waits = unmaps = lifetime_errors = 0;
    fail_wait = fail_unmap = fail_alloc = fail_map = 0;
}
static void seed(void)
{
    uint8_t *p; uint32_t off;
    if (!psgl_alloc_client_array_scratch(48,&p,&off)) abort();
    *p = 0x31; queued = p; queued_value = 0x31;
}
static void retained_failure(unsigned size, const char *name)
{
    void *old = g_psgl_client_array_scratch;
    unsigned cursor = g_psgl_client_array_scratch_cursor;
    unsigned capacity = g_psgl_client_array_scratch_size;
    unsigned base_offset = g_psgl_client_array_scratch_offset;
    uint8_t *p = (uint8_t *)(uintptr_t)1; uint32_t off = 123;
    int ok = psgl_alloc_client_array_scratch(size,&p,&off);
    check(!ok && error == GL_OUT_OF_MEMORY &&
          g_psgl_client_array_scratch == old &&
          g_psgl_client_array_scratch_cursor == cursor &&
          g_psgl_client_array_scratch_size == capacity &&
          g_psgl_client_array_scratch_offset == base_offset &&
          !block(old)->freed && block(old)->mapped &&
          p == (uint8_t *)(uintptr_t)1 && off == 123 && !lifetime_errors, name);
}
int main(void)
{
    uint8_t *p; uint32_t off;
    reset(); seed();
    check(psgl_alloc_client_array_scratch(8,&p,&off) && !waits && !unmaps,
          "space in current lap needs no wait");
    reset(); seed();
    int ok = psgl_alloc_client_array_scratch(48,&p,&off);
    if (ok) *p = 0x72;
    complete();
    check(ok && waits == 1 && !unmaps && !lifetime_errors,
          "queued draw reads original bytes before wrap overwrite");
    reset(); seed();
    void *old = g_psgl_client_array_scratch;
    ok = psgl_alloc_client_array_scratch(128,&p,&off);
    complete();
    check(ok && waits == 1 && unmaps == 1 && !lifetime_errors &&
          block(old)->freed && !block(old)->mapped &&
          g_psgl_client_array_scratch != old,
          "growth completes queued draw and unmaps before freeing");
    reset(); seed(); fail_wait = 1;
    retained_failure(48,"wrap timeout preserves old storage and outputs");
    reset(); seed(); fail_wait = 1;
    retained_failure(128,"growth timeout preserves old storage and outputs");
    reset(); seed(); fail_unmap = 1;
    retained_failure(128,"growth unmap failure preserves old storage and outputs");
    fail_unmap = 0; error = 0;
    check(psgl_alloc_client_array_scratch(128,&p,&off) && !error && !lifetime_errors,
          "growth can be retried after unmap failure");
    reset(); seed(); fail_wait = 1;
    (void)psgl_alloc_client_array_scratch(48,&p,&off);
    fail_wait = 0; error = 0;
    ok = psgl_alloc_client_array_scratch(48,&p,&off);
    if (ok) *p = 0x72;
    complete();
    check(ok && !error && !lifetime_errors,"wrap can be retried after timeout");
    reset(); fail_alloc = 1;
    check(!psgl_alloc_client_array_scratch(48,&p,&off) && !g_psgl_client_array_scratch,
          "initial allocation failure leaves no live scratch");
    reset(); fail_map = 1;
    check(!psgl_alloc_client_array_scratch(48,&p,&off) && !g_psgl_client_array_scratch &&
          block_count == 1 && blocks[0].freed && !lifetime_errors,
          "initial map failure releases only unmapped memory");
    reset();
    printf("PSGL_CLIENT_SCRATCH_%s checks=%u failures=%u\n",failures?"FAIL":"OK",checks,failures);
    return failures ? 1 : 0;
}
