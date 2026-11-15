/* Report-ring feasibility probe. No production profiler instrumentation.
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
#include "probe_logic.h"
#include "state_tour_setup.h"

SYS_PROCESS_PARAM(1001, 0x100000);
enum { LABEL_REPORT = 110, LABEL_COPY = 111, LABEL_PUBLISH = 112,
       LABEL_GATE = 113, LABEL_FINAL = 114, ARENA_BYTES = 1 << 20,
       DEST0 = 128, DEST1 = 8192+128, USED_BYTES = 16384, BATCH_WORDS = 6000 };
_Static_assert(LABEL_REPORT >= CELL_GCM_LABEL_APP_FIRST && LABEL_FINAL <= CELL_GCM_LABEL_APP_LAST, "app labels");
_Static_assert(RRP_REPORT_FIRST >= CELL_GCM_REPORT_APP_FIRST &&
               RRP_REPORT_FIRST+4+4*RRP_MAX_PAIRS+3 <= CELL_GCM_REPORT_APP_LAST, "app reports");
_Static_assert(sizeof(CellGcmReportData) == 16, "report layout");
_Static_assert(DEST0+RRP_MAX_PAIRS*32 < 8192 && DEST1+RRP_MAX_PAIRS*32 < USED_BYTES, "guarded extents");
static CellGcmContextData *ctx;
static volatile CellGcmControl *control;
static uint64_t session_start;
static uint32_t serial, reference_serial;
static volatile uint32_t *gate_label;
static uint32_t gate_highest;
static int idle = 1;
static volatile uint32_t *reports;
static volatile uint8_t *arena;
static uint32_t arena_offset, report_offsets[2], poison_offset;
static unsigned char old_bytes[RRP_MAX_PAIRS*32], new_bytes[RRP_MAX_PAIRS*32];
static unsigned char immediate[RRP_MAX_PAIRS*32], settled[RRP_MAX_PAIRS*32];
static const char *const claim_names[] = {"A1", "A2", "B1", "B2"};
static const char *const verdict_names[] = {"INVALID", "REFUTED", "INCONCLUSIVE", "SUPPORTED_THIS_WORKLOAD"};

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
    FILE *f = fopen("/dev_hdd0/tmp/report-ring-probe.crumb", "a");
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
    log_line("RRP_RECOVERY_REQUIRED session=%"PRIu64" elapsed=%"PRIu64" at=%s gates_released=%u no_retry=1 no_normal_exit=1",
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
static void write_reports(unsigned index, unsigned pairs) {
    idle = 0;
    for (unsigned i = 0; i < pairs; ++i) {
        cellGcmSetTimeStamp(ctx, index+2*i);
        cellGcmSetDrawArrays(ctx, GCM_TYPE_QUADS, 0, 4);
        cellGcmSetTimeStamp(ctx, index+2*i+1);
    }
}
static int valid_times(const unsigned char *bytes, unsigned pairs, uint32_t sentinel) {
    uint64_t previous = 0, poison = ((uint64_t)sentinel << 32) | sentinel;
    for (unsigned i = 0; i < pairs; ++i) {
        uint64_t begin, end;
        memcpy(&begin, bytes+i*32, 8); memcpy(&end, bytes+i*32+16, 8);
        if (!begin || !end || begin == poison || end == poison || end < begin || begin < previous) return 0;
        previous = end;
    }
    return 1;
}
static void copy_reports(unsigned destination, uint32_t source, unsigned pairs) {
    uint32_t bytes = pairs*32;
    cellGcmSetTransferData(ctx, CELL_GCM_TRANSFER_LOCAL_TO_MAIN, arena_offset+destination,
                           bytes, source, bytes, bytes, 1);
    idle = 0;
}
static int guards_ok(unsigned pairs, int second, uint32_t sentinel) {
    const volatile uint32_t *words = (const volatile uint32_t *)arena;
    for (unsigned b = 0; b < USED_BYTES; b += 4) {
        if ((b >= DEST0 && b < DEST0+pairs*32) || (second && b >= DEST1 && b < DEST1+pairs*32)) continue;
        if (words[b/4] != sentinel) return 0;
    }
    for (unsigned i = 0; i < 16; ++i)
        if (reports[i] != sentinel || reports[(4+4*RRP_MAX_PAIRS)*4+i] != sentinel) return 0;
    return 1;
}
static void reset_slots(uint32_t sentinel) {
    /* Only called after the preceding final fence. Every report and every
     * readback word used by this trial (including guards) gets a sentinel. */
    for (unsigned i = 0; i < 512*4; ++i) reports[i] = sentinel;
    for (unsigned i = 0; i < USED_BYTES/4; ++i) ((volatile uint32_t *)arena)[i] = sentinel;
    sync_memory();
}
static int trial(unsigned claim, unsigned negative, unsigned pairs, unsigned trial_id) {
    if (elapsed_us() >= RRP_SESSION_US) return -1;
    unsigned bank = trial_id % 2;
    unsigned index = RRP_REPORT_FIRST+4+bank*RRP_MAX_PAIRS*2;
    volatile uint8_t *source = (volatile uint8_t *)cellGcmGetReportDataAddressLocation(index, CELL_GCM_LOCATION_LOCAL);
    uint32_t sentinel = 0xa5a50000u ^ trial_id;
    unsigned bytes = pairs*32;
    reset_slots(sentinel);
    reserve_batch();
    /* Quiescent old-generation evidence for B1/B2 and A2 is acquired before
     * any tested DMA. A1 starts from sentinel reports instead. */
    if (claim != RRP_A1) {
        write_reports(index, pairs); quiet();
        snapshot(old_bytes, source, bytes);
        if (!valid_times(old_bytes, pairs, sentinel)) return -2;
    }
    if (elapsed_us() >= RRP_SESSION_US) return -1;
    reserve_batch();
    uint32_t publication = next_serial(), gate = next_serial(), final, ref;
    const unsigned char *plan = rrp_plan(claim, negative);
    for (; *plan != RRP_END; ++plan) {
        switch (*plan) {
        case RRP_WRITE: write_reports(index, pairs); break;
        case RRP_REPORT_FENCE: backend_gate(LABEL_REPORT, next_serial()); break;
        case RRP_COPY_FENCE: backend_gate(LABEL_COPY, next_serial()); break;
        case RRP_COPY_OLD: copy_reports(DEST0, report_offsets[bank], pairs); break;
        case RRP_COPY_NEW: copy_reports(DEST1, report_offsets[bank], pairs); break;
        case RRP_COPY_BAD: copy_reports(DEST0, poison_offset, pairs); break;
        case RRP_PUBLISH: backend_gate(LABEL_PUBLISH, publication); break;
        case RRP_EARLY: cellGcmSetWriteCommandLabel(ctx, LABEL_PUBLISH, publication); idle = 0; break;
        case RRP_GATE:
            /* Arm cleanup BEFORE queuing the wait, including uncertain emit. */
            gate_highest = gate;
            cellGcmSetWaitLabel(ctx, LABEL_GATE, gate);
            break;
        }
    }
    emit_final(&final, &ref);
    cellGcmFlush(ctx);
    if (!await_label(LABEL_PUBLISH, publication, RRP_SESSION_US)) recovery_required("publication");
    /* This copy is frozen BEFORE any final fence wait. A later good read must
     * never repair evidence of early publication or missing CPU visibility. */
    snapshot(immediate, arena+DEST0, bytes);
    if (claim == RRP_B2 && negative) {
        (void)release_gates();
    }
    await_final(final, ref);
    snapshot(settled, arena+DEST0, bytes);
    snapshot(new_bytes, source, bytes);
    if (!valid_times(new_bytes, pairs, sentinel) || !guards_ok(pairs, claim == RRP_A2, sentinel)) return -2;
    const unsigned char *expected = claim == RRP_A1 ? new_bytes : old_bytes;
    int different = memcmp(claim == RRP_B2 ? immediate : settled, expected, bytes) != 0;
    if (claim == RRP_A2) {
        unsigned char copied_new[RRP_MAX_PAIRS*32];
        snapshot(copied_new, arena+DEST1, bytes);
        uint64_t old_end, new_begin;
        memcpy(&old_end, old_bytes+bytes-16, 8); memcpy(&new_begin, new_bytes, 8);
        if (new_begin <= old_end || memcmp(copied_new, new_bytes, bytes)) return -2;
    }
    if (claim == RRP_B2 && memcmp(settled, old_bytes, bytes)) return -2;
    return different;
}
static int reports_addressable(const CellGcmConfig *cfg) {
    for (unsigned bank = 0; bank < 2; ++bank) {
        unsigned first = RRP_REPORT_FIRST+4+bank*RRP_MAX_PAIRS*2;
        uintptr_t p = (uintptr_t)cellGcmGetReportDataAddressLocation(first, CELL_GCM_LOCATION_LOCAL);
        uintptr_t last = (uintptr_t)cellGcmGetReportDataAddressLocation(first+RRP_MAX_PAIRS*2-1, CELL_GCM_LOCATION_LOCAL);
        uint32_t off = UINT32_MAX;
        int rc = cellGcmAddressToOffset((void *)p, &off);
        log_line("RRP_ADDRESS bank=%u ptr=0x%"PRIxPTR" last=0x%"PRIxPTR" local=0x%"PRIxPTR" local_size=%u rc=%d offset=0x%x",
                 bank, p, last, (uintptr_t)cfg->localAddress, cfg->localSize, rc, off);
        if (rc || last-p != RRP_MAX_PAIRS*32-16 ||
            !rrp_local_span((uintptr_t)cfg->localAddress, cfg->localSize, p, RRP_MAX_PAIRS*32, off)) return 0;
        for (unsigned i = 0; i < RRP_MAX_PAIRS*2; ++i)
            if ((uintptr_t)cellGcmGetReportDataAddressLocation(first+i, CELL_GCM_LOCATION_LOCAL) != p+i*16) return 0;
        report_offsets[bank] = off;
    }
    return 1;
}
static int end_run(int rc, const char *reason) {
    unsigned released = release_gates();
    if (!idle) quiet();
    log_line("RRP_END session=%"PRIu64" rc=%d reason=%s gates_released=%u idle=1 elapsed=%"PRIu64,
             session_start, rc, reason, released, elapsed_us());
    /* No frees: GCM mappings remain owned until process teardown. No flips or
     * zcull bindings are issued by this probe, so none need retirement here. */
    return rc;
}
int main(int argc, const char **argv) {
    (void)argc; (void)argv;
    session_start = now_us();
    log_line("RRP_BEGIN session=%"PRIu64" trials=%u lengths=1,7,126 deadline_us=%"PRIu64,
             session_start, RRP_TRIALS, RRP_SESSION_US);
    void *host = memalign(1024*1024, HOST_HEAP_SIZE);
    if (!host || !init_rsx(host)) {
        unsigned released = release_gates();
        log_line("RRP_SETUP_FAILED init gates_released=%u", released); return 3;
    }
    ctx = CELL_GCM_CURRENT; control = cellGcmGetControlRegister();
    if (!ctx || !control) recovery_required("missing-context");
    for (unsigned label = LABEL_REPORT; label <= LABEL_FINAL; ++label)
        *(volatile uint32_t *)cellGcmGetLabelAddress((uint8_t)label) = 0;
    sync_memory();
    for (unsigned label = LABEL_REPORT; label <= LABEL_FINAL; ++label)
        if (*(volatile uint32_t *)cellGcmGetLabelAddress((uint8_t)label)) recovery_required("label-reset");
    gate_label = (volatile uint32_t *)cellGcmGetLabelAddress(LABEL_GATE);
    uint32_t initial_ref = control->ref;
    reference_serial = initial_ref == UINT32_MAX ? 0 : initial_ref;
    init_shaders(); init_geometry();
    reserve_batch(); env_reset(ctx); idle = 0;
    CellGcmConfig cfg; cellGcmGetConfiguration(&cfg);
    reports = (volatile uint32_t *)cellGcmGetReportDataAddressLocation(RRP_REPORT_FIRST, CELL_GCM_LOCATION_LOCAL);
    if (!reports) recovery_required("missing-report-map");
    for (unsigned i = 0; i < 512; ++i)
        if ((uintptr_t)cellGcmGetReportDataAddressLocation(RRP_REPORT_FIRST+i, CELL_GCM_LOCATION_LOCAL) !=
            (uintptr_t)reports+i*16) return end_run(3, "noncontiguous-cpu-report-map");
    sys_addr_t allocation = 0;
    if (gcm_host_buffer_alloc(ARENA_BYTES, &allocation) ||
        cellGcmMapMainMemory((void *)(uintptr_t)allocation, ARENA_BYTES, &arena_offset))
        return end_run(3, "readback-allocation");
    arena = (volatile uint8_t *)(uintptr_t)allocation;
    for (unsigned i = 0; i < ARENA_BYTES/4; ++i) ((volatile uint32_t *)arena)[i] = 0xa5a50000u;
    uint32_t *poison = (uint32_t *)local_align(64, RRP_MAX_PAIRS*32);
    if (cellGcmAddressToOffset(poison, &poison_offset) ||
        !rrp_local_span((uintptr_t)cfg.localAddress, cfg.localSize, (uintptr_t)poison,
                        RRP_MAX_PAIRS*32, poison_offset)) return end_run(3, "poison-address");
    for (unsigned i = 0; i < RRP_MAX_PAIRS*8; ++i) poison[i] = 0xdead8000u+i;
    quiet();
    reset_slots(0xa5a50000u);
    cellGcmSetReportLocation(ctx, CELL_GCM_LOCATION_LOCAL);
    cellGcmSetClearColor(ctx, 0xff000000u);
    cellGcmSetClearSurface(ctx, GCM_CLEAR_R | GCM_CLEAR_G | GCM_CLEAR_B | GCM_CLEAR_A);
    write_reports(RRP_REPORT_FIRST+4, 1); quiet();
    snapshot(old_bytes, (volatile uint8_t *)reports+4*16, 32);
    uint64_t begin, end;
    memcpy(&begin, old_bytes, 8); memcpy(&end, old_bytes+16, 8);
    uint32_t pixel = *(volatile uint32_t *)(g_tile+32*TILE_W+32);
    log_line("RRP_BASELINE pixel=0x%08x begin=%"PRIu64" end=%"PRIu64, pixel, begin, end);
    if (pixel != 0xffffffffu || !valid_times(old_bytes, 1, 0xa5a50000u) || end == begin)
        return end_run(3, "baseline-invalid");
    /* Exercise a known ordinary local-memory transfer even on platforms whose
     * report mapping cannot safely be submitted as a LOCAL source. */
    reserve_batch(); copy_reports(DEST0, g_tile_offset, 1); quiet();
    snapshot(settled, arena+DEST0, 32);
    for (unsigned i = 0; i < 32; ++i) if (settled[i] != 0xff) return end_run(3, "ordinary-copy-invalid");
    log_line("RRP_BASELINE ordinary_local_copy=OK labels_reset=5 ref=%u", reference_serial);
    if (!reports_addressable(&cfg)) {
        log_line("RRP_B1 ADDRESS_UNAVAILABLE no_report_dma=1");
        for (unsigned c = 0; c < 4; ++c) log_line("RRP_CLAIM %s INCONCLUSIVE reason=addressability", claim_names[c]);
        return end_run(2, "address-unavailable");
    }
    const unsigned lengths[] = {1, 7, RRP_MAX_PAIRS};
    const unsigned claim_order[] = {RRP_B1, RRP_A1, RRP_A2, RRP_B2};
    unsigned trial_id = 0, overall = RRP_SUPPORTED;
    for (unsigned order = 0; order < 4; ++order) for (unsigned l = 0; l < 3; ++l) {
        unsigned c = claim_order[order];
        unsigned complete = 0, bad = 0, caught = 0;
        for (unsigned negative = 0; negative < 2; ++negative) for (unsigned t = 0; t < RRP_TRIALS; ++t) {
            log_line("RRP_TRIAL_BEGIN id=%u claim=%s control=%u pairs=%u", ++trial_id, claim_names[c], negative, lengths[l]);
            int result = trial(c, negative, lengths[l], trial_id);
            log_line("RRP_TRIAL id=%u result=%d elapsed=%"PRIu64, trial_id, result, elapsed_us());
            if (result < 0) return end_run(3, result == -1 ? "deadline" : "reference-invalid");
            if (negative) caught += (unsigned)result;
            else { ++complete; bad += (unsigned)result; }
        }
        unsigned verdict = rrp_verdict(complete, bad, 0, caught);
        if (verdict < overall) overall = verdict;
        log_line("RRP_CLAIM %s pairs=%u %s candidates=%u bad=%u control_caught=%u/%u",
                 claim_names[c], lengths[l], verdict_names[verdict], complete, bad, caught, RRP_TRIALS);
        if (c == RRP_B1 && verdict != RRP_SUPPORTED) {
            log_line("RRP_DEPENDENTS A1,A2,B2 INCONCLUSIVE reason=B1-gate");
            return end_run(verdict == RRP_REFUTED ? 1 : 2, "B1-not-qualified");
        }
    }
    if (elapsed_us() >= RRP_SESSION_US) return end_run(3, "deadline-final");
    return end_run(overall == RRP_SUPPORTED ? 0 : overall == RRP_REFUTED ? 1 : 2, "complete");
}
