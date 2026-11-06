#include "ob_drv.h"

#include <pthread.h>
#include <stdio.h>
#include <string.h>

#include "ob_cases.h"
#include "ob_csv.h"

typedef struct ob_worker {
    pthread_mutex_t mu;
    pthread_cond_t cv;
    int sync_ok, created, acked, reg_rc, has_job, done, quit, joined;
    void *h;
    ob_block b;
    uint64_t t0, t1;
    int c;
    const ob_drv_port *p;
} ob_worker;

static const uint16_t g_cases[] = {OB_P_HOT, OB_P_NEST8, OB_P_COLD, OB_P_DROP, OB_P_FRAME, OB_P_EMPTY, OB_S_PAIR,
                                   OB_S_FLUSH0, OB_S_FLUSH1, OB_S_FLUSH16, OB_S_FLUSH128, OB_S_STAGEFULL,
                                   OB_S_BACKPRESS};
static const int g_cold[] = {0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
#define NCASES (sizeof g_cases / sizeof g_cases[0])
static ob_block g_sched[16384];
static ob_worker g_w[OB_PPU_WORKERS];

static void lg(const ob_drv_port *p, const char *what, int rc)
{
    if (p->log)
        p->log(p->ctx, what, rc);
}

static void *worker_main(void *arg)
{
    ob_worker *w = arg;
    char name[24];
    snprintf(name, sizeof name, "ob-%s", ob_case_table[w->c].name);
    int reg = w->p->thread_begin(w->p->ctx, name);
    pthread_mutex_lock(&w->mu);
    w->acked = 1, w->reg_rc = reg;
    pthread_cond_broadcast(&w->cv);
    if (reg != 0) { /* not registered: nothing to end; exit at once, the driver joins */
        pthread_mutex_unlock(&w->mu);
        return NULL;
    }
    for (;;) {
        while (!w->has_job && !w->quit)
            pthread_cond_wait(&w->cv, &w->mu);
        if (w->quit)
            break;
        pthread_mutex_unlock(&w->mu);
        w->p->run_ppu(w->p->ctx, &w->b, &w->t0, &w->t1);
        pthread_mutex_lock(&w->mu);
        w->has_job = 0, w->done = 1;
        pthread_cond_broadcast(&w->cv);
    }
    pthread_mutex_unlock(&w->mu);
    w->p->thread_end(w->p->ctx);
    return NULL;
}

/* 0 when the worker of case c is created and registered */
static int start_worker(const ob_drv_port *p, ob_worker *w, int c)
{
    int rc;
    memset(w, 0, sizeof *w);
    w->c = c, w->p = p;
    if ((rc = p->mutex_init(p->ctx, &w->mu)) != 0 || (rc = p->cond_init(p->ctx, &w->cv)) != 0) {
        lg(p, "drv: worker mutex/cond init failed", rc);
        return -1;
    }
    w->sync_ok = 1;
    if ((rc = p->thread_create(p->ctx, &w->h, worker_main, w)) != 0) {
        lg(p, "drv: worker create failed", rc);
        return -1;
    }
    w->created = 1;
    pthread_mutex_lock(&w->mu); /* the startup acknowledgement: the worker posts it first thing */
    while (!w->acked)
        pthread_cond_wait(&w->cv, &w->mu);
    rc = w->reg_rc;
    pthread_mutex_unlock(&w->mu);
    if (rc != 0)
        lg(p, "drv: worker registration failed", rc);
    return rc != 0 ? -1 : 0;
}

static int worker_of(int c)
{
    for (unsigned i = 0; i < OB_PPU_WORKERS; ++i)
        if (g_cases[i] == c)
            return (int)i;
    return -1;
}

static void post(ob_worker *w, const ob_block *b)
{
    pthread_mutex_lock(&w->mu);
    w->b = *b, w->done = 0, w->has_job = 1;
    pthread_cond_broadcast(&w->cv);
    while (!w->done)
        pthread_cond_wait(&w->cv, &w->mu);
    pthread_mutex_unlock(&w->mu);
}

/* every created worker: quit, join (checked).  Returns the number retained (join failed). */
static unsigned stop_workers(const ob_drv_port *p)
{
    unsigned retained = 0;
    for (unsigned i = 0; i < OB_PPU_WORKERS; ++i) {
        ob_worker *w = &g_w[i];
        if (!w->created)
            continue;
        pthread_mutex_lock(&w->mu);
        w->quit = 1;
        pthread_cond_broadcast(&w->cv);
        pthread_mutex_unlock(&w->mu);
        int rc = p->thread_join(p->ctx, w->h);
        if (rc != 0) {
            lg(p, "drv: worker join failed: worker retained", rc);
            ++retained;
        } else {
            w->joined = 1;
        }
    }
    return retained;
}

static void path(char *out, unsigned long n, const char *base, const char *suffix)
{
    snprintf(out, n, "%s%s", base, suffix);
}

void ob_drv_run(const ob_drv_port *p, ob_drv_result *res)
{
    char tmp[200], fin[200], bad[200];
    ob_fw csv, gcsv;
    ob_spu spu;
    ob_gpu gpu;
    int ok = 1, prof = 0, drain_thread = 0, spu_opened = 0, gpu_opened = 0, csv_open = 0, gcsv_open = 0;
    memset(res, 0, sizeof *res);
    res->rc = -1, res->spu_life = -1, res->gpu_life = -1;
    memset(g_w, 0, sizeof g_w);
    long nb = ob_schedule(p->seed, g_cases, g_cold, (unsigned)NCASES, p->r, g_sched, sizeof g_sched / sizeof g_sched[0]);
    if (nb < 0) {
        lg(p, "drv: schedule does not fit", -1);
        return;
    }
    res->planned = (unsigned long)nb;
    path(tmp, sizeof tmp, p->base, ".csv.tmp");
    if (ob_fw_open(&csv, p->io, tmp) != 0) {
        lg(p, "drv: CSV open failed", csv.err);
        return; /* nothing was created, nothing else was started */
    }
    csv_open = 1;
    /* ---- startup ---- */
    if (ob_csv_begin(&csv, p->seed, g_sched, (unsigned long)nb, p->tb_freq) != 0)
        ok = 0;
    if (ok) {
        int rc = p->prof_init(p->ctx);
        if (rc != 0) {
            lg(p, "drv: profiler init failed", rc);
            ok = 0;
        } else {
            prof = 1;
        }
    }
    if (ok) {
        int rc = p->thread_begin(p->ctx, "ob-drain"); /* the SPU lane's owner (owner_self) */
        if (rc != 0) {
            lg(p, "drv: drain thread registration failed", rc);
            ok = 0;
        } else {
            drain_thread = 1;
        }
    }
    if (ok) {
        spu_opened = 1;
        if (ob_spu_open(&spu, p->spu, p->reply_ea, p->spu_polls) != 0)
            ok = 0;
    }
    if (ok) {
        gpu_opened = 1;
        if (ob_gpu_open(&gpu, p->gpu, p->gpu_report_count, p->gpu_polls) != 0)
            ok = 0;
    }
    for (unsigned i = 0; ok && i < OB_PPU_WORKERS; ++i)
        if (start_worker(p, &g_w[i], g_cases[i]) != 0)
            ok = 0;
    /* ---- blocks ---- */
    for (long i = 0; ok && i < nb; ++i) {
        const ob_block *b = &g_sched[i];
        ob_row row;
        memset(&row, 0, sizeof row);
        int drc = p->drain(p->ctx); /* between blocks only */
        if (drc != 0) {
            lg(p, "drv: drain between blocks failed", drc);
            ok = 0;
            break;
        }
        int stop = 0;
        if (!ob_case_table[b->c].spu) {
            int k = worker_of(b->c);
            if (k < 0 || !g_w[k].created || g_w[k].reg_rc != 0) {
                row.rc = OB_RC_NOWORKER;
                stop = 1;
            } else {
                post(&g_w[k], b);
                row.t0 = g_w[k].t0, row.t1 = g_w[k].t1;
                res->invalid += row.t1 < row.t0;
                if (b->tu == OB_TU_ON) {
                    row.led = ob_planned(b->c);
                    if (b->kind == OB_BLK_WARM)
                        row.led.regs = b->c == OB_P_NEST8 ? 8u : (b->c == OB_P_HOT || b->c == OB_P_DROP) ? 1u : 0u;
                }
            }
        } else {
            ob_spu_reply o;
            int brc = ob_spu_block(&spu, b->c, b->tu == OB_TU_ON ? 0u : 1u, &o);
            row.counted = 1;
            row.t0 = o.w[0], row.t1 = o.w[1];
            row.led.sseq = o.w[3], row.led.tail_lost = o.w[4], row.led.staged = (int32_t)o.w[5];
            row.led.published = o.w[6], row.led.lost = o.w[7];
            row.ppu_t0 = o.ppu_t0, row.ppu_t1 = o.ppu_t1;
            row.dec_reset = o.dec_reset, row.spu_cmd = o.cmd, row.spu_echo = o.echo;
            row.rc = brc != 0 ? o.rc : (int)o.w[2];
            char why[160];
            int led_bad = 0;
            if (brc == 0 && b->tu == OB_TU_ON)
                led_bad = ob_ledger_check(b->c, &row.led, why, sizeof why) != 0;
            else if (brc == 0) /* OFF / OFF-b: no writer call, an all-zero ledger */
                led_bad = row.led.sseq || row.led.tail_lost || row.led.staged || row.led.published || row.led.lost;
            if (led_bad) {
                ++res->mismatches;
                lg(p, "drv: SPU ledger differs from the plan: run failed, no further block", (int)i);
            }
            if (brc == 0 && row.rc != 0)
                lg(p, "drv: SPU block returned a nonzero execution rc: run failed, no further block", row.rc);
            /* a transport failure, a nonzero execution rc or a ledger mismatch: the diagnostic row is written, the
             * blocks stop, the run fails (no receipt) */
            if (brc != 0 || row.rc != 0 || led_bad)
                stop = 1;
        }
        if (ob_csv_row(&csv, (unsigned long)i, b, &row) != 0) {
            ok = 0;
            break;
        }
        ++res->rows;
        if (stop) {
            ok = 0;
            break;
        }
    }
    /* ---- the GPU section ---- */
    if (ok) {
        path(tmp, sizeof tmp, p->base, ".gpu.csv.tmp");
        if (ob_fw_open(&gcsv, p->io, tmp) != 0) {
            ok = 0;
        } else {
            gcsv_open = 1;
            if (ob_gpu_run(&gpu, p->seed, p->r, &gcsv) != 0)
                ok = 0;
            res->gpu_rows = gpu.rows;
        }
    }
    /* ---- teardown ---- */
    res->workers_retained = (int)stop_workers(p);
    if (spu_opened)
        res->spu_life = ob_spu_close(&spu);
    if (gpu_opened)
        res->gpu_life = ob_gpu_close(&gpu);
    int retained = res->workers_retained || res->spu_life == OB_LIFE_RETAINED || res->gpu_life == OB_LIFE_RETAINED;
    if (res->workers_retained || (res->spu_life != -1 && res->spu_life != OB_LIFE_CLEAN) ||
        (res->gpu_life != -1 && res->gpu_life != OB_LIFE_CLEAN))
        ok = 0;
    if (prof && !retained) {
        int rc = p->drain(p->ctx);
        if (rc != 0) {
            lg(p, "drv: final drain failed", rc);
            ok = 0;
        }
        if (drain_thread)
            p->thread_end(p->ctx);
        rc = p->shutdown(p->ctx);
        if (rc != 0) {
            lg(p, "drv: profiler shutdown failed", rc);
            ok = 0;
        } else {
            res->profiler_shut_down = 1;
        }
    } else if (prof) {
        lg(p, "drv: a worker, producer, lane or domain is retained: the profiler is left running", 0);
        ok = 0;
    }
    /* ---- publication ---- */
    if (ob_csv_end(&csv, res->rows) != 0)
        ok = 0;
    if (csv_open && ob_fw_finish(&csv) != 0)
        ok = 0;
    if (gcsv_open && ob_fw_finish(&gcsv) != 0)
        ok = 0;
    /* both CSVs reach their final names together or neither does; a later failure moves them to .failed */
    char ctmp[200], cfin[200], cbad[200], gtmp[200], gfin[200], gbad[200];
    path(ctmp, sizeof ctmp, p->base, ".csv.tmp");
    path(cfin, sizeof cfin, p->base, ".csv");
    path(cbad, sizeof cbad, p->base, ".csv.failed");
    path(gtmp, sizeof gtmp, p->base, ".gpu.csv.tmp");
    path(gfin, sizeof gfin, p->base, ".gpu.csv");
    path(gbad, sizeof gbad, p->base, ".gpu.csv.failed");
    int g_final = 0, c_final = 0;
    if (ok && gcsv_open) {
        g_final = p->io->rename(p->io->ctx, gtmp, gfin) == 0;
        c_final = g_final && p->io->rename(p->io->ctx, ctmp, cfin) == 0;
        if (!g_final || !c_final)
            ok = 0;
    } else {
        ok = 0;
    }
    if (ok) {
        char line[160];
        snprintf(line, sizeof line, "BENCH_END rows=%lu invalid=%lu ledger_mismatches=%lu receipt=complete\n",
                 res->rows, res->invalid, res->mismatches);
        path(tmp, sizeof tmp, p->base, ".bench-end.tmp");
        path(fin, sizeof fin, p->base, ".bench-end.txt");
        path(bad, sizeof bad, p->base, ".bench-end.failed");
        if (ob_publish_text(p->io, tmp, fin, bad, line) != 0) {
            lg(p, "drv: receipt publication failed", -1);
            ok = 0;
        }
    }
    if (!ok) { /* the failed evidence apart from the accepted names (best effort) */
        (void)p->io->rename(p->io->ctx, g_final ? gfin : gtmp, gbad);
        (void)p->io->rename(p->io->ctx, c_final ? cfin : ctmp, cbad);
        return;
    }
    res->receipt_published = 1;
    res->rc = 0;
}
