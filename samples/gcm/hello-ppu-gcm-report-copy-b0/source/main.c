/* B0 report-object copy experiment. No production profiler instrumentation.
 * See README for exact claims, controls, setup provenance and exit policy. */
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stddef.h>
#include <stdarg.h>
#include <malloc.h>
#include <unistd.h>
#include <ppu-lv2.h>
#include <ppu-types.h>
#include <sys/process.h>
#include <sys/sys_time.h>
#include <cell/gcm.h>
#include <Cg/cg.h>
#include "../../common/gcm_host_buffer.h"
#include "vpshader_vpo.h"
#include "fpshader_fpo.h"
#include "../../hello-ppu-gcm-report-ring/source/probe_logic.h"
#include "b0_logic.h"
#include "../../hello-ppu-gcm-report-ring/source/state_tour_setup.h"

SYS_PROCESS_PARAM(1001, 0x100000);
enum { LABEL_REPORT = 110, LABEL_COPY = 111, LABEL_PUBLISH = 112,
       LABEL_GATE = 113, LABEL_FINAL = 114, ARENA_BYTES = 1 << 20,
       DEST0 = 128, BATCH_WORDS = 6000 };
_Static_assert(B0_REPORT_INDEX >= CELL_GCM_REPORT_APP_FIRST && B0_REPORT_INDEX+1 <= CELL_GCM_REPORT_APP_LAST, "app reports");
_Static_assert(LABEL_REPORT >= CELL_GCM_LABEL_APP_FIRST && LABEL_FINAL <= CELL_GCM_LABEL_APP_LAST, "app labels");
_Static_assert(sizeof(CellGcmReportData)==16, "report layout");
static CellGcmContextData *ctx;
static volatile CellGcmControl *control;
static uint64_t session_start;
static uint32_t serial, reference_serial;
static volatile uint32_t *gate_label;
static uint32_t gate_highest;
static int idle = 1;
static volatile uint8_t *arena;
static uint32_t arena_offset;

static void sync_memory(void) { __asm__ volatile("sync" ::: "memory"); }
static uint64_t now_us(void) { return (uint64_t)sys_time_get_system_time(); }
static uint64_t elapsed_us(void) {
    uint64_t now = now_us();
    return now >= session_start ? now-session_start : UINT64_MAX;
}
static void log_line(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void log_line(const char *fmt, ...) {
    char line[768]; va_list ap;
    va_start(ap, fmt); vsnprintf(line, sizeof line, fmt, ap); va_end(ap);
    printf("%s\n", line); fflush(stdout);
    FILE *f = fopen("/dev_hdd0/tmp/report-copy-b0.crumb", "a");
    if (f) { fprintf(f, "%s\n", line); if (!fflush(f)) (void)fsync(fileno(f)); fclose(f); }
}
static unsigned release_gates(void) {
    /* LABEL_GATE is the only PPU-owned RSX wait. Batches start idle, so only
     * one target can be outstanding. Keep the highest target even after a
     * normal release: every abort may safely repeat this PPU-only store. */
    if (!gate_highest) return 0;
    *gate_label = gate_highest;
    sync_memory();
    return 1;
}
static void recovery_required(const char *where) __attribute__((noreturn));
static void recovery_required(const char *where) {
    unsigned released = release_gates();
    log_line("B0_RECOVERY_REQUIRED session=%"PRIu64" elapsed=%"PRIu64" at=%s gates_released=%u no_retry=1 no_normal_exit=1",
             session_start, elapsed_us(), where, released);
    /* Terminal operator recovery state, explicitly outside measurement waits.
     * Do not return, exitspawn, free mapped buffers, or issue any GPU command.
     * An injected run-hook watchdog can still override this at its own timeout. */
    for (;;) usleep(1000000);
}
static uint32_t next_serial(void) {
    if (serial >= 0x7ffffffeu) recovery_required("serial-exhausted");
    return ++serial;
}
static void reserve_batch(void) {
    /* Callback, if needed, runs while the previous batch is known idle. The
     * whole batch fits before a GPU gate can be queued; no mid-gate callback. */
    cellGcmReserveMethodSize(ctx, BATCH_WORDS);
    /* Context members are firmware-width pointers even in an LP64 app. */
    uint32_t *end = ctx->end, *current = ctx->current;
    if ((uintptr_t)end < (uintptr_t)current ||
        ((uintptr_t)end-(uintptr_t)current)/4 < BATCH_WORDS)
        recovery_required("command-room");
}
static void backend_gate(unsigned label, uint32_t target) {
    cellGcmSetWriteBackEndLabel(ctx, (uint8_t)label, target);
    cellGcmSetWaitLabel(ctx, (uint8_t)label, target);
    idle = 0;
}
static int await_label(unsigned label, uint32_t target, uint64_t session_limit) {
    uint64_t start = now_us();
    for (;;) {
        uint64_t now = now_us();
        uint32_t got = *(volatile uint32_t *)cellGcmGetLabelAddress((uint8_t)label);
        int decision = rrp_wait(elapsed_us(), session_limit, got == target);
        if (decision == RRP_EXPIRED || now < start || now-start >= RRP_WAIT_US) return 0;
        if (got > target) return 0; /* future is not completion of this generation */
        if (decision == RRP_DONE) { sync_memory(); return 1; }
        usleep(10);
    }
}
static void emit_final(uint32_t *fence, uint32_t *ref) {
    *fence = next_serial();
    if (reference_serial >= 0x7ffffffeu) recovery_required("reference-exhausted");
    *ref = ++reference_serial;
    backend_gate(LABEL_FINAL, *fence);
    /* Same reference command used by cellGcmFinish, with a bounded CPU wait. */
    cellGcmSetReferenceCommand(ctx, *ref);
}
static void await_final(uint32_t fence, uint32_t ref) {
    if (!await_label(LABEL_FINAL, fence, RRP_DRAIN_US)) recovery_required("final-label");
    uint64_t start = now_us();
    for (;;) {
        uint64_t now = now_us();
        int passed = control->ref == ref && control->get == control->put;
        int decision = rrp_wait(elapsed_us(), RRP_DRAIN_US, passed);
        if (decision == RRP_EXPIRED || now < start || now-start >= RRP_WAIT_US)
            recovery_required("final-reference");
        if (decision == RRP_DONE) { sync_memory(); idle = 1; return; }
        usleep(10);
    }
}
static void quiet(void) {
    uint32_t fence, ref;
    emit_final(&fence, &ref);
    cellGcmFlush(ctx);
    await_final(fence, ref);
}
static void snapshot(unsigned char *dst, const volatile uint8_t *src, size_t n) {
    sync_memory();
    for (size_t i = 0; i < n; ++i) dst[i] = src[i];
    sync_memory();
}
static int end_run(int rc, const char *reason) {
    unsigned released = release_gates();
    if (!idle) quiet();
    log_line("B0_END session=%"PRIu64" rc=%d reason=%s gates_released=%u idle=1 elapsed=%"PRIu64,
             session_start, rc, reason, released, elapsed_us());
    /* No frees: GCM mappings remain owned until process teardown. No flips or
     * zcull bindings are issued by this probe, so none need retirement here. */
    return rc;
}
static void reset_destination(uint32_t sentinel) {
    if (!idle) recovery_required("reset-before-idle");
    for (unsigned i=0;i<ARENA_BYTES/4;i++) ((volatile uint32_t *)arena)[i]=sentinel;
    sync_memory();
}
static void log_bytes(const char *name,const unsigned char *bytes) {
    char hex[33];
    for (unsigned i=0;i<16;i++) (void)snprintf(hex+i*2,3,"%02x",bytes[i]);
    log_line("B0_BYTES %s=%s",name,hex);
}
int main(int argc,const char **argv) {
    (void)argc;(void)argv;
    session_start=now_us();
    log_line("B0_BEGIN session=%"PRIu64" report=%u bytes=16 candidates=2 deadline_us=%"PRIu64,
             session_start,B0_REPORT_INDEX,RRP_SESSION_US);
    void *host=memalign(1024*1024,HOST_HEAP_SIZE);
    if (!host || !init_rsx(host)) {
        log_line("B0_SETUP_FAILED init gates_released=%u",release_gates());return 3;
    }
    ctx=CELL_GCM_CURRENT;control=cellGcmGetControlRegister();
    if (!ctx || !control) recovery_required("missing-context");
    for (unsigned label=LABEL_REPORT; label <= LABEL_FINAL; ++label)
        *(volatile uint32_t *)cellGcmGetLabelAddress((uint8_t)label) = 0;
    sync_memory();
    for (unsigned label=LABEL_REPORT; label <= LABEL_FINAL; ++label)
        if (*(volatile uint32_t *)cellGcmGetLabelAddress((uint8_t)label)) recovery_required("label-reset");
    gate_label=(volatile uint32_t *)cellGcmGetLabelAddress(LABEL_GATE);
    uint32_t initial_ref=control->ref;
    reference_serial=initial_ref==UINT32_MAX ? 0:initial_ref;
    init_shaders();init_geometry();
    reserve_batch();env_reset(ctx);idle=0;
    const volatile uint8_t *source=(const volatile uint8_t *)cellGcmGetReportDataAddressLocation(B0_REPORT_INDEX,CELL_GCM_LOCATION_LOCAL);
    const volatile uint8_t *after=(const volatile uint8_t *)cellGcmGetReportDataAddressLocation(B0_REPORT_INDEX+1,CELL_GCM_LOCATION_LOCAL);
    if (!source || (uintptr_t)after!=(uintptr_t)source+16) return end_run(3,"report-map-invalid");
    sys_addr_t allocation=0;
    if (gcm_host_buffer_alloc(ARENA_BYTES,&allocation) ||
        cellGcmMapMainMemory((void *)(uintptr_t)allocation,ARENA_BYTES,&arena_offset))
        return end_run(3,"readback-allocation");
    if (arena_offset>UINT32_MAX-ARENA_BYTES) return end_run(3,"readback-offset-overflow");
    arena=(volatile uint8_t *)(uintptr_t)allocation;
    quiet();
    unsigned char previous[16],reference[32],expected[16],result[16];
    /* CPU reads only: no report-space sentinel writes. Change from the old
     * timestamp, ordered pair and a completed final fence validate freshness. */
    snapshot(previous,source,16);
    reserve_batch();
    cellGcmSetReportLocation(ctx,CELL_GCM_LOCATION_LOCAL);
    cellGcmSetClearColor(ctx,0xff000000u);
    cellGcmSetClearSurface(ctx,GCM_CLEAR_R|GCM_CLEAR_G|GCM_CLEAR_B|GCM_CLEAR_A);
    cellGcmSetTimeStamp(ctx,B0_REPORT_INDEX);
    cellGcmSetDrawArrays(ctx,GCM_TYPE_QUADS,0,4);
    cellGcmSetTimeStamp(ctx,B0_REPORT_INDEX+1);idle=0;quiet();
    snapshot(reference,source,32);
    uint64_t begin,end,old_begin;
    memcpy(&begin,reference,8);memcpy(&end,reference+16,8);memcpy(&old_begin,previous,8);
    uint32_t pixel=*(volatile uint32_t *)(g_tile+32*TILE_W+32);
    log_line("B0_BASELINE pixel=0x%08x begin=%"PRIu64" end=%"PRIu64" labels_reset=5",pixel,begin,end);
    log_line("B0_SOURCE report=%u cpu_ea=0x%"PRIxPTR" input_object=0x66626660 output_object=0xfeed0001",
             B0_REPORT_INDEX,(uintptr_t)source);
    log_bytes("reference",reference);
    if (pixel!=0xffffffffu || !begin || end<=begin || begin==old_begin)
        return end_run(3,"baseline-invalid");
    if (elapsed_us()>=RRP_SESSION_US) return end_run(3,"deadline");
    /* Preserve the proven ordinary local -> MAIN copy prerequisite. */
    reset_destination(0xa5a50000u);reserve_batch();
    cellGcmSetTransferData(ctx,CELL_GCM_TRANSFER_LOCAL_TO_MAIN,arena_offset+DEST0,
                           16,g_tile_offset,16,16,1);idle=0;quiet();
    snapshot(result,arena+DEST0,16);
    for (unsigned i=0;i<16;i++) if (result[i]!=0xff) return end_run(3,"ordinary-copy-invalid");
    if (!b0_guards((const volatile uint32_t *)arena,ARENA_BYTES,DEST0,0xa5a50000u))
        return end_run(3,"ordinary-copy-guard");
    log_line("B0_BASELINE ordinary_local_copy=OK idle=1");
    /* Sensitive omitted-copy control, before any experimental source binding. */
    reset_destination(0xa5a51000u);reserve_batch();quiet();
    snapshot(result,arena+DEST0,16);
    int caught=!b0_equal(result,reference);
    log_line("B0_CONTROL omitted_copy caught=%d idle=1",caught);
    if (!caught || !b0_guards((const volatile uint32_t *)arena,ARENA_BYTES,DEST0,0xa5a51000u))
        return end_run(2,"control-inconclusive");
    for (unsigned candidate=0; candidate < 2; ++candidate) {
        if (elapsed_us()>=RRP_SESSION_US) return end_run(3,"deadline");
        uint32_t sentinel=0xa5a52000u+candidate;
        reset_destination(sentinel);
        /* One small CPU report read before each candidate; no re-write/reuse. */
        snapshot(expected,source,16);
        if (!b0_equal(expected,reference)) return end_run(3,"reference-changed");
        log_line("B0_TRIAL_BEGIN candidate=%u source_offset=0x%08x destination_offset=0x%08x sentinel=0x%08x",
                 candidate+1,b0_offset(candidate),arena_offset+DEST0,sentinel);
        reserve_batch();
        if (elapsed_us()>=RRP_SESSION_US) return end_run(3,"deadline-before-copy");
        uint32_t *p=ctx->current;
        b0_packet(p,arena_offset+DEST0,b0_offset(candidate));ctx->current=p+12;idle=0;
        quiet(); /* A fault/timeout is terminal; candidate2 is never queued then. */
        snapshot(result,arena+DEST0,16);
        int guard=b0_guards((const volatile uint32_t *)arena,ARENA_BYTES,DEST0,sentinel);
        int equal=b0_equal(result,expected);
        int decision=b0_decide(idle,guard,1,equal);
        log_bytes("copied",result);
        log_line("B0_TRIAL candidate=%u exact=%d guards=%d idle=%d",candidate+1,equal,guard,idle);
        if (decision==B0_STOP) return end_run(3,"candidate-invalid");
        if (decision==B0_MATCH) {
            log_line("B0_MATCH candidate=%u source_offset=0x%08x bytes=16 ordering_claims=UNTESTED",
                     candidate+1,b0_offset(candidate));
            return end_run(0,"exact-copy");
        }
    }
    return end_run(2,"no-candidate-match");
}
