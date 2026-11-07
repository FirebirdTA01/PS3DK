#include "ob_gpu_run.h"

#include <string.h>

#include "ob_teardown.h"

static const char *const g_names[OB_G_CASES] = {"G-g0", "G-g1", "G-g2", "G-cal-idle", "G-cal-queued"};

static void lg(ob_gpu *g, const char *what, int rc)
{
    if (g->p->log)
        g->p->log(g->p->ctx, what, rc);
}

/* finite: g->polls polls */
static int wait_label(ob_gpu *g, uint32_t v)
{
    for (uint32_t i = 0; i < g->polls; ++i) {
        if (g->p->label_read(g->p->ctx) == v)
            return 0;
        g->p->pause(g->p->ctx);
    }
    lg(g, "gpu: the bench label did not reach its value", (int)v);
    return -1;
}

static int fence(ob_gpu *g)
{
    uint32_t v = ++g->label_value;
    g->p->rsx->label(g->p->rsx->ctx, v);
    g->p->rsx->flush(g->p->rsx->ctx);
    return wait_label(g, v);
}

int ob_gpu_open(ob_gpu *g, const ob_gpu_port *p, uint32_t report_count, uint32_t polls)
{
    char why[128];
    int rc;
    memset(g, 0, sizeof *g);
    g->p = p;
    g->polls = polls;
    g->report_count = report_count;
    if ((rc = p->gcm_init(p->ctx)) != 0) {
        lg(g, "gpu: GCM init failed", rc);
        return -1;
    }
    g->opened = 1;
    p->label_store(p->ctx, 0);
    if (fence(g) != 0) /* the context is quiescent before the attach */
        return -1;
    ob_owner o = {OB_GPU_REPORT_FIRST, report_count, OB_GPU_LABEL, OB_BENCH_REPORT, 2u, OB_BENCH_LABEL,
                  OB_WRITER_TAG, OB_BENCH_TAG};
    if (ob_owner_check(&o, why, sizeof why) != 0) {
        lg(g, why, -1);
        return -1;
    }
    if ((rc = p->attach(p->ctx, OB_GPU_REPORT_FIRST, report_count, OB_GPU_LABEL, &g->gpu)) != 0) {
        lg(g, "gpu: attach failed", rc);
        return -1;
    }
    g->attached = 1;
    return 0;
}

static uint32_t variant(ob_gpu *g, int v, uint32_t *bytes)
{
    uint32_t *before = g->p->cmd_current(g->p->ctx), rc = 0;
    unsigned zbad = ob_rsx_variant(g->p->rsx, v, OB_BENCH_REPORT, ++g->label_value);
    uint32_t *after = g->p->cmd_current(g->p->ctx);
    if (after < before)
        rc |= OB_GE_WRAP;
    *bytes = after >= before ? (uint32_t)((after - before) * 4u) : 0u;
    return rc | (zbad ? OB_GE_ZONE : 0u);
}

int ob_gpu_run(ob_gpu *g, uint64_t seed, unsigned r, ob_fw *w)
{
    static ob_block sched[1 + OB_G_CASES * 1024];
    long n = ob_gpu_schedule(seed, r, sched, sizeof sched / sizeof sched[0]);
    int failed = 0;
    g->rows = 0;
    if (n < 0 || !g->attached) {
        lg(g, "gpu: not attached or schedule too large", (int)n);
        return -1;
    }
    if (ob_fw_printf(w, "# OBG seed=%llu blocks=%ld digest=%016llx\n", (unsigned long long)seed, n,
                     (unsigned long long)ob_sched_digest(sched, (unsigned long)n)) != 0 ||
        ob_fw_printf(w, "kind,index,case,blk,rep,r0,r1,cmd_bytes,t0,t1,rc\n") != 0)
        return -1;
    for (long i = 0; i < n; ++i) {
        const ob_block *b = &sched[i];
        uint32_t rc = 0, bytes = 0;
        uint64_t r0 = 0, r1 = 0, t0 = 0, t1 = 0;
        int drc = g->p->drain(g->p->ctx); /* between blocks only */
        if (drc != 0) {
            lg(g, "gpu: drain between blocks failed: section stopped", drc);
            failed = 1;
            break;
        }
        if (b->c <= OB_G_G2) {
            t0 = g->p->tb(g->p->ctx);
            rc = variant(g, b->c, &bytes);
            rc |= wait_label(g, g->label_value) ? OB_GE_LABEL : 0u;
            t1 = g->p->tb(g->p->ctx);
            if (!(rc & OB_GE_LABEL)) {
                r0 = g->p->report_time(g->p->ctx, OB_BENCH_REPORT);
                r1 = g->p->report_time(g->p->ctx, OB_BENCH_REPORT + 1u);
            }
        } else {
            uint32_t qb = 0;
            if (b->c == OB_G_CAL_QUEUED)
                rc = variant(g, OB_G0, &qb) & ~OB_GE_WRAP; /* the queued g0 is not this row's byte count */
            t0 = g->p->tb(g->p->ctx);
            int crc = g->p->calibrate(g->p->ctx, g->gpu);
            t1 = g->p->tb(g->p->ctx);
            if (crc != 0) {
                lg(g, "gpu: calibrate failed", crc);
                rc |= OB_GE_CAL;
            }
            rc |= wait_label(g, g->label_value) ? OB_GE_LABEL : 0u; /* idle again before the next block */
        }
        if (ob_fw_printf(w, "GPU,%ld,%s,%s,%u,%llu,%llu,%u,%llu,%llu,%u\n", i, g_names[b->c],
                         b->kind == OB_BLK_WARM ? "warm" : "timed", (unsigned)b->rep, (unsigned long long)r0,
                         (unsigned long long)r1, bytes, (unsigned long long)t0, (unsigned long long)t1, rc) != 0) {
            failed = 1;
            break;
        }
        ++g->rows;
        if (rc & (OB_GE_ZONE | OB_GE_CAL))
            failed = 1;
        if (rc & OB_GE_LABEL) {
            failed = 1;
            break; /* the GPU is not known idle: no later block keeps its precondition */
        }
    }
    if (ob_fw_printf(w, "END,%lu\n", g->rows) != 0)
        failed = 1;
    return failed || g->rows != (unsigned long)n ? -1 : 0;
}

int ob_gpu_close(ob_gpu *g)
{
    int rc;
    if (!g->p || !g->opened)
        return OB_LIFE_FAILED;
    if (!g->attached)
        return OB_LIFE_CLEAN; /* nothing attached: GCM itself lives until process exit */
    if (fence(g) != 0) {
        lg(g, "gpu: label wait failed: domain retained", -1);
        return OB_LIFE_RETAINED;
    }
    if ((rc = g->p->detach(g->p->ctx, g->gpu)) != 0) {
        lg(g, "gpu: detach failed: domain retained", rc);
        return OB_LIFE_RETAINED;
    }
    if ((rc = g->p->release(g->p->ctx, g->gpu)) != 0) {
        lg(g, "gpu: release failed: domain retained", rc);
        return OB_LIFE_RETAINED;
    }
    g->attached = 0;
    return OB_LIFE_CLEAN;
}
