/* A1/A2/B2 report-object ordering experiments. No production profiler instrumentation.
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
#include "../../hello-ppu-gcm-report-copy-b0/source/b0_logic.h"
#include "../../hello-ppu-gcm-report-ring/source/state_tour_setup.h"

#include "ordering_logic.h"
#if ORDERING_CLAIM == 0
#define ORD_NAME "A1"
#elif ORDERING_CLAIM == 1
#define ORD_NAME "A2"
#elif ORDERING_CLAIM == 2
#define ORD_NAME "B2"
#else
#error invalid ordering claim
#endif
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
    FILE *f = fopen("/dev_hdd0/tmp/report-copy-" ORD_NAME ".crumb", "a");
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
    log_line("ORD_RECOVERY_REQUIRED session=%"PRIu64" elapsed=%"PRIu64" at=%s gates_released=%u no_retry=1 no_normal_exit=1",
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
    log_line("ORD_END session=%"PRIu64" rc=%d reason=%s gates_released=%u idle=1 elapsed=%"PRIu64,
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
    log_line("ORD_BYTES %s=%s",name,hex);
}
enum { ORD_MAX_PAIRS=126, DEST1=8192 };
static unsigned char old_bytes[4032],new_bytes[4032],settled[4032],immediate[4032],copied_new[4032];
static unsigned char report_guards[128];
static const volatile uint8_t *report_base;
static const volatile uint8_t *report_ptr(unsigned index) { return report_base+(index-512)*16; }
static void save_report_guards(void) {
    snapshot(report_guards,report_ptr(512),64);
    snapshot(report_guards+64,report_ptr(1020),64);
}
static int report_guards_ok(void) {
    unsigned char observed[128];
    snapshot(observed,report_ptr(512),64);snapshot(observed+64,report_ptr(1020),64);
    return ord_equal(observed,report_guards,128);
}
static void write_reports(unsigned index,unsigned pairs) {
    idle=0;
    for(unsigned i=0;i<pairs;i++) {
        cellGcmSetTimeStamp(ctx,index+2*i);
        cellGcmSetDrawArrays(ctx,GCM_TYPE_QUADS,0,4);
        cellGcmSetTimeStamp(ctx,index+2*i+1);
    }
}
static void copy_reports(unsigned dest,unsigned index,unsigned bytes) {
    uint32_t *p=ctx->current;
    ord_packet(p,arena_offset+dest,index,bytes);ctx->current=p+12;idle=0;
}
static int seed(unsigned index,unsigned pairs) {
    if(!idle)recovery_required("seed-before-idle");
    if(elapsed_us()>=RRP_SESSION_US)return 0;
    save_report_guards();
    unsigned char prior[16];snapshot(prior,report_ptr(index),16);
    reserve_batch();write_reports(index,pairs);quiet();
    snapshot(old_bytes,report_ptr(index),pairs*32);
    return ord_times(old_bytes,pairs,0)&&memcmp(old_bytes,prior,8)!=0&&report_guards_ok();
}
static int quiescent(unsigned pairs) {
    unsigned bytes=pairs*32;
    if(!seed(516,pairs))return 0;
    reset_destination(0xa5a54000u+pairs);reserve_batch();
    if(elapsed_us()>=RRP_SESSION_US)return 0;
    copy_reports(DEST0,516,bytes);quiet();snapshot(settled,arena+DEST0,bytes);
    int ok=ord_equal(settled,old_bytes,bytes)&&report_guards_ok()&&
        ord_guards((const volatile uint32_t *)arena,ARENA_BYTES,DEST0,bytes,DEST1,0,0xa5a54000u+pairs);
    log_line("ORD_QUIESCENT claim=%s pairs=%u exact_guards=%d idle=1",ORD_NAME,pairs,ok);
    return ok;
}
/* -1 invalid reference/deadline; 0 exact; 1 candidate mismatch/control catch. */
static int trial(unsigned negative,unsigned pairs,unsigned id) {
    const unsigned claim=ORDERING_CLAIM;
    unsigned index=516+(id%2)*252,bytes=pairs*32;
    uint32_t sentinel=0xa5a55000u+id;
    if(!seed(index,pairs))return -1;
    uint64_t old_end;memcpy(&old_end,old_bytes+bytes-16,8);
    reset_destination(sentinel);
    if(elapsed_us()>=RRP_SESSION_US)return -1;
    reserve_batch();
    uint32_t publication=next_serial(),gate=next_serial(),final,ref;
    for(const unsigned char *p=ord_plan(claim,negative);*p!=ORD_END;p++) {
        switch(*p) {
        case ORD_WRITE:write_reports(index,pairs);break;
        case ORD_REPORT_FENCE:backend_gate(LABEL_REPORT,next_serial());break;
        case ORD_COPY_FENCE:backend_gate(LABEL_COPY,next_serial());break;
        case ORD_COPY_OLD:copy_reports(DEST0,index,bytes);break;
        case ORD_COPY_NEW:copy_reports(DEST1,index,bytes);break;
        case ORD_PUBLISH:backend_gate(LABEL_PUBLISH,publication);break;
        case ORD_EARLY:cellGcmSetWriteCommandLabel(ctx,LABEL_PUBLISH,publication);idle=0;break;
        case ORD_GATE:
            gate_highest = gate;
            cellGcmSetWaitLabel(ctx, LABEL_GATE, gate);idle=0;break;
        }
    }
    emit_final(&final,&ref);cellGcmFlush(ctx);
    if(!await_label(LABEL_PUBLISH,publication,RRP_SESSION_US))recovery_required("publication");
    snapshot(immediate,arena+DEST0,bytes);
    if(claim==ORD_B2 && negative)(void)release_gates();
    await_final(final,ref);
    snapshot(settled,arena+DEST0,bytes);snapshot(new_bytes,report_ptr(index),bytes);
    if(!report_guards_ok()||!ord_guards((const volatile uint32_t *)arena,ARENA_BYTES,DEST0,bytes,DEST1,claim==ORD_A2,sentinel))return -1;
    if(claim==ORD_B2) {
        if(!ord_equal(new_bytes,old_bytes,bytes)||!ord_equal(settled,old_bytes,bytes))return -1;
    } else if(!ord_times(new_bytes,pairs,old_end))return -1;
    if(claim==ORD_A2) {
        snapshot(copied_new,arena+DEST1,bytes);
        if(!ord_equal(copied_new,new_bytes,bytes))return -1;
    }
    const unsigned char *expected=claim==ORD_A1?new_bytes:old_bytes;
    int different=ord_evidence(claim,expected,settled,immediate,bytes);
    log_line("ORD_TRIAL claim=%s id=%u control=%u pairs=%u index=%u mismatch=%d guards=1 idle=1 old_end=%"PRIu64,
             ORD_NAME,id,negative,pairs,index,different,old_end);
    return different;
}

int main(int argc,const char **argv) {
    (void)argc;(void)argv;
    session_start=now_us();
    log_line("ORD_BEGIN session=%"PRIu64" report=%u baseline_bytes=16 ordering_claim=" ORD_NAME " deadline_us=%"PRIu64,
             session_start,B0_REPORT_INDEX,RRP_SESSION_US);
    void *host=memalign(1024*1024,HOST_HEAP_SIZE);
    if (!host || !init_rsx(host)) {
        log_line("ORD_SETUP_FAILED init gates_released=%u",release_gates());return 3;
    }
    ctx=CELL_GCM_CURRENT;control=cellGcmGetControlRegister();
    if (!ctx || !control) recovery_required("missing-context");
    for (unsigned label=LABEL_REPORT; label <= LABEL_FINAL; ++label)
        *(volatile uint32_t *)cellGcmGetLabelAddress((uint8_t)label) = 0;
    sync_memory();
    for (unsigned label=LABEL_REPORT; label <= LABEL_FINAL; ++label)
        if (*(volatile uint32_t *)cellGcmGetLabelAddress((uint8_t)label)) recovery_required("label-reset");
    gate_label = (volatile uint32_t *)cellGcmGetLabelAddress(LABEL_GATE);
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
    unsigned char previous[16],reference[32],result[16];
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
    log_line("ORD_BASELINE pixel=0x%08x begin=%"PRIu64" end=%"PRIu64" labels_reset=5",pixel,begin,end);
    log_line("ORD_SOURCE report=%u cpu_ea=0x%"PRIxPTR" input_object=0x66626660 output_object=0xfeed0001",
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
    log_line("ORD_BASELINE ordinary_local_copy=OK idle=1");
    /* Sensitive omitted-copy control, before any experimental source binding. */
    reset_destination(0xa5a51000u);reserve_batch();quiet();
    snapshot(result,arena+DEST0,16);
    int caught=!b0_equal(result,reference);
    log_line("ORD_CONTROL omitted_copy caught=%d idle=1",caught);
    if (!caught || !b0_guards((const volatile uint32_t *)arena,ARENA_BYTES,DEST0,0xa5a51000u))
        return end_run(2,"control-inconclusive");
    report_base=(const volatile uint8_t *)cellGcmGetReportDataAddressLocation(512,CELL_GCM_LOCATION_LOCAL);
    if(!report_base)return end_run(3,"report-map-null");
    for(unsigned i=512;i<=1023;i++)
        if((uintptr_t)cellGcmGetReportDataAddressLocation(i,CELL_GCM_LOCATION_LOCAL)!=(uintptr_t)report_base+(i-512)*16)
            return end_run(3,"report-map-noncontiguous");
    const unsigned lengths[]={1,7,126};
    const char *const verdicts[]={"INCONCLUSIVE","SUPPORTED_THIS_WORKLOAD","REFUTED"};
    unsigned id=0;int outcome=0;
    for(unsigned l=0;l<3;l++) {
        if(!quiescent(lengths[l]))return end_run(3,"quiescent-copy-invalid");
        unsigned complete=0,bad=0,caught=0;
        for(unsigned n=0;n<8;n++)for(unsigned negative=0;negative<2;negative++) {
            log_line("ORD_TRIAL_BEGIN claim=%s id=%u control=%u pairs=%u",ORD_NAME,++id,negative,lengths[l]);
            int result=trial(negative,lengths[l],id);
            if(result<0)return end_run(3,"reference-guards-or-deadline");
            if(negative)caught+=(unsigned)result;else {++complete;bad+=(unsigned)result;}
        }
        unsigned verdict=ord_result(complete,bad,caught);
        log_line("ORD_CLAIM %s pairs=%u verdict=%s valid=%u bad=%u controls_caught=%u/8",
                 ORD_NAME,lengths[l],verdicts[verdict],complete,bad,caught);
        if(verdict==ORD_REFUTED)return end_run(1,"counterexample");
        if(verdict==ORD_INCONCLUSIVE)outcome=2;
    }
    return end_run(outcome,outcome?"inconclusive":"supported-this-workload");
}
