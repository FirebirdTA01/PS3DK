/* Clock calibration probe: RAW observations of the SPU decrementer and the RSX timestamp, each bracketed between two
 * PPU timebase reads. It records only; any mapping is derived later, and this program makes no calibration claim.
 *
 * Bracket rule: tb0 is read before the request is issued (then sync); tb1 is read only after the exact completion
 * was observed, its payload or report was read under barriers and its identity rechecked (then sync; isync; mftb).
 *   SPU: tb0, mailbox request seq, poll the COMMIT line for seq, sync, copy the PAYLOAD line, sync, recheck commit
 *        and payload identity, tb1. The SPU writes payload, waits for the transfer, then writes commit.
 *   RSX: tb0, timestamp report + label(seq) queued and flushed, poll the label for seq, sync, read the report, sync,
 *        recheck the label, tb1.
 * One request is outstanding at a time. A timeout or identity failure STOPS that domain for the rest of the run (no
 * further request goes into a possibly outstanding one) and is recorded as such, never as a sample.
 *
 * Phases: SPU 0 (CAL_SPU_SAMPLES), RSX (CAL_RSX_SAMPLES), SPU 1; CAL_PERIOD_US apart.
 * Output: /dev_hdd0/tmp/clockcal-<timebase>.csv, columns kind,phase,index,tb0,tb1,value,note:
 *   meta (tb0 = timebase Hz, tb1 = system time us), spu samples (note "dec2=N": the SPU's second decrementer
 *   reading after a fixed spin, so a counter that does not count shows directly), rsx samples, spu-stop / rsx-stop / rsx-unavailable (note =
 *   the reason), cleanup (note = failed step or "ok", value = its code, tb0 = 1 if the SPU group was left running),
 *   end (tb0 = spu samples, tb1 = rsx samples, value = the run result, note = the first failure or "ok").
 * The 200 ms poll limit bounds polling only: no LV2 call, join or file write has a deadline here. */
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
#include "ledger.h"

SYS_PROCESS_PARAM(1001, 0x100000);

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

typedef struct {
    volatile uint32_t sequence;
    volatile uint32_t dec;
    volatile uint32_t dec2; /* the SPU's second reading after a fixed spin: counting check */
    uint32_t pad[29];
} __attribute__((aligned(128))) line;

static line payload, commit; /* separate cache lines, written only by the SPU's DMA */
static volatile int quit;
static uint64_t wait_ticks;
static FILE *out;
static int write_failed;
static unsigned spu_samples, rsx_samples;
static const char *first_failure = "ok";
static int run_rc;

/* tb0: the timebase, then a full barrier, so the read precedes the request that follows. */
static inline uint64_t tb_before(void)
{
    uint64_t t = __builtin_ppc_get_timebase();
    __asm__ volatile("sync" ::: "memory");
    return t;
}

/* tb1: a full barrier and a context synchronization first, so every load above completed before the read. */
static inline uint64_t tb_after(void)
{
    __asm__ volatile("sync\n\tisync" ::: "memory");
    return __builtin_ppc_get_timebase();
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

/* Returns 0 to continue, 1 if the SPU domain is stopped for the rest of the run. */
static int spu_phase(uint32_t thread, unsigned phase, uint32_t *sequence)
{
    for (unsigned i = 0; i < CAL_SPU_SAMPLES && !quit; ++i) {
        uint32_t seq = ++*sequence;
        uint64_t t0 = tb_before();
        int rc = sysSpuThreadWriteMb(thread, seq);
        if (rc) {
            row("spu-stop", phase, i, t0, 0, (uint64_t)(uint32_t)rc, "mailbox-write");
            return 1;
        }
        int seen = 0;
        for (;;) {
            if (commit.sequence == seq) {
                seen = 1;
                break;
            }
            if (__builtin_ppc_get_timebase() - t0 > wait_ticks)
                break;
        }
        if (!seen) {
            /* The request may still be answered later: never issue another one into it. */
            row("spu-stop", phase, i, t0, 0, seq, "timeout");
            return 1;
        }
        __asm__ volatile("sync" ::: "memory");
        uint32_t p_seq = payload.sequence, p_dec = payload.dec, p_dec2 = payload.dec2;
        __asm__ volatile("sync" ::: "memory");
        uint32_t c_seq = commit.sequence;
        uint64_t t1 = tb_after();
        if (p_seq != seq || c_seq != seq) {
            row("spu-stop", phase, i, t0, t1, ((uint64_t)p_seq << 32) | c_seq, "identity");
            return 1;
        }
        char spin_note[24];
        snprintf(spin_note, sizeof spin_note, "dec2=%u", (unsigned)p_dec2);
        row("spu", phase, i, t0, t1, p_dec, spin_note);
        ++spu_samples;
        if (cellSysutilCheckCallback() != 0)
            quit = 1;
        usleep(CAL_PERIOD_US);
    }
    return 0;
}

static void rsx_phase(void)
{
    volatile uint32_t *label = cellGcmGetLabelAddress(RSX_LABEL);
    if (!label) {
        row("rsx-unavailable", 0, 0, 0, 0, 0, "label-address");
        return;
    }
    *label = 0;
    __asm__ volatile("sync" ::: "memory");
    CellGcmContextData *ctx = CELL_GCM_CURRENT;
    for (unsigned i = 0; i < CAL_RSX_SAMPLES && !quit; ++i) {
        uint32_t want = i + 1;
        uint64_t t0 = tb_before();
        cellGcmSetTimeStamp(ctx, RSX_REPORT);
        cellGcmSetWriteBackEndLabel(ctx, RSX_LABEL, want);
        cellGcmFlush(ctx);
        int seen = 0;
        for (;;) {
            if (*label == want) {
                seen = 1;
                break;
            }
            if (__builtin_ppc_get_timebase() - t0 > wait_ticks)
                break;
        }
        if (!seen) {
            /* The queued report and label may still land later: the report is never reused after this. */
            row("rsx-stop", 0, i, t0, 0, want, "timeout");
            return;
        }
        __asm__ volatile("sync" ::: "memory");
        uint64_t value = cellGcmGetTimeStamp(RSX_REPORT);
        __asm__ volatile("sync" ::: "memory");
        uint32_t again = *label;
        uint64_t t1 = tb_after();
        if (again != want) {
            row("rsx-stop", 0, i, t0, t1, again, "identity");
            return;
        }
        row("rsx", 0, i, t0, t1, value, "");
        ++rsx_samples;
        if (cellSysutilCheckCallback() != 0)
            quit = 1;
        usleep(CAL_PERIOD_US);
    }
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
    char path[96];
    snprintf(path, sizeof path, "/dev_hdd0/tmp/clockcal-%016llx.csv", (unsigned long long)__builtin_ppc_get_timebase());
    int fd = open(path, O_WRONLY | O_CREAT | O_EXCL, 0600);
    if (fd < 0) {
        printf("CLOCKCAL_FAIL open %s\n", path);
        return 1;
    }
    out = fdopen(fd, "w");
    if (!out) {
        close(fd);
        printf("CLOCKCAL_FAIL fdopen %s\n", path);
        return 1;
    }
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
    int rc;
    if ((rc = cellSysutilRegisterCallback(0, on_sysutil, NULL)) != 0) {
        fail("sysutil-callback", rc);
        goto teardown;
    }
    if ((rc = sysSpuInitialize(6, 0)) != 0 || (rc = sysSpuImageImport(&image, clockcal_spu_bin, 0)) != 0) {
        fail("spu-image", rc);
        goto teardown;
    }
    led.image_open = 1;
    sysSpuThreadGroupAttribute gattr = {0};
    gattr.name = "clockcal";
    gattr.nsize = 9;
    if ((rc = sysSpuThreadGroupCreate(&group, 1, 100, &gattr)) != 0) {
        fail("spu-group", rc);
        goto teardown;
    }
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
    __asm__ volatile("sync" ::: "memory");
    if ((rc = sysSpuThreadInitialize(&thread, group, 0, &image, &tattr, &args)) != 0) {
        fail("spu-thread", rc);
        goto teardown;
    }
    if ((rc = sysSpuThreadGroupStart(group)) != 0) {
        fail("spu-start", rc);
        goto teardown;
    }
    led.group_started = 1;

    sys_addr_t host = 0;
    int gcm_ok = 0;
    if ((rc = gcm_host_buffer_alloc(1u << 20, &host)) != 0)
        row("rsx-unavailable", 0, 0, 0, 0, (uint64_t)(uint32_t)rc, "host-buffer");
    else if ((rc = cellGcmInit(64u << 10, 1u << 20, (void *)(uintptr_t)host)) != 0)
        row("rsx-unavailable", 0, 0, 0, 0, (uint64_t)(uint32_t)rc, "gcm-init");
    else
        gcm_ok = 1;

    int spu_stopped = spu_phase(thread, 0, &sequence);
    if (gcm_ok && !quit)
        rsx_phase();
    if (!spu_stopped && !quit)
        spu_phase(thread, 1, &sequence);
    if (quit)
        fail("exit-requested", 2);

teardown:;
    ledger_ops ops = {op_terminate, op_join, op_destroy, op_image_close, &lc};
    ledger_teardown(&led, &ops);
    row("cleanup", 0, 0, (uint64_t)led.left_running, 0, (uint64_t)(uint32_t)led.cleanup_rc,
        led.cleanup_rc ? led.cleanup_at : "ok");
    row("end", 0, 0, spu_samples, rsx_samples, (uint64_t)(uint32_t)run_rc, first_failure);
    int flush_failed = fflush(out) != 0 || fsync(fileno(out)) != 0;
    int close_failed = fclose(out) != 0; /* always attempted */
    int log_ok = !write_failed && !flush_failed && !close_failed;
    printf("CLOCKCAL_END run=%d first_failure=%s cleanup=%s left_running=%d spu=%u rsx=%u log=%s\n", run_rc,
           first_failure, led.cleanup_rc ? led.cleanup_at : "ok", led.left_running, spu_samples, rsx_samples,
           log_ok ? "ok" : "FAILED");
    return run_rc || led.cleanup_rc || !log_ok ? 1 : 0;
}
