#ifndef HETEROGENEOUS_PROF_EXT_H
#define HETEROGENEOUS_PROF_EXT_H
/* SPU-lane and RSX-domain profiling for the heterogeneous workload (SAMPLE_PROF_EXT; design
 * docs/local/prof-viewer/hw-sample-spu-rsx-design.md r1 + r2/r3 deltas + codex 20:57 and 21:08 corrections).
 *
 * Pure logic and the lifecycle ORDER live here, with no SDK header: the profiler and GCM calls go through ports,
 * bound to the real API in prof_ext_bind.c (target) and to mocks in host-tests/test_prof_ext.c.
 *
 * SPU epochs (W32): a word carries PE_FLAG_RESET when the worker's last reset is PE_RESET_US old, and idle workers
 * get reset-only words on the same rule, so each epoch is serviced (flush, CAL, dec_write, fini) within
 * PE_EPOCH_BOUND_US of its reset.  The OLD epoch of a reset stays tracked until the worker acknowledges the word:
 * its service (the flush before the dec_write) may happen as late as that acknowledgement, so both epochs are
 * checked at completion.  Any check that fails, any SPU-side failure, a deadline overrun, a partial send, a
 * missing final marker or a failed join clears the capture-wide w32_eligible flag for good. */
#include <stdint.h>

#define PE_MAX_WORKERS 6u
#define PE_RESET_US 20000000ull      /* reset when the last one is this old */
#define PE_DEADLINE_US 2000000ull    /* the sample's SPE job deadline */
#define PE_EPOCH_BOUND_US (PE_RESET_US + PE_DEADLINE_US)
#define PE_FLAG_RESET 0x80000000u    /* job word: flush + dec_write before the job */
#define PE_FLAG_RESET_ONLY 0x40000000u /* job word: flush + dec_write, no work */
#define PE_SEQ_MASK 0x3fffffffu      /* also the final marker's sequence */
/* GPU resources (audited, host-tests/label-audit.txt: the sample uses timestamp reports 0/1 and label 240) */
#define PE_GPU_REPORT_FIRST 32u
#define PE_GPU_REPORT_COUNT 32u
#define PE_GPU_LABEL 241u            /* owned by the profiler's ticket protocol: never written by the sample */
#define PE_FENCE_LABEL 242u          /* the application's completion fence */
#define PE_WAIT_US 2000000ull
/* ext config */
#define PE_RING_SLOTS 1024u
#define PE_IMAGE_BYTES (256u * 1024u)
#define PE_MEM_MAX (2u * 1024u * 1024u)
#define PE_LANE_BYTES (PE_RING_SLOTS * 8u + 256u)
/* SPU status word (work_slot.reserved[0], EXT builds only) */
#define PE_SPU_ATTACH_FAILED 1u
#define PE_SPU_RESET_FAILED 2u
#define PE_SPU_FLUSH_FAILED 4u
#define PE_SPU_FINI_FAILED 8u
#define PE_SPU_ANY_FAILURE 15u

typedef struct {
    uint64_t last_reset_us; /* the current epoch's reset (the PPU dispatch time of its word: an early bound) */
    uint64_t old_reset_us;  /* the previous epoch's reset, while its acknowledgement is pending */
    uint64_t dispatch_us;   /* the word in flight */
    int started, ack_pending, in_flight;
} pe_worker;

typedef struct {
    pe_worker w[PE_MAX_WORKERS];
    unsigned workers;
    int w32_eligible; /* capture-wide; cleared for good */
    const char *why;  /* the first reason it was cleared */
} pe_epochs;

void pe_epochs_init(pe_epochs *e, unsigned workers, uint64_t start_us);
/* The word for worker i at now_us (sequence <= PE_SEQ_MASK - 1, checked by the caller).  `work` 0 asks for a
 * reset-only word, returned only when a reset is due (else 0: nothing to send). */
uint32_t pe_job_word(pe_epochs *e, unsigned i, uint32_t sequence, uint64_t now_us, int work);
/* The word in flight for worker i completed at done_us with the worker's status word. */
void pe_job_done(pe_epochs *e, unsigned i, uint64_t done_us, uint32_t spu_status);
/* The worker's final marker (after its fini) was seen at now_us with its final status word. */
void pe_final(pe_epochs *e, unsigned i, uint64_t now_us, uint32_t spu_status);
void pe_clear(pe_epochs *e, const char *why);

/* SPU thread arguments.  OFF: arg0 slot EA, arg1 lane, arg2 lanes, arg3 0 (today's ABI exactly).  EXT: arg0 slot
 * EA, arg1 (lane << 32) | lanes, arg2 ring EA, arg3 (ring_slots << 32) | control-block EA (32-bit EAs). */
typedef struct {
    uint64_t arg0, arg1, arg2, arg3;
} pe_args;
pe_args pe_pack_off(uint32_t slot_ea, uint32_t lane, uint32_t lanes);
int pe_pack_ext(uint32_t slot_ea, uint32_t lane, uint32_t lanes, uint64_t ring_ea, uint32_t ring_slots,
                uint64_t ctl_ea, pe_args *out);
typedef struct {
    uint32_t slot_ea, lane, lanes, ring_slots;
    uint64_t ring_ea, ctl_ea;
} pe_unpacked;
pe_unpacked pe_unpack_ext(pe_args a);

/* ---- the application fence (label PE_FENCE_LABEL only) --------------------------------------------------------- */

typedef struct pe_fence_io {
    void *ctx;
    volatile uint32_t *(*label_addr)(void *ctx, uint32_t label);
    void (*write_label)(void *ctx, uint32_t label, uint32_t value); /* a back-end label write in the command stream */
    void (*flush)(void *ctx);
    void (*barrier)(void *ctx);
    uint64_t (*now_us)(void *ctx);
    void (*sleep_us)(void *ctx, unsigned us);
} pe_fence_io;

typedef struct {
    volatile uint32_t *label;
    uint32_t value;
} pe_fence_state;

/* Writes a fresh value to PE_FENCE_LABEL after the last submitted commands, flushes and waits (PE_WAIT_US).
 * 0: the GPU passed every earlier command; -1: no label address or a timeout. */
int pe_fence(pe_fence_state *f, const pe_fence_io *io);

/* ---- the run receipts ------------------------------------------------------------------------------------------
 * A loader-launched run has no captured stdout.  Start notes are appended to <csv stem>.ext.txt (informational).
 * The ACCEPTANCE record is <csv stem>.ext-end.txt: written ONCE, after the CSV's own checked finalization, to a fresh
 * temporary <csv stem>.ext-end.tmp, fsynced and closed, and only then renamed to its final name.  Any failure
 * (open, write, fsync, close, rename) leaves no acceptance file; the judge ignores temporary files and requires
 * exactly one complete END record (ending in "receipt=complete") in the acceptance file of this run. */

typedef struct pe_file_io {
    void *ctx;
    int (*open_append)(void *ctx, const char *path); /* fd, or < 0 */
    int (*open_trunc)(void *ctx, const char *path);  /* a fresh empty file: fd, or < 0 */
    long (*write)(void *ctx, int fd, const void *buf, unsigned long n); /* bytes written, or < 0 */
    int (*fsync)(void *ctx, int fd);
    int (*close)(void *ctx, int fd);
    int (*rename)(void *ctx, const char *from, const char *to);
    int (*unlink)(void *ctx, const char *path);
} pe_file_io;

/* csv_path ending in ".csv" -> the same path with `suffix` in place of ".csv"; -1 when it does not end in .csv or
 * the result does not fit. */
int pe_receipt_path(const char *csv_path, const char *suffix, char *out, unsigned long n);
/* Appends `line` completely (short writes continued, a zero-byte write is a failure), then fsync and close, each
 * checked.  0 only when every step succeeded.  For start notes only (not an acceptance record). */
int pe_receipt_write(const pe_file_io *io, const char *path, const char *line);
/* Publishes the acceptance record: `line` into a fresh `tmp_path` (complete write, fsync, close, each checked), then
 * rename to `final_path`.  On any failure the temporary file is unlinked (best effort) and `final_path` is never
 * created.  0 only when the record is published. */
int pe_receipt_publish(const pe_file_io *io, const char *tmp_path, const char *final_path, const char *line);
/* The END line; -1 when it does not fit. */
int pe_end_line(char *buf, unsigned long n, int join_ok, int w32_eligible, const char *why, int rc, unsigned frames,
                unsigned checked, unsigned phase_mask);

/* ---- the lifecycle, through a port ------------------------------------------------------------------------- */

typedef struct {
    uint32_t handle, ring_slots;
    uint64_t ea_ring, ea_ctl;
} pe_lane;

typedef struct pe_port {
    void *ctx;
    int (*init_ex)(void *ctx, unsigned lanes);
    int (*thread_begin)(void *ctx);
    int (*register_image)(void *ctx, uint32_t *image);
    int (*lane_create)(void *ctx, uint32_t image, pe_lane *out);
    int (*lane_end)(void *ctx, uint32_t handle);
    int (*gpu_attach)(void *ctx, uint32_t *gpu);
    int (*gpu_calibrate)(void *ctx, uint32_t gpu);
    int (*drain)(void *ctx);
    int (*fence)(void *ctx); /* pe_fence on the production io */
    int (*gpu_detach)(void *ctx, uint32_t gpu);
    int (*gpu_release)(void *ctx, uint32_t gpu);
    int (*thread_end)(void *ctx);
    int (*shutdown)(void *ctx);
    void (*log)(void *ctx, const char *what, int rc);
} pe_port;

typedef struct {
    int inited, thread, spu, gpu; /* what is live */
    uint32_t image, gpu_id;
    unsigned lanes;
    pe_lane lane[PE_MAX_WORKERS];
    int lane_live[PE_MAX_WORKERS];
    int retained_lanes, retained_gpu; /* kept because they may still be referenced */
} pe_state;

/* init_ex -> thread_begin -> register -> one lane per worker.  Returns 0 when the profiler is up (SPU lanes may
 * be off after a registration/lane failure, logged); nonzero when init itself failed (nothing to undo). */
int pe_start(pe_state *s, const pe_port *p, unsigned workers);
/* After scene_init: fence (quiescent context), then attach.  A failure leaves GPU profiling off. */
void pe_gpu_start(pe_state *s, const pe_port *p);
/* Calibration point: calibrate + drain (checked, logged; never fatal). */
void pe_gpu_calibrate(pe_state *s, const pe_port *p);
/* Teardown after the frame loop.  `join_ok`: the SPU group joined after every final marker.  Order: calibrate +
 * drain, fence, detach, release only when fence AND detach succeeded; lane_end per live lane only when joined;
 * thread_end; shutdown unless something was retained.  Returns nonzero when anything failed or was retained. */
int pe_stop(pe_state *s, const pe_port *p, int join_ok);

#endif
