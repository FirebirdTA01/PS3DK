/* Delayed FIFO accounting rows against retired capture-window declarations.
 * Included by runtime_schema2_test.c only under TEST_DELAYED_FIFO.
 *
 * Producer, drainer, FIFO accounting queue and writer are the real ones. The
 * only seam is the existing link wrap of ps3dk_prof__fifo_emit_chunk:
 *  - DEFER: while held, a type 43 emit returns nonzero WITHOUT calling the real
 *    emitter. drain_accounting() then leaves read_idx unchanged, so the real
 *    16-row queue keeps the row (FIFO head) and drops the newest rows on
 *    overflow, exactly as under sink back-pressure. Nothing is acknowledged.
 *  - FORGE (boundary/negative cases only): at release, the held row's window
 *    or frame field is rewritten in the drainer's own payload buffer before the
 *    real emitter runs. The rows queued behind it in the boundary case are
 *    swallowed (accepted by the test, never written), which the writer sees as
 *    an ordinary producer sequence gap.
 * The retired frontier oracle is a test link wrap of the writer's prepare: it
 * reads the schema3 shared header words (88 retired count, 96 greatest retired
 * id, 104 its last frame) from the canonical checkpoint the real writer just
 * encoded. A checkpoint below schema3 reports no retirement. */
#ifndef S2_LATE_WINDOWS
#define S2_LATE_WINDOWS 40u          /* FRAMES windows after the whole-run window */
#endif
#ifndef S2_LATE_WINDOWS_MAX
#define S2_LATE_WINDOWS_MAX 64u      /* extra windows allowed for the frontier to pass */
#endif
#ifndef S2_LATE_DRAWS
#define S2_LATE_DRAWS 100u           /* per frame: enough OPS volume to rotate 6 slots */
#endif
#define S2_LATE_TARGET 3u            /* whole-run = 1, first FRAMES = 2, held = second FRAMES */
#define S2_LATE_QUEUE 16u            /* FIFO_ACCOUNT_ROWS */

enum { S2L_RETIRED = 1, S2L_BOUNDARY, S2L_PAST, S2L_FUTURE, S2L_ZERO };
static struct {
    int mode, held, released, forged, refused_seen, saved;
    uint64_t accepted_held_index;
    uint8_t original[80];
    uint64_t held_seq, held_frame, held_window, deferrals;
    uint64_t forge_window, forge_frame;
    int held_rc;                                /* real emitter result for the held row */
    uint64_t queued_delivered, swallowed, first_after_seq, last_seq_seen;
    uint64_t release_frame;
    /* writer oracle: latest accepted canonical checkpoint */
    uint64_t retired_count, retired_max, retired_last, schema;
    uint64_t at_release_count, at_release_max, at_release_last;
    /* real output refusal */
    int invalid_rc; uint32_t invalid_type; uint64_t invalid_seq, invalid_window, invalid_frame;
} s2l;

static uint64_t s2l_u64(const uint8_t *b) { return s2_r64(b); }
static void s2l_put64(uint8_t *b, uint64_t v) { for (unsigned i = 0; i < 8; ++i) b[i] = (uint8_t)(v >> (8 * i)); }

/* 1 = handled (return *rc without calling the real emitter); 0 = call it. */
static int s2_late_emit_before(uint32_t ty, uint8_t *b, size_t bytes, int *rc)
{
    uint64_t seq, frame, window;
    if (!s2l.mode || ty != 43) return 0;
    S2(bytes == 80, "accounting-row-payload");
    frame = s2l_u64(b + 24); window = s2l_u64(b + 32); seq = s2l_u64(b + 64);
    if (!s2l.held && window == S2_LATE_TARGET) {
        s2l.held = 1; s2l.held_seq = seq; s2l.held_frame = frame; s2l.held_window = window;
        memcpy(s2l.original, b, 80);
    }
    if (s2l.held && !s2l.released) {
        S2(seq == s2l.held_seq && !memcmp(b, s2l.original, 80), "held-row-stays-queue-head");
        ++s2l.deferrals; *rc = -1; return 1;     /* real queue keeps it; nothing acknowledged */
    }
    if (s2l.released && seq == s2l.held_seq && s2l.mode != S2L_RETIRED && !s2l.forged) {
        s2l_put64(b + 32, s2l.forge_window); s2l_put64(b + 24, s2l.forge_frame); s2l.forged = 1;
    }
    if (s2l.released && s2l.mode == S2L_BOUNDARY && seq > s2l.held_seq && seq < s2l.held_seq + S2_LATE_QUEUE) {
        ++s2l.swallowed; *rc = 0; return 1;      /* test drop: a writer-visible sequence gap */
    }
    return 0;
}
static void s2_late_emit_after(uint32_t ty, const uint8_t *b, int rc)
{
    uint64_t seq;
    if (!s2l.mode || ty != 43) return;
    seq = s2l_u64(b + 64);
    if (s2l.held && seq == s2l.held_seq) s2l.held_rc = rc;
    if (rc) return;
    if (s2l.held && seq >= s2l.held_seq && seq < s2l.held_seq + S2_LATE_QUEUE) {
        S2(!s2l.last_seq_seen || seq == s2l.last_seq_seen + 1u, "queued-rows-consecutive");
        ++s2l.queued_delivered;
    } else if (s2l.held && seq > s2l.held_seq && !s2l.first_after_seq) s2l.first_after_seq = seq;
    s2l.last_seq_seen = seq;
}
static void s2_late_accept(int rc, const uint8_t *raw)
{
    if (!s2l.mode) return;
    if (rc == 0) {
        pfo_view out;
        if (s2_r32(raw) == 43 && s2l_u64(raw + 16 + 64) == s2l.held_seq)
            s2l.accepted_held_index = s2l_u64(raw + 8);
        if (s2l.accepted_held_index && !s2l.saved) {
            S2(pfo_get_view(g_prof->rolling->output, &out) == 0, "late-commit-view");
            if (out.committed_next_chunk > s2l.accepted_held_index) {
                /* Snapshot at the first real commit containing the row. A
                 * single drain can commit >6 groups and legitimately evict
                 * its earliest rows before returning to the application. */
                s2_copy("late-release.profr"); s2l.saved = 1;
            }
        }
        return;
    }
    if (rc == PFO_RESOURCE || s2l.refused_seen) return;
    s2l.refused_seen = 1; s2l.invalid_rc = rc; s2l.invalid_type = s2_r32(raw);
    if (s2l.invalid_type == 43) {
        s2l.invalid_frame = s2l_u64(raw + 16 + 24); s2l.invalid_window = s2l_u64(raw + 16 + 32);
        s2l.invalid_seq = s2l_u64(raw + 16 + 64);
    }
}
int __real_ps3dk_prof__roll_combined2_prepare(void *, const void *, void *, const void *, size_t,
                                              void *, size_t, pfo_prepared *);
int __wrap_ps3dk_prof__roll_combined2_prepare(void *c, const void *a, void *b, const void *chunk, size_t n,
                                              void *cp, size_t cap, pfo_prepared *out)
{
    int rc = __real_ps3dk_prof__roll_combined2_prepare(c, a, b, chunk, n, cp, cap, out);
    const uint8_t *k = cp;
    if (rc == 0 && out && out->checkpoint_bytes >= 120 && !memcmp(k, "PFRMIX01", 8)) {
        s2l.schema = (uint64_t)k[8] | (uint64_t)k[9] << 8;
        if (s2l.schema >= 3) {
            uint64_t count = s2l_u64(k + 88), max = s2l_u64(k + 96), last = s2l_u64(k + 104);
            S2(count >= s2l.retired_count && max >= s2l.retired_max && last >= s2l.retired_last,
               "retired-frontier-monotone");
            s2l.retired_count = count; s2l.retired_max = max; s2l.retired_last = last;
        }
    }
    return rc;
}
/* s2_settle, except that a writer stop (resource or refusal) is reported as the
 * verdict instead of hanging on OPS that a stopped capture never acknowledges. */
static void s2_late_settle(const auto_capture *a)
{
    uint64_t end = s2_now() + UINT64_C(20000000000); s2_resume();
    while (LD_ACQ(&a->operations.read_idx) != LD_ACQ(&a->operations.write_idx)) {
        if (LD_ACQ(&g_prof->stop_latch) || LD_ACQ(&g_prof->snk.failed)) {
            fprintf(stderr, "LATE_STOP: resource_refusals=%u refused_type=%u refused_next=%" PRIu64
                    " invalid_rc=%d invalid_type=%u held=%d released=%d retired_max=%" PRIu64 " schema=%" PRIu64 "\n",
                    s2_refusals, s2_refused_type, s2_refused_next, s2l.invalid_rc, s2l.invalid_type,
                    s2l.held, s2l.released, s2l.retired_max, s2l.schema);
            S2(0, "capture-continues-before-late-release");
        }
        S2(s2_now() < end, "accepted-queue-deadline"); sched_yield();
    }
    s2_pause();
    S2(RETIRED(a) == LD_ACQ(&a->ring.issued), "accepted-blocks-retire");
}
/* One application frame: draws, native publication, ONE frame boundary, then a
 * real settled drainer pass (the rotation recipe's order: the boundary closes
 * the frame's OPS before the settle waits for them). */
static void s2_late_tick(const auto_capture *a)
{
    for (uint32_t i = 0; i < S2_LATE_DRAWS; ++i) draw();
    s2_app_flush(0); ps3dk_prof_frame(); s2_app_flush(0); s2_app_flush(0); s2_late_settle(a);
}
static void s2_late_wait_refusal(void)
{
    uint64_t end = s2_now() + UINT64_C(20000000000);
    s2_app_flush(0); s2_resume();
    while (!LD_ACQ(&g_prof->snk.failed)) { S2(s2_now() < end, "refusal-deadline"); sched_yield(); }
}
static void s2_late_manifest(const char *name, int rc, const ps3dk_prof_test_rolling_record *r)
{
    char p[1200]; FILE *f; s2_path(p, sizeof p, "late.json"); f = fopen(p, "wbx"); S2(f, "late-manifest-open");
    S2(fprintf(f, "{\"schema\":1,\"case\":\"%s\",\"shutdown\":%d,\"state\":%u,\"reason\":%u,\"io_result\":%d,"
        "\"held_seq\":%" PRIu64 ",\"held_frame\":%" PRIu64 ",\"held_window\":%" PRIu64 ",\"release_frame\":%" PRIu64 ","
        "\"deferrals\":%" PRIu64 ",\"queued_delivered\":%" PRIu64 ",\"swallowed\":%" PRIu64 ",\"first_after_seq\":%" PRIu64 ","
        "\"retired_count\":%" PRIu64 ",\"retired_max\":%" PRIu64 ",\"retired_last\":%" PRIu64 ","
        "\"forged_window\":%" PRIu64 ",\"forged_frame\":%" PRIu64 ",\"invalid_rc\":%d,\"invalid_type\":%u,\"invalid_seq\":%" PRIu64 ","
        "\"original_row_hex\":\"",
        name, rc, r->state, r->reason, r->io_result, s2l.held_seq, s2l.held_frame, s2l.held_window, s2l.release_frame,
        s2l.deferrals, s2l.queued_delivered, s2l.swallowed, s2l.first_after_seq,
        s2l.at_release_count, s2l.at_release_max, s2l.at_release_last, s2l.forge_window, s2l.forge_frame,
        s2l.invalid_rc, s2l.invalid_type, s2l.invalid_seq) > 0, "late-manifest-write");
    for (unsigned i = 0; i < 80; ++i) S2(fprintf(f, "%02x", s2l.original[i]) > 0, "late-manifest-row");
    S2(fputs("\"}\n", f) >= 0 && !fclose(f), "late-manifest-close");
}
static int s2_late_main(const char *name)
{
    const auto_capture *a; ps3dk_prof_capture_info info = {0}; ps3dk_prof_test_rolling_record r = {0};
    uint32_t w; int rc;
    s2l.mode = !strcmp(name, "late-retired") ? S2L_RETIRED : !strcmp(name, "late-boundary") ? S2L_BOUNDARY :
               !strcmp(name, "late-past-boundary") ? S2L_PAST : !strcmp(name, "late-future") ? S2L_FUTURE :
               !strcmp(name, "late-zero") ? S2L_ZERO : 0;
    S2(s2l.mode, "known-late-case");
    s2_setup(1, 0); a = s2_attach();
    /* Close s2_setup's whole-run window (id 1) so every later window is a 2-frame FRAMES window. */
    S2(ps3dk_prof_capture_end() == 0, "whole-run-END"); s2_late_tick(a); s2_late_tick(a);
    for (w = 0; w < S2_LATE_WINDOWS_MAX; ++w) {
        S2(PS3DK_PROF_CAPTURE_FRAMES("delayed-fifo", 2) == 0, "frames-window-BEGIN");
        s2_late_tick(a); s2_late_tick(a); s2_late_tick(a);       /* opens, second frame, closes */
        S2(ps3dk_prof_capture_state(&info) == 0 && info.state == 0 && info.id == w + 2u, "window-closed-between");
        if (!s2l.released && s2l.held && w + 1u >= S2_LATE_WINDOWS &&
            s2l.retired_max >= S2_LATE_TARGET + S2_LATE_QUEUE / 2u) break;
    }
    /* Preconditions of the scenario, not of the product verdict. */
    S2(s2l.held && s2l.deferrals > 0 && !s2l.released, "row-held-by-real-queue");
    S2(s2l.schema >= 3, "schema3-retired-frontier-present");
    S2(s2l.retired_max >= S2_LATE_TARGET + S2_LATE_QUEUE / 2u && s2l.held_frame <= s2l.retired_last,
       "all-queued-windows-in-retired-prefix");
    {   ps3dk_prof_token t = ps3dk_prof_frame_token(); s2l.release_frame = t.frame;
        S2(t.frame > s2l.held_frame + S2_LATE_QUEUE, "delay-exceeds-queue-rows-in-frames"); }
    s2l.at_release_count = s2l.retired_count; s2l.at_release_max = s2l.retired_max; s2l.at_release_last = s2l.retired_last;
    switch (s2l.mode) {
    case S2L_BOUNDARY: s2l.forge_window = s2l.retired_max; s2l.forge_frame = s2l.retired_last; break;
    case S2L_PAST:     s2l.forge_window = s2l.retired_max; s2l.forge_frame = s2l.retired_last + 1u; break;
    case S2L_FUTURE:   s2l.forge_window = (uint64_t)info.id + 1u; s2l.forge_frame = s2l.held_frame; break;
    case S2L_ZERO:     s2l.forge_window = 0; s2l.forge_frame = s2l.held_frame; break;
    default: break;
    }
    S2(s2l.mode != S2L_FUTURE || s2l.forge_window > s2l.retired_max, "future-id-above-retired-ceiling");
    s2l.released = 1;
    if (s2l.mode == S2L_RETIRED || s2l.mode == S2L_BOUNDARY) {
        s2_late_tick(a);                                          /* the release drain pass */
        S2(s2l.held_rc == 0, "late-row-accepted-whole");
        S2(s2l.mode != S2L_RETIRED || s2l.queued_delivered == S2_LATE_QUEUE, "all-16-queued-rows-kept");
        S2(s2l.mode != S2L_BOUNDARY || (s2l.queued_delivered == 1 && s2l.swallowed == S2_LATE_QUEUE - 1u),
           "boundary-row-alone");
        for (w = 0; w < 4; ++w) {                                 /* capture continues */
            S2(PS3DK_PROF_CAPTURE_FRAMES("delayed-fifo", 2) == 0, "post-release-window-BEGIN");
            s2_late_tick(a); s2_late_tick(a); s2_late_tick(a);
        }
        S2(s2l.saved, "released-row-observed-at-its-commit");
        S2(s2l.first_after_seq > s2l.held_seq + S2_LATE_QUEUE, "newest-rows-dropped-as-seq-gap");
        s2_resume(); ps3dk_prof_thread_end();
        rc = ps3dk_prof_shutdown();
        S2(ps3dk_prof_test_rolling_result(&r) == 0, "late-result");
        s2_late_manifest(name, rc, &r);
        S2(!s2l.refused_seen && s2_refusals == 0, "no-writer-refusal");
        S2(rc == 0 && r.state == PFR_SEALED && r.reason == PFR_REASON_NONE && !r.io_result, "capture-continues-sealed");
        S2(s2_ledger_at == s2_ledger_count && !s2_pending_generation, "accepted-OPS-ledger-exhausted");
        s2_copy("capture.profr");
    } else {
        s2_late_wait_refusal();
        ps3dk_prof_thread_end();
        rc = ps3dk_prof_shutdown();
        S2(ps3dk_prof_test_rolling_result(&r) == 0, "late-result");
        s2_late_manifest(name, rc, &r);
        S2(s2l.forged && s2l.held_rc != 0, "forged-row-not-accepted");
        S2(s2l.refused_seen && s2l.invalid_rc == PFR_INVALID && s2l.invalid_type == 43 &&
           s2l.invalid_seq == s2l.held_seq && s2l.invalid_window == s2l.forge_window &&
           s2l.invalid_frame == s2l.forge_frame, "malformed-refusal-is-the-forged-row");
        S2(s2_refusals == 0, "not-a-resource-stop");
        S2(rc == PS3DK_PROF_ESINKFAIL && r.io_result == PFR_INVALID && r.state != PFR_SEALED, "malformed-input-refusal");
        s2_copy("capture.profr");
    }
    puts("delayed FIFO accounting case PASS");
    return 0;
}
