/* E1: the first on-console SPU-lane + RSX-domain probe (design p3.3 E1 r1-r4 + review 05:30).  UNVALIDATED ON TARGET.
 *
 * One SPU lane (one thread, the program in spu/), then one GPU domain (NOP blocks, no display), APP drain mode on this
 * thread, phases strictly in sequence.  Every group of calls is bracketed by an EXTP1 marker (e1_marker.h) so a hang
 * is localized to "after the last valid marker"; no marker claims which unbracketed call blocked.
 *
 * Rules carried from the design:
 *  - The SPU reports through two PPU lines written only by its DMA (e1_report.h); a report is trusted only after:
 *    commit == s, sync, private copy of the payload, sync, commit re-read == s, and e1_report_check() on the copy.  A
 *    trusted report that carries an error is a FAILED experiment.  Report fields are printed only for a trusted
 *    report.
 *  - The GO write is one sysSpuThreadWriteMb; a nonzero return is a SUBMISSION WITH UNKNOWN OUTCOME (unres q): no
 *    retry, the report lines are not reused, cleanup is terminate + join.
 *  - Only a SUCCESSFUL join authorizes lane_end and the release of the group, the image and the report lines; after a
 *    failed join everything SPU-side stays borrowed to process end and the GPU phase is skipped.
 *  - A drain, stats or lane_end failure is an extension fault: no further extension call except shutdown.
 *  - A failed marker (logging failure) stops NEW work before the next submission (group start, GO, GPU attach, each
 *    frame, detach); cleanup still runs, unmarked (99 was attempted once).
 *  - GPU: attach failure -> no frames; a frames or detach failure -> no release; cellGcmFinish only after a
 *    successful detach; release only after it.
 * Deadlines bound POLLING only; join, cellGcmFinish, the drains, shutdown and the marker file calls are not bounded. */
#ifndef E1_HOST_TEST
#include <cell/gcm.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/process.h>
#include <sys/spu.h>
#include <sys/stat.h>
#include <sys/systime.h>
#include <unistd.h>

#include <ps3dk/prof.h>

#include "e1_spu_bin.h" /* the SPU program's raw ELF bytes (ps3_add_spu_image) */
#include "gcm_host_buffer.h"
#include "prof_gpu.h"
#include "prof_spu.h"
#include "psp_layout.h"

#include "e1_report.h"

#define E1_TB() __builtin_ppc_get_timebase()
#define E1_SYNC() __asm__ volatile("sync" ::: "memory")
#define E1_BASE "/dev_hdd0/tmp/ps3dk-extp"
#define E1_OPEN(p, seq) open((p), O_WRONLY | O_CREAT | O_EXCL, 0644)
#define E1_WRITE(fd, b, n, seq) write((fd), (b), (n))
#define E1_FSYNC(fd, seq) fsync(fd)
#define E1_CLOSE(fd, seq) close(fd)
#define E1_MKDIR(p) mkdir((p), 0755)
#define E1_POLL_PAUSE() ((void)0)
#define E1_SLEEP_US(us) usleep(us)
#define E1_AFTER_COPY(seq) ((void)0)
#define E1_IMAGE_REGISTER(out) ps3dk_prof_spu_image_register_elf(e1_spu_bin, (uint64_t)e1_spu_bin_size, (out))
#define E1_GCM_CTX() gCellGcmCurrentContext
#define E1_NOP_BLOCK(ctx) cellGcmSetNopCommand((ctx), 64u)
#define E1_CAP_OPEN(p) open((p), O_RDONLY)
#define E1_CAP_READ(fd, b, n) read((fd), (b), (n))
#define E1_CAP_CLOSE(fd) close(fd)
SYS_PROCESS_PARAM(1001, 0x100000);

/* the report lines: static, 128-aligned, written only by the SPU's DMA; never reused before a successful join */
static e1_line e1_payload_line, e1_commit_line;
static uint64_t e1_tb_hz;
static uint64_t e1_now_us(void) { return E1_TB() / (e1_tb_hz / 1000000u); }
static void e1_clock_init(void) { e1_tb_hz = sysGetTimebaseFrequency(); }
static uint32_t e1_commit_ld(void) { return *(volatile const uint32_t *)&e1_commit_line.w[0]; }
static uint32_t e1_payload_ld(unsigned i) { return ((volatile const uint32_t *)e1_payload_line.w)[i]; }
static void e1_lines_zero(void)
{
    memset(&e1_payload_line, 0, sizeof e1_payload_line);
    memset(&e1_commit_line, 0, sizeof e1_commit_line);
    E1_SYNC();
}
/* the report lines' EAs, checked (P-EA32, e1_report.h): -1 when either is above 4 GiB (never narrowed) */
static int e1_report_eas(uint64_t *out)
{
    return e1_pack_report_eas((uint64_t)(uintptr_t)&e1_payload_line, (uint64_t)(uintptr_t)&e1_commit_line, out);
}
/* the PPU's own read of line A's read_idx (the drainer's store): one aligned 64-bit load after a sync, at the full
 * process address the lane reported (no 32-bit round trip) */
static uint64_t e1_line_a_read_idx(uint64_t ea_ctl)
{
    E1_SYNC();
    return *(volatile const uint64_t *)(uintptr_t)(ea_ctl + PSP_A_READ_IDX);
}
#else
#include "e1_host.h" /* host tests: fakes for every LV2/GCM call, the host SPU producer, the file seams */
#endif

#include "e1_marker.h"

/* The data model, printed in the start marker, is the compiled one; a build that declares it (E1_ABI_LP64, from
 * CMake's E1_ABI) must declare the compiled one. */
#ifdef __LP64__
#define E1_ABI_NAME "lp64"
#else
#define E1_ABI_NAME "ilp32"
#endif
#if defined(E1_ABI_LP64)
_Static_assert(E1_ABI_LP64 == (sizeof(void *) == 8), "E1_ABI is not the compiled data model");
#elif !defined(E1_HOST_TEST)
#error "E1_ABI_LP64 must be defined by the build"
#endif

#ifndef E1_WAIT_US
#define E1_WAIT_US 2000000u /* each polling deadline */
#endif
#define E1_FRAMES 30u
#define E1_GPU_REPORT_FIRST 0u /* reports 0..15 and label 64: unused by cellGcmInit / rsxFinish (no libPSGL here) */
#define E1_GPU_REPORT_COUNT 16u
#define E1_GPU_LABEL 64u

static int e1_failed;     /* any failure: the exit status */
static int e1_ext_fault;  /* a drain / stats / lane_end failure: only shutdown remains */

static const char *e1_call_name(uint32_t c)
{
    static const char *n[] = {"none", "attach", "flush", "dec", "fini", "event", "go"};
    return c <= E1_CALL_GO ? n[c] : "?";
}

/* ---- the SPU reports ---- */
enum { REP_OK, REP_TIMEOUT, REP_IDENTITY };

/* waits for report s (1 or 2) and copies it into w[32]; REP_OK only for a trusted copy */
static int e1_wait_report(uint32_t s, uint32_t *w, int *rule)
{
    uint64_t t0 = e1_now_us();
    *rule = 0;
    while (e1_commit_ld() != s) {
        if (e1_now_us() - t0 > E1_WAIT_US)
            return REP_TIMEOUT;
        E1_POLL_PAUSE();
    }
    E1_SYNC();
    for (unsigned i = 0; i < 32u; i++)
        w[i] = e1_payload_ld(i);
    E1_SYNC();
    E1_AFTER_COPY(s);
    if (e1_commit_ld() != s) { /* the commit changed under the copy: untrusted */
        *rule = -1;
        return REP_IDENTITY;
    }
    *rule = e1_report_check(w, s);
    return *rule ? REP_IDENTITY : REP_OK;
}

/* the result text of a report; *spu_error = 1 for a trusted report carrying an error */
static void e1_report_result(int k, const uint32_t *w, char *res, size_t n, int *spu_error)
{
    *spu_error = 0;
    if (k == REP_TIMEOUT)
        snprintf(res, n, "timeout");
    else if (k == REP_IDENTITY)
        snprintf(res, n, "identity");
    else if (w[E1_W_FAIL_CALL] != E1_CALL_NONE) {
        snprintf(res, n, "spu-status:%s:%d", e1_call_name(w[E1_W_FAIL_CALL]), (int)(int32_t)w[E1_W_FAIL_RC]);
        *spu_error = 1;
    } else
        snprintf(res, n, "ok");
}

static void e1_fail_field(const uint32_t *w, char *out, size_t n)
{
    if (w[E1_W_FAIL_CALL] == E1_CALL_NONE)
        snprintf(out, n, "fail=none");
    else
        snprintf(out, n, "fail=%s:%d", e1_call_name(w[E1_W_FAIL_CALL]), (int)(int32_t)w[E1_W_FAIL_RC]);
}

/* ---- the SPU side's acquisition state (each cleanup step runs once, guarded by these flags) ---- */
typedef struct e1_spu {
    ps3dk_prof_spu_lane lane;
    sysSpuImage image;
    uint32_t group, thread;
    int lane_created, imported, group_created, started; /* started: 0 no, 1 ok, 2 unknown (start failed) */
    int exited;      /* a trusted report says the program returned (an error report, or report 2) */
    int joined;      /* 1 only after a SUCCESSFUL join */
    int clean;       /* both reports trusted and clean, join and lane_end OK: the GPU phase may run */
} e1_spu;

static void e1_hex_err(char *res, size_t n, const char *what, int rc)
{
    snprintf(res, n, "error:%s:%08x", what, (unsigned)(uint32_t)rc);
}

/* SPU cleanup (r3 C): terminate (only if the program may still run), join, lane_end, destroy + image close */
static void e1_spu_cleanup(e1_spu *sp)
{
    char res[48], f[96];
    int rc;
    if (!sp->lane_created)
        return; /* nothing SPU-side exists (register_elf / lane_create failed) */
    int terminate_failed = 0;
    if (sp->started) {
        if (!sp->exited) {
            rc = sysSpuThreadGroupTerminate(sp->group, 0);
            if (rc) {
                e1_hex_err(res, sizeof res, "terminate", rc);
                terminate_failed = 1;
                e1_failed = 1;
            } else
                snprintf(res, sizeof res, "ok");
        } else
            snprintf(res, sizeof res, "not-needed");
        e1_mark(13, "terminate-D", res, "");
        if (terminate_failed) {
            /* the program may still be blocked on its inbound mailbox: a join could never return */
            e1_mark(14, "join-B", "not-attempted:terminate-failed", "");
        } else {
            uint32_t cause = 0, status = 0;
            e1_mark(14, "join-B", "ok", "");
            rc = sysSpuThreadGroupJoin(sp->group, &cause, &status);
            if (rc) {
                e1_hex_err(res, sizeof res, "join", rc);
                e1_failed = 1;
                e1_mark(15, "join-D", res, "");
            } else {
                sp->joined = 1;
                snprintf(f, sizeof f, "cause=%08x status=%08x", (unsigned)cause, (unsigned)status);
                e1_mark(15, "join-D", "ok", f);
            }
        }
    } else {
        e1_mark(14, "join-B", "not-attempted:not-started", "");
    }

    /* lane_end only after a successful join of a started group (a never-started lane is finalized by shutdown) */
    if (!sp->joined)
        e1_mark(16, "lane-end-D", "not-attempted:not-joined", "");
    else if (e1_ext_fault)
        e1_mark(16, "lane-end-D", "not-attempted:ext-fault", "");
    else {
        ps3dk_prof_spu_lane_stats st;
        memset(&st, 0, sizeof st);
        rc = ps3dk_prof_spu_lane_end(sp->lane.handle, E1_WAIT_US, &st);
        if (rc) {
            e1_hex_err(res, sizeof res, "lane-end", rc);
            e1_failed = 1;
            e1_ext_fault = 1;
            e1_mark(16, "lane-end-D", res, "");
        } else {
            char fin[16];
            if (st.final_written)
                snprintf(fin, sizeof fin, "%u", (unsigned)st.fin_state);
            else
                snprintf(fin, sizeof fin, "none");
            snprintf(f, sizeof f, "events=%llu lost=%llu final=%s", (unsigned long long)st.events,
                     (unsigned long long)st.lost, fin);
            e1_mark(16, "lane-end-D", "ok", f);
            if (sp->clean)
                sp->clean = 2; /* lane_end OK too */
        }
    }

    /* the group and the image: released only when no SPU thread can run (joined, or never started) */
    if (sp->started && !sp->joined) {
        e1_mark(17, "spu-release-D", "not-attempted:not-joined", "");
    } else if (!sp->group_created && !sp->imported) {
        e1_mark(17, "spu-release-D", "not-attempted:not-created", "");
    } else {
        rc = 0;
        if (sp->group_created && (rc = sysSpuThreadGroupDestroy(sp->group)) != 0)
            e1_hex_err(res, sizeof res, "destroy", rc);
        else if (sp->imported && (rc = sysSpuImageClose(&sp->image)) != 0)
            e1_hex_err(res, sizeof res, "image-close", rc);
        else
            snprintf(res, sizeof res, "ok");
        if (rc)
            e1_failed = 1;
        e1_mark(17, "spu-release-D", res, "");
    }
}

/* the SPU phase (markers 04-12); returns with sp describing what exists */
static void e1_spu_phase(e1_spu *sp)
{
    char res[64], f[160], ft[48];
    uint32_t w[32];
    int rc, rule, spu_error;
    uint32_t image_id = 0;
    const char *call = NULL;

    /* 04: registration, the lane, then the LV2 SPU objects (the report lines are zeroed before the thread exists) */
    if ((rc = E1_IMAGE_REGISTER(&image_id)) != 0)
        call = "register-elf";
    else if ((rc = ps3dk_prof_spu_lane_create("probe", image_id, 1, &sp->lane)) != 0)
        call = "lane-create";
    else {
        sp->lane_created = 1;
        if ((rc = sysSpuInitialize(6, 0)) != 0)
            call = "spu-init";
        else if ((rc = sysSpuImageImport(&sp->image, e1_spu_bin, 0)) != 0)
            call = "import";
        else {
            sysSpuThreadGroupAttribute gattr;
            memset(&gattr, 0, sizeof gattr);
            sp->imported = 1;
            gattr.name = "extprobe";
            gattr.nsize = 9;
            if ((rc = sysSpuThreadGroupCreate(&sp->group, 1, 100, &gattr)) != 0)
                call = "group-create";
            else {
                sysSpuThreadAttribute tattr;
                sysSpuThreadArgument args;
                memset(&tattr, 0, sizeof tattr);
                memset(&args, 0, sizeof args);
                sp->group_created = 1;
                tattr.name = "extprobe-spu";
                tattr.nsize = 13;
                e1_lines_zero();
                args.arg0 = sp->lane.ea_ring;
                args.arg1 = (uint64_t)sp->lane.ring_slots << 32;
                args.arg2 = sp->lane.ea_ctl;
                if (e1_report_eas(&args.arg3) != 0) {
                    rc = -1;
                    call = "ea32"; /* a report line above 4 GiB: P-EA32 failed, the thread is not created */
                } else if ((rc = sysSpuThreadInitialize(&sp->thread, sp->group, 0, &sp->image, &tattr, &args)) != 0)
                    call = "thread-init";
            }
        }
    }
    f[0] = '\0';
    if (sp->lane_created)
        snprintf(f, sizeof f, "lane=%u", (unsigned)sp->lane.handle);
    if (call) {
        e1_hex_err(res, sizeof res, call, rc);
        e1_failed = 1;
        e1_mark(4, "spu-setup-done", res, f);
        return;
    }
    e1_mark(4, "spu-setup-done", "ok", f);

    /* 05/06: the group start (a failed start leaves the group state unknown: terminate + join in cleanup) */
    if (e1_logging_failed())
        return;
    e1_mark(5, "group-start-B", "ok", "");
    if (e1_logging_failed())
        return; /* the B marker itself failed: the start is not submitted */
    rc = sysSpuThreadGroupStart(sp->group);
    if (rc) {
        sp->started = 2;
        e1_unres[0] = 'u';
        e1_failed = 1;
        e1_hex_err(res, sizeof res, "group-start", rc);
        e1_mark(6, "group-start-D", res, "");
        return;
    }
    sp->started = 1;
    e1_mark(6, "group-start-D", "ok", "");

    /* 07: report 1 (epoch 1 flushed, or the first failure before GO) */
    rc = e1_wait_report(1u, w, &rule);
    e1_report_result(rc, w, res, sizeof res, &spu_error);
    if (rc != REP_OK) {
        e1_unres[0] = 'u';
        e1_failed = 1;
        e1_mark(7, "epoch1-report-D", res, ""); /* no payload-derived field */
        return;
    }
    e1_count[0] = w[E1_W_EV1];
    e1_fail_field(w, ft, sizeof ft);
    snprintf(f, sizeof f, "spu_ev1=%u %s", (unsigned)w[E1_W_EV1], ft);
    if (spu_error) {
        sp->exited = 1; /* a trusted error report: the program returned */
        e1_failed = 1;
        e1_mark(7, "epoch1-report-D", res, f);
        return;
    }
    e1_mark(7, "epoch1-report-D", "ok", f);

    /* 08/09: drain until the lane's accepted events cover epoch 1, then the PPU's own read of line A */
    e1_mark(8, "drain1-B", "ok", "");
    {
        ps3dk_prof_spu_lane_stats st;
        uint64_t t0 = e1_now_us();
        int done = 0;
        memset(&st, 0, sizeof st);
        for (;;) {
            if ((rc = ps3dk_prof_drain()) != 0) {
                e1_hex_err(res, sizeof res, "drain", rc);
                break;
            }
            if ((rc = ps3dk_prof_spu_lane_get_stats(sp->lane.handle, &st)) != 0) {
                e1_hex_err(res, sizeof res, "stats", rc);
                break;
            }
            if (st.events >= E1_EVENTS_PER_EPOCH) {
                done = 1;
                break;
            }
            if (e1_now_us() - t0 > E1_WAIT_US) {
                snprintf(res, sizeof res, "timeout");
                break;
            }
            E1_SLEEP_US(1000);
        }
        if (!done) {
            if (rc)
                e1_ext_fault = 1;
            e1_failed = 1;
            e1_mark(9, "drain1-D", res, "");
            return; /* the program waits for GO: terminate + join in cleanup */
        }
        uint64_t ridx = e1_line_a_read_idx(sp->lane.ea_ctl);
        snprintf(f, sizeof f, "read_idx=%016llx accepted=%llu", (unsigned long long)ridx,
                 (unsigned long long)st.events);
        e1_mark(9, "drain1-D", "ok", f);
    }

    /* 10/11: GO (one write; a nonzero return is a submission with unknown outcome) */
    if (e1_logging_failed())
        return;
    e1_mark(10, "go-B", "ok", "");
    if (e1_logging_failed())
        return; /* the B marker itself failed: GO is not submitted (the program waits: terminate + join) */
    rc = sysSpuThreadWriteMb(sp->thread, E1_GO);
    if (rc) {
        e1_unres[0] = 'q';
        e1_failed = 1;
        snprintf(res, sizeof res, "mbox-error:%08x", (unsigned)(uint32_t)rc);
        e1_mark(11, "go-D", res, "");
        return;
    }
    e1_mark(11, "go-D", "ok", "");

    /* 12: report 2 (final) */
    rc = e1_wait_report(2u, w, &rule);
    e1_report_result(rc, w, res, sizeof res, &spu_error);
    if (rc != REP_OK) {
        e1_unres[0] = 'u';
        e1_failed = 1;
        e1_mark(12, "final-report-D", res, "");
        return;
    }
    sp->exited = 1;
    {
        char dec[24], seen[24];
        if (w[E1_W_DEC_STATE] == E1_DEC_OK)
            snprintf(dec, sizeof dec, "ok:%d", (int)(int32_t)w[E1_W_DEC_RC]);
        else if (w[E1_W_DEC_STATE] == E1_DEC_FAILED)
            snprintf(dec, sizeof dec, "failed:%d", (int)(int32_t)w[E1_W_DEC_RC]);
        else
            snprintf(dec, sizeof dec, "not-attempted");
        if (w[E1_W_RIDX_STATE] == E1_RIDX_VALID)
            snprintf(seen, sizeof seen, "%08x%08x", (unsigned)w[E1_W_RIDX_HI], (unsigned)w[E1_W_RIDX_LO]);
        else
            snprintf(seen, sizeof seen, "unavailable");
        e1_count[0] = w[E1_W_EV1] + w[E1_W_EV2];
        e1_fail_field(w, ft, sizeof ft);
        snprintf(f, sizeof f, "spu_ev2=%u dec=%s seen_ridx=%s %s", (unsigned)w[E1_W_EV2], dec, seen, ft);
    }
    if (spu_error)
        e1_failed = 1;
    else
        sp->clean = 1;
    e1_mark(12, "final-report-D", res, f);
}

/* ---- the GPU phase (markers 18-25) ---- */
static void e1_gpu_phase(void)
{
    char res[64], f[96];
    int rc;
    sys_addr_t host = 0;
    uint32_t gpu = 0;
    ps3dk_prof_gpu_stats st;

    if ((rc = gcm_host_buffer_alloc(1u << 20, &host)) != 0) {
        snprintf(res, sizeof res, "unavailable:host-buffer:%08x", (unsigned)(uint32_t)rc);
    } else if ((rc = cellGcmInit(64u << 10, 1u << 20, (void *)(uintptr_t)host)) != 0) {
        snprintf(res, sizeof res, "unavailable:gcm-init:%08x", (unsigned)(uint32_t)rc);
    }
    if (rc) {
        e1_failed = 1;
        e1_mark(18, "gcm-init-D", res, "");
        return;
    }
    e1_mark(18, "gcm-init-D", "ok", "");
    ps3dk_prof_gcm_ctx *ctx = E1_GCM_CTX();

    if (e1_logging_failed())
        return;
    rc = ps3dk_prof_gpu_attach(ctx, E1_GPU_REPORT_FIRST, E1_GPU_REPORT_COUNT, E1_GPU_LABEL, &gpu);
    if (rc) {
        e1_hex_err(res, sizeof res, "gpu-attach", rc);
        e1_failed = 1;
        e1_mark(19, "gpu-attach-D", res, "");
        return; /* no frames, nothing to detach */
    }
    snprintf(f, sizeof f, "gpu=%u", (unsigned)gpu);
    e1_mark(19, "gpu-attach-D", "ok", f);

    /* 20/21: 30 frames, one B/D pair around all of them; each frame resolves before the next */
    e1_mark(20, "frames-B", "ok", "");
    unsigned frames = 0;
    uint64_t retired = 0;
    snprintf(res, sizeof res, "ok");
    for (unsigned fr = 0; fr < E1_FRAMES; fr++) {
        ps3dk_prof_gpu_zone z = PS3DK_PROF_GPU_ZONE_INIT;
        if (e1_logging_failed() || e1_ext_fault) {
            snprintf(res, sizeof res, "stopped");
            break;
        }
        ps3dk_prof_frame();
        if ((rc = ps3dk_prof_gpu_frame(gpu, ps3dk_prof_frame_token())) != 0) {
            e1_hex_err(res, sizeof res, "frame", rc);
            break;
        }
        if ((rc = PS3DK_PROF_GPU_ZONE_BEGIN(gpu, ctx, "e1-gpu", z)) != 0) {
            e1_hex_err(res, sizeof res, "begin", rc);
            break;
        }
        E1_NOP_BLOCK(ctx);
        if ((rc = PS3DK_PROF_GPU_ZONE_END(gpu, ctx, z)) != 0) {
            e1_hex_err(res, sizeof res, "end", rc);
            break;
        }
        if ((rc = PS3DK_PROF_GPU_MARK(gpu, ctx, "e1-gpu-mark")) != 0) {
            e1_hex_err(res, sizeof res, "mark", rc);
            break;
        }
        cellGcmFlush(ctx);
        /* poll until this frame's 3 events are drained (report reuse only after retirement) */
        uint64_t t0 = e1_now_us();
        int seen = 0;
        for (;;) {
            if ((rc = ps3dk_prof_drain()) != 0) {
                e1_hex_err(res, sizeof res, "drain", rc);
                e1_ext_fault = 1;
                break;
            }
            if ((rc = ps3dk_prof_gpu_get_stats(gpu, &st)) != 0) {
                e1_hex_err(res, sizeof res, "stats", rc);
                e1_ext_fault = 1;
                break;
            }
            retired = st.events;
            if (st.events >= 3u * (fr + 1u)) {
                seen = 1;
                break;
            }
            if (e1_now_us() - t0 > E1_WAIT_US) {
                snprintf(res, sizeof res, "timeout");
                e1_unres[1] = 'u';
                break;
            }
            E1_SLEEP_US(1000);
        }
        if (!seen)
            break;
        frames++;
        e1_count[1] = (unsigned)retired;
    }
    int frames_ok = frames == E1_FRAMES;
    if (!frames_ok)
        e1_failed = 1;
    snprintf(f, sizeof f, "frames=%u events_retired=%llu", frames, (unsigned long long)retired);
    e1_mark(21, "frames-D", res, f);

    /* 22: detach (inline drains in APP mode); a failed or skipped detach leaves the domain borrowed */
    if (e1_logging_failed() || e1_ext_fault)
        return;
    rc = ps3dk_prof_gpu_detach(gpu, ctx, E1_WAIT_US);
    if (rc == PS3DK_PROF_ETIMEDOUT)
        snprintf(res, sizeof res, "timeout");
    else if (rc)
        e1_hex_err(res, sizeof res, "detach", rc);
    if (rc) {
        e1_failed = 1;
        e1_mark(22, "detach-D", res, "");
        return;
    }
    e1_mark(22, "detach-D", "ok", "");
    if (!frames_ok)
        return; /* a frames failure: no release (the domain's resources stay borrowed) */

    /* 23/24: the idle operation (blocking until the GPU reaches the reference) */
    e1_mark(23, "idle-B", "ok", "");
    cellGcmFinish(ctx, 0x45310001u);
    e1_mark(24, "idle-D", "ok", "");

    /* 25: release */
    uint32_t released = 0;
    rc = ps3dk_prof_gpu_release(gpu, &released);
    if (rc) {
        e1_hex_err(res, sizeof res, "release", rc);
        e1_failed = 1;
    } else
        snprintf(res, sizeof res, "ok");
    snprintf(f, sizeof f, "released=%u", released ? 1u : 0u);
    e1_mark(25, "release-D", res, f);
}

/* 28: read the sealed capture back (FNV-1a 64 over its bytes; the host reader checks the format) */
static void e1_capture_check(void)
{
    ps3dk_prof_capture_record rec;
    char res[64], f[96];
    uint8_t buf[4096];
    uint64_t h = 0xcbf29ce484222325ull, size = 0;
    int rc = ps3dk_prof_get_capture_record(&rec);
    if (rc) {
        e1_hex_err(res, sizeof res, "open", rc);
        e1_failed = 1;
        e1_mark(28, "capture-check-D", res, "");
        return;
    }
    int fd = E1_CAP_OPEN(rec.path);
    if (fd < 0) {
        e1_hex_err(res, sizeof res, "open", errno);
        e1_failed = 1;
        e1_mark(28, "capture-check-D", res, "");
        return;
    }
    for (;;) {
        long n = (long)E1_CAP_READ(fd, buf, sizeof buf);
        if (n < 0 && errno == EINTR)
            continue;
        if (n < 0) {
            e1_hex_err(res, sizeof res, "read", errno);
            (void)E1_CAP_CLOSE(fd);
            e1_failed = 1;
            e1_mark(28, "capture-check-D", res, "");
            return;
        }
        if (n == 0)
            break;
        for (long i = 0; i < n; i++) {
            h ^= buf[i];
            h *= 0x100000001b3ull;
        }
        size += (uint64_t)n;
    }
    (void)E1_CAP_CLOSE(fd);
    if (size == 0) {
        e1_hex_err(res, sizeof res, "size", 0);
        e1_failed = 1;
        e1_mark(28, "capture-check-D", res, "");
        return;
    }
    snprintf(f, sizeof f, "size=%llu fnv=%016llx", (unsigned long long)size, (unsigned long long)h);
    e1_mark(28, "capture-check-D", "ok", f);
}

static uint64_t e1_build_value(void)
{
    uint64_t v = 0;
    for (unsigned i = 0; i < 16u; i++) {
        char c = E1_BUILD_ID[i];
        v = (v << 4) | (uint64_t)(c >= 'a' ? c - 'a' + 10 : c - '0');
    }
    return v;
}

int main(void)
{
    char res[64];
    int rc;
    e1_spu sp;
    memset(&sp, 0, sizeof sp);
    e1_clock_init();
    e1_run = E1_TB() ^ e1_build_value();

    /* the run directory: created fresh (an existing one, or any failure, ends the probe with no marker at all) */
    if (E1_MKDIR(E1_BASE) != 0 && errno != EEXIST)
        return 2;
    rc = snprintf(e1_dir, sizeof e1_dir, "%s/%016llx", E1_BASE, (unsigned long long)e1_run);
    if (rc < 0 || (size_t)rc >= sizeof e1_dir || E1_MKDIR(e1_dir) != 0)
        return 2;
    e1_mark(1, "main-entered", "ok", "abi=" E1_ABI_NAME);

    /* 02: init (APP drain mode: this thread drains) */
    {
        ps3dk_prof_config cfg;
        ps3dk_prof_ext_config ext;
        const char *call = NULL;
        ps3dk_prof_config_defaults(&cfg);
        cfg.mode = PS3DK_PROF_MODE_APP;
        cfg.app = "extp";
        cfg.build_id = E1_BUILD_ID;
        memset(&ext, 0, sizeof ext);
        ext.size = PS3DK_PROF_EXT_CONFIG_SIZE;
        ext.spu_lanes_max = 1;
        ext.spu_ring_slots = 1024;
        ext.spu_image_bytes_max = 1u << 20;
        ext.gpu_domains_max = 1;
        if ((rc = ps3dk_prof_set_sink_path(e1_dir)) != 0)
            call = "sink-path";
        else if ((rc = ps3dk_prof_init_ex(&cfg, &ext)) != 0)
            call = "init-ex";
        if (call) {
            e1_hex_err(res, sizeof res, call, rc);
            e1_mark(2, "prof-init-done", res, "");
            return 1; /* nothing was acquired */
        }
        e1_mark(2, "prof-init-done", "ok", "");
    }

    /* 03: this thread's incarnation (the lane owner and the GPU ctx owner) */
    int begun = 0;
    if ((rc = ps3dk_prof_thread_begin("main")) != 0) {
        e1_hex_err(res, sizeof res, "thread-begin", rc);
        e1_failed = 1;
        e1_mark(3, "thread-begin-done", res, "");
    } else {
        begun = 1;
        e1_mark(3, "thread-begin-done", "ok", "");
        e1_spu_phase(&sp);
        e1_spu_cleanup(&sp);
        if (sp.clean == 2 && !e1_ext_fault && !e1_logging_failed())
            e1_gpu_phase();
    }

    /* 26/27: shutdown (always attempted after a successful init) */
    e1_mark(26, "shutdown-B", "ok", "");
    if (begun)
        ps3dk_prof_thread_end();
    rc = ps3dk_prof_shutdown();
    if (rc) {
        e1_hex_err(res, sizeof res, "shutdown", rc);
        e1_failed = 1;
        e1_mark(27, "shutdown-D", res, "");
    } else {
        e1_mark(27, "shutdown-D", "ok", "");
        e1_capture_check();
    }
    return e1_failed || e1_logging_failed() ? 1 : 0;
}
