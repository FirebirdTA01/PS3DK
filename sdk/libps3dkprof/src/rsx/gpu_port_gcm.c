/* P3.3 R3: the GCM device binding (contract in gpu_port_gcm.h). */
#include "gpu_port_gcm.h"

#include <stddef.h>
#include <string.h>

#define GPP_METHOD(m, n) (((uint32_t)(n) << 18) | (uint32_t)(m))
#define GPP_QUERY_GET 0x1800u                   /* NV40TCL_QUERY_GET */
#define GPP_SEMAPHORE_OFFSET 0x1d6cu            /* NV40TCL_SEMAPHORE_OFFSET */
#define GPP_SEMAPHORE_BACKENDWRITE_RELEASE 0x1d70u
#define GPP_QUERY_TIMESTAMP (1u << 24)          /* the report type in bits 24..31 (the chosen encoding) */

#ifdef GPP_HOST_DEVMEM
#ifdef PS3DK_PROF_PAL_LV2
#error "GPP_HOST_DEVMEM is a host-only test seam"
#endif
#define GPP_LD64(pc, a) ((pc)->dev->ld64(a))
#define GPP_ST64(pc, a, v) ((pc)->dev->st64((a), (v)))
#define GPP_LD32(pc, a) ((pc)->dev->ld32(a))
#define GPP_ST32(pc, a, v) ((pc)->dev->st32((a), (v)))
#else /* the real path: one aligned volatile access each */
#define GPP_LD64(pc, a) ((void)(pc), *(a))
#define GPP_ST64(pc, a, v) ((void)(pc), *(a) = (v))
#define GPP_LD32(pc, a) ((void)(pc), *(a))
#define GPP_ST32(pc, a, v) ((void)(pc), *(a) = (v))
#endif

static int gpp_label_reserved(uint32_t l)
{
    return l == 255u || (l >= 65u && l <= 68u); /* our libPSGL's idle token and prepared-buffer/status labels */
}

/* 1 when the context is valid and has room for the pair (*cur: its current address); 0 otherwise.  Integer
 * addresses only: nothing is subtracted or formed before validation. */
static int gpp_room(const gpp_ctx *c, uintptr_t *cur)
{
    uintptr_t b = (uintptr_t)c->begin, u = (uintptr_t)c->current, e = (uintptr_t)c->end;

    if (b == 0 || u == 0 || e == 0)
        return 0;
    if (((b | u | e) & (uintptr_t)(sizeof(uint32_t) - 1)) != 0)
        return 0;
    if (b > u || u > e)
        return 0;
    if ((e - u) / sizeof(uint32_t) < GPP_PAIR_WORDS)
        return 0;
    *cur = u;
    return 1;
}

static int gpp_ctx_valid(const gpp_ctx *c)
{
    uintptr_t b = (uintptr_t)c->begin, u = (uintptr_t)c->current, e = (uintptr_t)c->end;

    return b != 0 && u != 0 && e != 0 && ((b | u | e) & (uintptr_t)(sizeof(uint32_t) - 1)) == 0 && b <= u && u <= e;
}

/* one compiler-visible call through the SDK callback type: the compiler owns the call's ABI (argument homes,
 * clobbers, LR/TOC), and the context is re-read after it */
__attribute__((noinline)) static int32_t gpp_invoke(gpp_ctx *c, uint32_t count)
{
    return c->callback(c, count);
}

static int gpp_reserve_pair(void *x, uint32_t report, uint32_t label, uint32_t value)
{
    gpp_port *pc = x;
    gpp_ctx *c;
    uintptr_t cur;
    uint32_t *w;

    if (!pc->ready || label != pc->label || report < pc->report_first || report - pc->report_first >= pc->report_count)
        return 1;
    c = pc->ctx;
    if (!gpp_room(c, &cur)) {
        if (!gpp_ctx_valid(c) || c->callback == NULL || gpp_invoke(c, GPP_PAIR_WORDS) != 0)
            return 1;
        if (!gpp_room(c, &cur)) /* re-read: the callback may have flushed, relocated or rewritten the context */
            return 1;
    }
    w = (uint32_t *)cur;
    w[0] = GPP_METHOD(GPP_QUERY_GET, 1);
    w[1] = GPP_QUERY_TIMESTAMP | (report * GPP_REPORT_STRIDE);
    w[2] = GPP_METHOD(GPP_SEMAPHORE_OFFSET, 1);
    w[3] = label * 0x10u;
    w[4] = GPP_METHOD(GPP_SEMAPHORE_BACKENDWRITE_RELEASE, 1);
    w[5] = (value & 0xff00ff00u) | ((value >> 16) & 0xffu) | ((value & 0xffu) << 16); /* the back-end byte swap */
    c->current = w + GPP_PAIR_WORDS;
    return 0;
}

static void gpp_flush(void *x)
{
    gpp_port *pc = x;

    if (pc->ready)
        pc->dev->flush(pc->ctx);
}

static uint32_t gpp_read_label(void *x, uint32_t label)
{
    const gpp_port *pc = x;

    if (!pc->ready || label != pc->label)
        return 0;
    return GPP_LD32(pc, pc->label_word);
}

static void gpp_write_label(void *x, uint32_t label, uint32_t value)
{
    gpp_port *pc = x;

    if (pc->ready && label == pc->label)
        GPP_ST32(pc, pc->label_word, value);
}

static uint64_t gpp_read_timer(void *x, uint32_t report)
{
    const gpp_port *pc = x;

    if (!pc->ready || report < pc->report_first || report - pc->report_first >= pc->report_count)
        return PRG_POISON;
    return GPP_LD64(pc, pc->timer[report - pc->report_first]);
}

static void gpp_write_timer(void *x, uint32_t report, uint64_t value)
{
    gpp_port *pc = x;

    if (pc->ready && report >= pc->report_first && report - pc->report_first < pc->report_count)
        GPP_ST64(pc, pc->timer[report - pc->report_first], value);
}

static void gpp_sync(void *x)
{
    (void)x;
#ifdef PS3DK_PROF_PAL_LV2
    __asm__ volatile("sync" ::: "memory");
#else
    /* host: a full barrier that TSan accepts (it refuses atomic_thread_fence): a SC no-op RMW on a private word */
    static uint32_t gpp_fence_word;
    (void)__atomic_fetch_add(&gpp_fence_word, 0u, __ATOMIC_SEQ_CST);
#endif
}

int gpp_prepare(gpp_port *pc, const gpp_dev *dev, gpp_ctx *ctx, uint32_t report_first, uint32_t report_count,
                uint32_t label)
{
    uintptr_t base = 0, a;
    uint32_t i;
    void *p;

    if (pc == NULL)
        return PRG_EINVAL;
    memset(pc, 0, sizeof *pc);
    if (dev == NULL || dev->report_addr == NULL || dev->label_addr == NULL || dev->flush == NULL || ctx == NULL ||
        !gpp_ctx_valid(ctx))
        return PRG_EINVAL;
#ifdef GPP_HOST_DEVMEM
    if (dev->ld64 == NULL || dev->st64 == NULL || dev->ld32 == NULL || dev->st32 == NULL)
        return PRG_EINVAL;
#endif
    /* the count first (nonzero, within the cache and the area), only then the first index against what remains */
    if (report_count == 0 || report_count > GPP_CACHE_MAX || report_count > GPP_REPORTS ||
        report_first > GPP_REPORTS - report_count)
        return PRG_EINVAL;
    /* the label range before any narrowing to the accessor's u8 index */
    if (label < GPP_LABEL_USER_FIRST || label > GPP_LABEL_LAST || gpp_label_reserved(label))
        return PRG_EINVAL;
    p = dev->label_addr(label);
    a = (uintptr_t)p;
    if (p == NULL || (a & (uintptr_t)(sizeof(uint32_t) - 1)) != 0)
        goto fail;
    pc->label_word = p;
    for (i = 0; i < report_count; i++) {
        p = dev->report_addr(report_first + i);
        a = (uintptr_t)p;
        if (p == NULL || (a & (uintptr_t)(sizeof(uint64_t) - 1)) != 0 || a > UINTPTR_MAX - (GPP_REPORT_STRIDE - 1))
            goto fail;
        if (i == 0)
            base = a;
        else if ((uintptr_t)i > (UINTPTR_MAX - base) / GPP_REPORT_STRIDE || a != base + (uintptr_t)i * GPP_REPORT_STRIDE)
            goto fail;
        pc->timer[i] = p; /* the timer is the entry's first field */
    }
    pc->report_first = report_first;
    pc->report_count = report_count;
    pc->label = label;
    pc->ctx = ctx;
    pc->dev = dev;
    __atomic_store_n(&pc->ready, 1u, __ATOMIC_RELEASE); /* readiness last, after every check */
    return PRG_OK;
fail:
    memset(pc, 0, sizeof *pc);
    return PRG_EINVAL;
}

void gpp_release(gpp_port *pc)
{
    if (pc != NULL) {
        __atomic_store_n(&pc->ready, 0u, __ATOMIC_RELEASE);
        memset(pc, 0, sizeof *pc);
    }
}

void gpp_bind(prg_port *p, gpp_port *pc)
{
    p->ctx = pc;
    p->reserve_pair = gpp_reserve_pair;
    p->flush = gpp_flush;
    p->read_label = gpp_read_label;
    p->write_label = gpp_write_label;
    p->read_timer = gpp_read_timer;
    p->write_timer = gpp_write_timer;
    p->sync = gpp_sync;
}

#ifdef PS3DK_PROF_PAL_LV2
static void *gpp_gcm_report(uint32_t index)
{
    return cellGcmGetReportDataAddressLocation(index, CELL_GCM_LOCATION_LOCAL);
}

static void *gpp_gcm_label(uint32_t index)
{
    return cellGcmGetLabelAddress((uint8_t)index); /* index <= 255, checked by gpp_prepare */
}

static void gpp_gcm_flush(gpp_ctx *ctx)
{
    cellGcmFlush(ctx);
}

const gpp_dev gpp_dev_gcm = {gpp_gcm_report, gpp_gcm_label, gpp_gcm_flush};
#endif
