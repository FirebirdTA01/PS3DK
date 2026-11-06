/* Localization probe W / W0 (design r4 2afa7bf3 + review 19:47): the c4 clock probe (samples/profiling/clock-
 * calibration at 0ae94745) PLUS checkpoint markers.  A CHANGED, INSTRUMENTED binary: never "c4 unchanged", and no
 * timing result is compared with c2/c4.  W_VARIANT_W0 omits the one decrementer write request (milestones 11/12);
 * W0 is a later, separately gated experiment.  UNVALIDATED ON TARGET.
 *
 * The measurement code, its CSV rows and their order are c4's (spu_request, spu_phase, spu_write_event, rsx_phase,
 * the ledger teardown, the cleanup/end rows), with these additions only:
 *  - the CLKW1 markers (w_marker.h), written ONLY between requests, never inside a tb0..tb1 bracket;
 *  - CSV checkpoints (fflush, then fsync only if fflush succeeded) after phases 08/10/12/14;
 *  - an experiment STOP before the next request when a CSV row write failed (detected before each request and at
 *    each checkpoint), a checkpoint failed, or a marker failed (logging): the remaining measurement stages are
 *    skipped, teardown and the CSV close still run, and the end row carries the failure (no synthetic clean end);
 *  - the classification of each stop for the markers: a mailbox-write error is a SUBMISSION WITH UNKNOWN OUTCOME
 *    (no LV2 contract here establishes "not enqueued"), a poll timeout or an identity mismatch an UNRESOLVED
 *    SUBMISSION (the request may still complete: nothing of that domain is re-armed or reused in this run).
 *
 * Positive evidence only: a valid marker shows execution reached its write; an absent marker proves nothing. */
#ifndef W_HOST_TEST
#include <cell/gcm.h>
#include <cell/sysutil.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/process.h>
#include <sys/spu.h>
#include <sys/sys_time.h>
#include <sys/systime.h>
#include <unistd.h>
#include "clockcal_spu_bin.h"
#include "gcm_host_buffer.h"
#define W_TB() __builtin_ppc_get_timebase()
#define W_SYNC() __asm__ volatile("sync" ::: "memory")
#define W_SYNC_ISYNC() __asm__ volatile("sync\n\tisync" ::: "memory")
#define W_DIR "/dev_hdd0/tmp"
#define W_MB_NO_ENQUEUE(rc) ((void)(rc), 0) /* no LV2 contract is known that proves a refused mailbox write */
SYS_PROCESS_PARAM(1001, 0x100000);
#else
#include "w_host.h" /* host tests: fakes for every LV2/GCM/sysutil call and a file-call seam */
#endif
#include "ledger.h"
#include "w_marker.h"

#ifndef CAL_SPU_SAMPLES
#define CAL_SPU_SAMPLES 1500
#endif
#ifndef CAL_RSX_SAMPLES
#define CAL_RSX_SAMPLES 1500
#endif
#ifndef CAL_PERIOD_US
#define CAL_PERIOD_US 10000
#endif
#define RSX_LABEL 241
#define RSX_REPORT 0
#define WAIT_US 200000

#ifdef W_VARIANT_W0
#define W_VARIANT "W0"
#else
#define W_VARIANT "W"
#endif

typedef struct {
    volatile uint32_t sequence;
    volatile uint32_t dec;
    volatile uint32_t dec2;  /* the SPU's second reading after a fixed spin: counting check */
    volatile uint32_t wrote; /* the value the SPU wrote to its decrementer for this request, or 0 */
    uint32_t pad[28];
} __attribute__((aligned(128))) line;

#define WRITE_FLAG 0x80000000u   /* a request that first writes the decrementer (spu/main.c) */
#define WRITE_VALUE 0xFFFFFFFFu  /* what the SPU writes; recorded and checked */

static line payload, commit; /* separate cache lines, written only by the SPU's DMA */
static volatile int quit;
static uint64_t wait_ticks;
static FILE *out;
static int write_failed;
static unsigned spu_samples, rsx_samples;
static const char *first_failure = "ok";
static int run_rc;

/* W: why the experiment stopped issuing requests (the first cause wins) */
enum { STOP_NONE, STOP_CSV_OPEN, STOP_SPU_SETUP, STOP_CSV, STOP_LOG };
static int stop_cause;
static int csv_bad; /* a CSV row write or checkpoint failed: no further checkpoint, final fflush/fsync not attempted */

/* tb0: the timebase, then a full barrier, so the read precedes the request that follows. */
static inline uint64_t tb_before(void)
{
    uint64_t t = W_TB();
    W_SYNC();
    return t;
}

/* tb1: a full barrier and a context synchronization first, so every load above completed before the read. */
static inline uint64_t tb_after(void)
{
    W_SYNC_ISYNC();
    return W_TB();
}

static void row(const char *kind, unsigned phase, unsigned index, uint64_t a, uint64_t b, uint64_t value,
                const char *note)
{
    if (fprintf(out, "%s,%u,%u,%llu,%llu,%llu,%s\n", kind, phase, index, (unsigned long long)a,
                (unsigned long long)b, (unsigned long long)value, note) < 0)
        write_failed = 1;
}

static void fail(const char *what, int rc)
{
    if (!run_rc) {
        run_rc = rc ? rc : -1;
        first_failure = what;
    }
}

static void on_sysutil(uint64_t status, uint64_t param, void *arg)
{
    (void)param, (void)arg;
    if (status == CELL_SYSUTIL_REQUEST_EXITGAME)
        quit = 1;
}

/* W: may another request be issued?  Detects a failed CSV row write and a marker logging failure BEFORE it. */
static int w_may_request(void)
{
    if (stop_cause == STOP_NONE && write_failed) {
        stop_cause = STOP_CSV;
        csv_bad = 1;
        fail("csv-row-write", -1);
    }
    if (stop_cause == STOP_NONE && w_logging_failed()) {
        stop_cause = STOP_LOG;
        fail("marker-logging", -1);
    }
    return stop_cause == STOP_NONE;
}

typedef struct {
    uint64_t t0, t1;
    uint32_t dec, dec2, wrote;
} spu_reply;

/* W: how a request ended (c4 wrote the same spu-stop rows) */
enum { REQ_OK, REQ_MB_REFUSED, REQ_MB_UNKNOWN, REQ_TIMEOUT, REQ_IDENTITY };
static int last_mb_rc;

/* One bracketed request (c4).  Returns REQ_OK with *r filled, or the stop kind after writing the spu-stop row: the
 * SPU domain is then stopped for the rest of the run (a possibly outstanding request is never followed by another). */
static int spu_request(uint32_t thread, uint32_t seq, unsigned phase, unsigned i, spu_reply *r)
{
    uint64_t t0 = tb_before();
    int rc = sysSpuThreadWriteMb(thread, seq);
    if (rc) {
        row("spu-stop", phase, i, t0, 0, (uint64_t)(uint32_t)rc, "mailbox-write");
        last_mb_rc = rc;
        if (W_MB_NO_ENQUEUE(rc))
            return REQ_MB_REFUSED;
        w_unres[0] = 'q'; /* the submission's outcome is unknown */
        return REQ_MB_UNKNOWN;
    }
    int seen = 0;
    for (;;) {
        if (commit.sequence == seq) {
            seen = 1;
            break;
        }
        if (W_TB() - t0 > wait_ticks)
            break;
    }
    if (!seen) {
        row("spu-stop", phase, i, t0, 0, seq, "timeout");
        w_unres[0] = 'u';
        return REQ_TIMEOUT;
    }
    W_SYNC();
    uint32_t p_seq = payload.sequence, p_dec = payload.dec, p_dec2 = payload.dec2, p_wrote = payload.wrote;
    W_SYNC();
    uint32_t c_seq = commit.sequence;
    uint64_t t1 = tb_after();
    uint32_t want_wrote = seq & WRITE_FLAG ? WRITE_VALUE : 0;
    if (p_seq != seq || c_seq != seq || p_wrote != want_wrote) {
        row("spu-stop", phase, i, t0, t1, ((uint64_t)p_seq << 32) | c_seq, "identity");
        w_unres[0] = 'u';
        return REQ_IDENTITY;
    }
    *r = (spu_reply){t0, t1, p_dec, p_dec2, p_wrote};
    return REQ_OK;
}

static void req_result(int k, char *buf, size_t n) /* the result text of a request outcome, into buf */
{
    switch (k) {
    case REQ_MB_REFUSED:
        snprintf(buf, n, "mbox-refused:%08x", (unsigned)(uint32_t)last_mb_rc);
        break;
    case REQ_MB_UNKNOWN:
        snprintf(buf, n, "mbox-error:%08x", (unsigned)(uint32_t)last_mb_rc);
        break;
    case REQ_TIMEOUT:
        snprintf(buf, n, "timeout");
        break;
    case REQ_IDENTITY:
        snprintf(buf, n, "identity");
        break;
    default:
        snprintf(buf, n, "ok");
        break;
    }
}

/* a phase's outcome for its done marker */
typedef struct {
    char result[32];
    unsigned issued; /* SUBMITTED requests (the mailbox write or the GCM flush returned) */
    uint32_t last;   /* the last submitted request's number (meaningful when issued >= 1) */
} phase_out;

/* c4's phase loop; returns 0 to continue, 1 if the SPU domain is stopped for the rest of the run */
static int spu_phase(uint32_t thread, unsigned phase, uint32_t *sequence, phase_out *po)
{
    snprintf(po->result, sizeof po->result, "ok");
    for (unsigned i = 0; i < CAL_SPU_SAMPLES && !quit; ++i) {
        spu_reply r;
        if (!w_may_request()) {
            snprintf(po->result, sizeof po->result, "csv-stopped");
            return 0;
        }
        int k = spu_request(thread, ++*sequence, phase, i, &r);
        if (k != REQ_MB_REFUSED && k != REQ_MB_UNKNOWN) {
            po->issued++;
            po->last = *sequence;
        }
        if (k != REQ_OK) {
            req_result(k, po->result, sizeof po->result);
            return 1;
        }
        char spin_note[24];
        snprintf(spin_note, sizeof spin_note, "dec2=%u", (unsigned)r.dec2);
        row("spu", phase, i, r.t0, r.t1, r.dec, spin_note);
        ++spu_samples;
        w_count[0] = spu_samples;
        if (cellSysutilCheckCallback() != 0)
            quit = 1;
        usleep(CAL_PERIOD_US);
    }
    if (po->issued < CAL_SPU_SAMPLES && quit)
        snprintf(po->result, sizeof po->result, "quit");
    return 0;
}

#ifndef W_VARIANT_W0
/* The one decrementer write (c3): it ends the unwritten epoch (phase 0) and starts the post-write epoch (phase 1).
 * Row "spu-write": value = the read right after the write, note "wrote=V dec2=N". Returns 1 if the domain stopped. */
static int spu_write_event(uint32_t thread, uint32_t *sequence, char *result, size_t rn, char *fields, size_t fn)
{
    spu_reply r;
    int k = spu_request(thread, ++*sequence | WRITE_FLAG, 1, 0, &r);
    if (k != REQ_OK) {
        req_result(k, result, rn);
        return 1;
    }
    char note[48];
    snprintf(note, sizeof note, "wrote=%u dec2=%u", (unsigned)r.wrote, (unsigned)r.dec2);
    row("spu-write", 1, 0, r.t0, r.t1, r.dec, note);
    snprintf(result, rn, "ok");
    snprintf(fields, fn, "dec=%08x dec2=%08x wrote=%08x ", (unsigned)r.dec, (unsigned)r.dec2, (unsigned)r.wrote);
    usleep(CAL_PERIOD_US);
    return 0;
}
#endif

static void rsx_phase(phase_out *po)
{
    snprintf(po->result, sizeof po->result, "ok");
    volatile uint32_t *label = cellGcmGetLabelAddress(RSX_LABEL);
    if (!label) {
        row("rsx-unavailable", 0, 0, 0, 0, 0, "label-address");
        snprintf(po->result, sizeof po->result, "unavailable:label-address");
        return;
    }
    *label = 0;
    W_SYNC();
    CellGcmContextData *ctx = CELL_GCM_CURRENT;
    for (unsigned i = 0; i < CAL_RSX_SAMPLES && !quit; ++i) {
        if (!w_may_request()) {
            snprintf(po->result, sizeof po->result, "csv-stopped");
            return;
        }
        uint32_t want = i + 1;
        uint64_t t0 = tb_before();
        cellGcmSetTimeStamp(ctx, RSX_REPORT);
        cellGcmSetWriteBackEndLabel(ctx, RSX_LABEL, want);
        cellGcmFlush(ctx);
        po->issued++;
        po->last = want;
        int seen = 0;
        for (;;) {
            if (*label == want) {
                seen = 1;
                break;
            }
            if (W_TB() - t0 > wait_ticks)
                break;
        }
        if (!seen) {
            /* The queued report and label may still land later: the report is never reused after this. */
            row("rsx-stop", 0, i, t0, 0, want, "timeout");
            w_unres[1] = 'u';
            snprintf(po->result, sizeof po->result, "timeout");
            return;
        }
        W_SYNC();
        uint64_t value = cellGcmGetTimeStamp(RSX_REPORT);
        W_SYNC();
        uint32_t again = *label;
        uint64_t t1 = tb_after();
        if (again != want) {
            row("rsx-stop", 0, i, t0, t1, again, "identity");
            w_unres[1] = 'u';
            snprintf(po->result, sizeof po->result, "identity");
            return;
        }
        row("rsx", 0, i, t0, t1, value, "");
        ++rsx_samples;
        w_count[1] = rsx_samples;
        if (cellSysutilCheckCallback() != 0)
            quit = 1;
        usleep(CAL_PERIOD_US);
    }
    if (po->issued < CAL_RSX_SAMPLES && quit)
        snprintf(po->result, sizeof po->result, "quit");
}

/* W: the CSV checkpoint after a phase: fflush, then fsync only if fflush succeeded; the first error is kept and
 * stops the experiment.  A row write failure seen here (after the phase's last request) stops it too. */
static void checkpoint(char *csv, size_t n)
{
    if (csv_bad || !out) {
        snprintf(csv, n, "not-attempted");
        return;
    }
    if (write_failed) {
        csv_bad = 1;
        if (stop_cause == STOP_NONE)
            stop_cause = STOP_CSV;
        fail("csv-row-write", -1);
        snprintf(csv, n, "row-write");
        return;
    }
    if (fflush(out) != 0) {
        snprintf(csv, n, "fflush:%08x", (unsigned)(uint32_t)errno);
    } else if (fsync(fileno(out)) != 0) {
        snprintf(csv, n, "fsync:%08x", (unsigned)(uint32_t)errno);
    } else {
        snprintf(csv, n, "ok");
        return;
    }
    csv_bad = 1;
    if (stop_cause == STOP_NONE)
        stop_cause = STOP_CSV;
    fail("csv-checkpoint", -1);
}

/* W: the skip reason of a measurement stage that does not run (precedence: the experiment-stopping cause, then
 * W0's omission of 12, then GCM unavailable for 10, then the SPU domain stop for 12/14, then quit) */
static const char *skip_reason(unsigned stage, int gcm_ok, int spu_stopped)
{
    switch (stop_cause) {
    case STOP_CSV_OPEN:
    case STOP_CSV:
        return "skipped:csv-failed";
    case STOP_SPU_SETUP:
        return "skipped:spu-setup-failed";
    case STOP_LOG:
        return "skipped:logging-failed"; /* never written: markers ended with the 99 */
    default:
        break;
    }
#ifdef W_VARIANT_W0
    if (stage == 12)
        return "skipped:variant-w0";
#endif
    if (stage == 10 && !gcm_ok)
        return "skipped:gcm-unavailable";
    if ((stage == 12 || stage == 14) && spu_stopped)
        return "skipped:spu-stopped";
    if (quit)
        return "skipped:quit";
    return NULL; /* the stage runs */
}

static void done_marker(unsigned seq, const char *stage, const phase_out *po, const char *csv)
{
    char f[96];
    if (po->issued)
        snprintf(f, sizeof f, "issued=%u last=%08x csv=%s", po->issued, (unsigned)po->last, csv);
    else
        snprintf(f, sizeof f, "issued=0 csv=%s", csv);
    w_mark(seq, stage, po->result, f);
}

static void skipped_phase(unsigned seq, const char *stage, const char *reason)
{
    phase_out po = {{0}, 0, 0};
    snprintf(po.result, sizeof po.result, "%s", reason);
    done_marker(seq, stage, &po, "not-attempted");
}

/* LV2 adapters for the ledger. */
typedef struct {
    sysSpuImage *image;
    uint32_t group;
} lv2_ctx;
static int op_terminate(void *c) { return sysSpuThreadGroupTerminate(((lv2_ctx *)c)->group, 0); }
static int op_join(void *c)
{
    uint32_t cause = 0, status = 0;
    return sysSpuThreadGroupJoin(((lv2_ctx *)c)->group, &cause, &status);
}
static int op_destroy(void *c) { return sysSpuThreadGroupDestroy(((lv2_ctx *)c)->group); }
static int op_image_close(void *c) { return sysSpuImageClose(((lv2_ctx *)c)->image); }

int main(void)
{
    char path[512], res[48], fields[160]; /* c4: path[96]; widened so a long host test directory cannot truncate */
    w_run = W_TB();
    w_variant = W_VARIANT;
    w_mark(1, "main-entered", "ok", "");

    char f2[24] = "";
    int pn = snprintf(path, sizeof path, W_DIR "/clockcal-%016llx.csv", (unsigned long long)w_run);
    if (pn < 0 || (size_t)pn >= sizeof path) { /* never open a truncated name */
        snprintf(res, sizeof res, "error:format:path");
        printf("CLOCKCAL_FAIL path\n");
    } else {
        int fd = open(path, O_WRONLY | O_CREAT | O_EXCL, 0600);
        if (fd < 0) {
            snprintf(res, sizeof res, "error:open:%08x", (unsigned)(uint32_t)errno);
            printf("CLOCKCAL_FAIL open %s\n", path);
        } else {
            out = fdopen(fd, "w");
            if (!out) {
                snprintf(res, sizeof res, "error:fdopen:%08x", (unsigned)(uint32_t)errno);
                if (close(fd) != 0) /* c4 closes the raw fd unchecked; W records the result */
                    snprintf(f2, sizeof f2, "rawclose=%08x", (unsigned)(uint32_t)errno);
                else
                    snprintf(f2, sizeof f2, "rawclose=ok");
                printf("CLOCKCAL_FAIL fdopen %s\n", path);
            }
        }
    }
    if (!out) { /* c4 returns here; W first records the stages that cannot run */
        w_mark(2, "csv-opened", res, f2);
        stop_cause = STOP_CSV_OPEN;
        w_mark(4, "spu-setup-done", "skipped:csv-failed", "");
        w_mark(6, "gcm-setup-done", "skipped:csv-failed", "");
        skipped_phase(8, "phase0-done", "skipped:csv-failed");
        skipped_phase(10, "rsx-done", "skipped:csv-failed");
        w_mark(12, "write-req-done", "skipped:csv-failed", "csv=not-attempted");
        skipped_phase(14, "phase1-done", "skipped:csv-failed");
        w_mark(16, "teardown-done", "skipped:csv-failed", "left_running=0 spu_join=not-started");
        w_mark(17, "csv-closed", "skipped:csv-failed", "fflush=not-attempted fsync=not-attempted fclose=not-attempted");
        return 1;
    }
    w_mark(2, "csv-opened", "ok", "");
    uint64_t hz = sysGetTimebaseFrequency();
    wait_ticks = hz / 1000000u * WAIT_US;
    if (fprintf(out, "kind,phase,index,tb0,tb1,value,note\n") < 0)
        write_failed = 1;
    row("meta", 0, 0, hz, (uint64_t)sys_time_get_system_time(), 0, "");
    printf("CLOCKCAL_BEGIN csv=%s tb_hz=%llu\n", path, (unsigned long long)hz);

    sysSpuImage image;
    uint32_t group = 0, thread = 0, sequence = 0;
    ledger led = {0};
    lv2_ctx lc = {&image, 0};
    int rc, gcm_ok = 0, spu_stopped = 0, started = 0;
    phase_out po;
    char csv[32];
    w_mark(3, "spu-setup-begin", "ok", "");
    const char *call = NULL;
    if ((rc = cellSysutilRegisterCallback(0, on_sysutil, NULL)) != 0) {
        fail("sysutil-callback", rc);
        call = "sysutil-register";
    } else if ((rc = sysSpuInitialize(6, 0)) != 0) { /* c4 chains these two in one condition: same calls, order */
        fail("spu-image", rc);
        call = "init";
    } else if ((rc = sysSpuImageImport(&image, clockcal_spu_bin, 0)) != 0) {
        fail("spu-image", rc);
        call = "import";
    } else {
        led.image_open = 1;
        sysSpuThreadGroupAttribute gattr = {0};
        gattr.name = "clockcal";
        gattr.nsize = 9;
        if ((rc = sysSpuThreadGroupCreate(&group, 1, 100, &gattr)) != 0) {
            fail("spu-group", rc);
            call = "group-create";
        } else {
            lc.group = group;
            led.group_created = 1;
            sysSpuThreadAttribute tattr = {0};
            tattr.name = "clockcal-spu";
            tattr.nsize = 13;
            sysSpuThreadArgument args = {0};
            memset((void *)&payload, 0, sizeof payload);
            memset((void *)&commit, 0, sizeof commit);
            args.arg0 = (uint64_t)(uintptr_t)&payload;
            args.arg1 = (uint64_t)(uintptr_t)&commit;
            W_SYNC();
            if ((rc = sysSpuThreadInitialize(&thread, group, 0, &image, &tattr, &args)) != 0) {
                fail("spu-thread", rc);
                call = "thread-init";
            } else if ((rc = sysSpuThreadGroupStart(group)) != 0) {
                fail("spu-start", rc);
                call = "group-start";
            } else {
                led.group_started = 1;
                started = 1;
            }
        }
    }
    if (call) { /* c4: goto teardown */
        snprintf(res, sizeof res, "error:%s:%08x", call, (unsigned)(uint32_t)rc);
        w_mark(4, "spu-setup-done", res, "");
        stop_cause = STOP_SPU_SETUP;
        w_mark(6, "gcm-setup-done", "skipped:spu-setup-failed", "");
        skipped_phase(8, "phase0-done", skip_reason(8, 0, 0));
        skipped_phase(10, "rsx-done", skip_reason(10, 0, 0));
        w_mark(12, "write-req-done", skip_reason(12, 0, 0), "csv=not-attempted");
        skipped_phase(14, "phase1-done", skip_reason(14, 0, 0));
        goto teardown;
    }
    w_mark(4, "spu-setup-done", "ok", "");

    w_mark(5, "gcm-setup-begin", "ok", "");
    sys_addr_t host = 0;
    if ((rc = gcm_host_buffer_alloc(1u << 20, &host)) != 0) {
        row("rsx-unavailable", 0, 0, 0, 0, (uint64_t)(uint32_t)rc, "host-buffer");
        snprintf(res, sizeof res, "unavailable:host-buffer:%08x", (unsigned)(uint32_t)rc);
    } else if ((rc = cellGcmInit(64u << 10, 1u << 20, (void *)(uintptr_t)host)) != 0) {
        row("rsx-unavailable", 0, 0, 0, 0, (uint64_t)(uint32_t)rc, "gcm-init");
        snprintf(res, sizeof res, "unavailable:gcm-init:%08x", (unsigned)(uint32_t)rc);
    } else {
        gcm_ok = 1;
        snprintf(res, sizeof res, "ok");
    }
    w_mark(6, "gcm-setup-done", res, "");

    /* phase 0: the unwritten epoch (it always starts: no exit request can be seen before it) */
    w_mark(7, "phase0-begin", "ok", "");
    memset(&po, 0, sizeof po);
    spu_stopped = spu_phase(thread, 0, &sequence, &po);
    checkpoint(csv, sizeof csv);
    done_marker(8, "phase0-done", &po, csv);

    const char *why = skip_reason(10, gcm_ok, spu_stopped);
    if (!why && !w_may_request())
        why = skip_reason(10, gcm_ok, spu_stopped);
    if (why) {
        skipped_phase(10, "rsx-done", why);
    } else {
        w_mark(9, "rsx-begin", "ok", "");
        memset(&po, 0, sizeof po);
        rsx_phase(&po);
        checkpoint(csv, sizeof csv);
        done_marker(10, "rsx-done", &po, csv);
    }

    why = skip_reason(12, gcm_ok, spu_stopped);
    if (!why && !w_may_request())
        why = skip_reason(12, gcm_ok, spu_stopped);
    if (why) {
        w_mark(12, "write-req-done", why, "csv=not-attempted");
    } else {
#ifndef W_VARIANT_W0
        w_mark(11, "write-req-begin", "ok", ""); /* BEFORE the mailbox write */
        if (w_may_request()) {                   /* a failed 11 stops the experiment before the request */
            fields[0] = '\0';
            spu_stopped = spu_write_event(thread, &sequence, res, sizeof res, fields, sizeof fields);
            checkpoint(csv, sizeof csv);
            snprintf(fields + strlen(fields), sizeof fields - strlen(fields), "csv=%s", csv);
            w_mark(12, "write-req-done", res, fields);
        }
#endif
    }

    why = skip_reason(14, gcm_ok, spu_stopped);
    if (!why && !w_may_request())
        why = skip_reason(14, gcm_ok, spu_stopped);
    if (why) {
        skipped_phase(14, "phase1-done", why);
    } else {
        w_mark(13, "phase1-begin", "ok", "");
        memset(&po, 0, sizeof po);
        spu_phase(thread, 1, &sequence, &po); /* the post-write epoch: never fitted together with phase 0 */
        checkpoint(csv, sizeof csv);
        done_marker(14, "phase1-done", &po, csv);
    }
    if (quit)
        fail("exit-requested", 2);

teardown:;
    w_mark(15, "teardown-begin", "ok", "");
    ledger_ops ops = {op_terminate, op_join, op_destroy, op_image_close, &lc};
    ledger_teardown(&led, &ops);
    row("cleanup", 0, 0, (uint64_t)led.left_running, 0, (uint64_t)(uint32_t)led.cleanup_rc,
        led.cleanup_rc ? led.cleanup_at : "ok");
    if (w_logging_failed()) /* a marker failure up to 15 reaches the end row (16/17 come after it: rc only) */
        fail("marker-logging", -1);
    row("end", 0, 0, spu_samples, rsx_samples, (uint64_t)(uint32_t)run_rc, first_failure);
    {
        const char *join = !started ? "not-started"
                           : !strcmp(led.cleanup_at, "terminate") ? "not-attempted"
                           : !strcmp(led.cleanup_at, "join") ? "failed"
                                                               : "ok";
        if (led.cleanup_rc)
            snprintf(res, sizeof res, "error:%s:%08x", led.cleanup_at, (unsigned)(uint32_t)led.cleanup_rc);
        else
            snprintf(res, sizeof res, "ok");
        snprintf(fields, sizeof fields, "left_running=%d spu_join=%s", led.left_running, join);
        w_mark(16, "teardown-done", res, fields);
    }
    /* the final CSV close: fflush, then fsync only if fflush succeeded (neither after an earlier CSV failure), then
     * fclose exactly once */
    char ff[16] = "not-attempted", fs[16] = "not-attempted", fc[16];
    int flush_failed = 0;
    if (!csv_bad) {
        if (fflush(out) != 0) {
            snprintf(ff, sizeof ff, "%08x", (unsigned)(uint32_t)errno);
            flush_failed = 1;
        } else {
            snprintf(ff, sizeof ff, "ok");
            if (fsync(fileno(out)) != 0) {
                snprintf(fs, sizeof fs, "%08x", (unsigned)(uint32_t)errno);
                flush_failed = 1;
            } else {
                snprintf(fs, sizeof fs, "ok");
            }
        }
    }
    int close_failed = fclose(out) != 0; /* always attempted, once */
    if (close_failed)
        snprintf(fc, sizeof fc, "%08x", (unsigned)(uint32_t)errno);
    else
        snprintf(fc, sizeof fc, "ok");
    int log_ok = !write_failed && !flush_failed && !close_failed && !csv_bad;
    snprintf(fields, sizeof fields, "fflush=%s fsync=%s fclose=%s", ff, fs, fc);
    w_mark(17, "csv-closed", log_ok ? "ok" : "error", fields);
    printf("CLOCKCAL_END run=%d first_failure=%s cleanup=%s left_running=%d spu=%u rsx=%u log=%s\n", run_rc,
           first_failure, led.cleanup_rc ? led.cleanup_at : "ok", led.left_running, spu_samples, rsx_samples,
           log_ok ? "ok" : "FAILED");
    return run_rc || led.cleanup_rc || !log_ok || w_logging_failed() ? 1 : 0;
}
