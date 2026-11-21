/* RSX frame capture runtime (INTERNAL; docs/design/ps3dk-prof-format.md section 12, design
 * .local/rsx-frame-capture/DESIGN.md r2). An AUTO member: dormant unless the executable defines
 * ps3dk_prof_automatic_rsx_frame_options (PS3_PROF_RSX_FRAME=ON).
 *
 * Threads and ownership:
 *  - the AUTO owner thread runs every hook below (draw begin/end, prewrap, preflush, frame boundary, program bind,
 *    labels) and is the only writer of the decoder, the label state and the current staging block;
 *  - a requester (the live agent thread, or any app thread through ps3dk_prof_rsx_frame_capture) claims the slot word
 *    and allocates the staging blocks; nothing else;
 *  - the drainer (core drain serialization) emits CONFIG, BEGIN, sealed blocks and END, returns blocks, and frees the
 *    staging after END.
 * The slot word is one atomic u64 with the RSXCAP_* layout of rsxcap.h (state, flags, epoch = capture id, outcome
 * and reason in TERMINAL); every transition is a whole-word CAS. No locks, timers or waits on the owner path. */
#ifndef PS3DK_PROF_RSXF_H
#define PS3DK_PROF_RSXF_H
#include <stddef.h>
#include <stdint.h>

#define rsxf_selected ps3dk_prof__rsxf_selected
#define rsxf_attach ps3dk_prof__rsxf_attach
#define rsxf_scan ps3dk_prof__rsxf_scan
#define rsxf_wrap ps3dk_prof__rsxf_wrap
#define rsxf_reset ps3dk_prof__rsxf_reset
#define rsxf_hook_begin ps3dk_prof__rsxf_hook_begin
#define rsxf_hook_admitted ps3dk_prof__rsxf_hook_admitted
#define rsxf_hook_end ps3dk_prof__rsxf_hook_end
#define rsxf_frame ps3dk_prof__rsxf_frame
#define rsxf_forced ps3dk_prof__rsxf_forced
#define rsxf_drain ps3dk_prof__rsxf_drain
#define rsxf_note_column ps3dk_prof__rsxf_note_column
#define rsxf_isolated ps3dk_prof__rsxf_isolated
#define rsxf_pixel_force ps3dk_prof__rsxf_pixel_force
#define rsxf_skip ps3dk_prof__rsxf_skip
#define rsxf_profiler_reports ps3dk_prof__rsxf_profiler_reports

/* ---- wire (rsxf_wire.c): pure encoders, little-endian, every reserved byte zero ---------------------------------- */
enum { RSXF_CHUNK_CONFIG = 44, RSXF_CHUNK_BEGIN = 45, RSXF_CHUNK_DRAWS = 46, RSXF_CHUNK_END = 48 };
enum { RSXF_CONFIG_BYTES = 64, RSXF_BEGIN_FIXED = 56, RSXF_DRAWS_HEADER = 16, RSXF_END_BYTES = 128,
       RSXF_DRAW_BYTES = 96, RSXF_PROGRAM_BYTES = 48, RSXF_NAME_MAX = 63, RSXF_TEXT_MAX = 63 };
enum { RSXF_REC_DRAW = 1, RSXF_REC_PROGRAM = 2, RSXF_REC_LABEL = 3, RSXF_REC_NAME = 4, RSXF_REC_BASELINE = 5,
       RSXF_REC_STATE = 6, RSXF_REC_VPCONST = 7, RSXF_REC_FPCONST = 8, RSXF_REC_UNIFORM = 9, RSXF_REC_UCODE = 10,
       RSXF_REC_SECOND = 11, RSXF_REC_ZCULL_REGION = 12, RSXF_REC_ZCULL_STATS = 13 };
/* ZCULL_REGION (decoder 5): one zcull region as cellGcmBindZcull bound it (or its unbind), 64 bytes. ZCULL_STATS (13) is
 * reserved for zcull statistics reports (report types 2..5): not written; their semantics need hardware
 * characterization first. */
enum { RSXF_ZCULL_REGION_BYTES = 64, RSXF_ZCULL_REGIONS = 8 };
enum { RSXF_ZR_BOUND = 1, RSXF_ZR_BEFORE_CAPTURE = 2 };
/* Request flags (= BEGIN flags): ISOLATED (WAIT_FOR_IDLE around every captured draw's markers), FP_BYTES (store FP
 * ucode bytes, up to the per-capture cap), BOTH (two consecutive frames: the first pipelined, the second isolated),
 * PIXELS (force ZPASS pixel counting on around each captured draw the application has it off for; decoder 5). */
enum { RSXF_REQ_ISOLATED = 1, RSXF_REQ_FP_BYTES = 2, RSXF_REQ_BOTH = 4, RSXF_REQ_PIXELS = 8, RSXF_REQ_ALL = 15 };
/* A draw's ZPASS pixel counting state (DRAW record u32@84, decoder 5): COUNTING the application had counting on, OFF
 * it had it off, FORCED the capture turned it on around this draw (PIXELS), PAUSED off inside an open application
 * occlusion query (never forced), INVALID reserved, UNTOUCHED no ZPASS method seen since the decoder attached (counting
 * assumed off; PIXELS forces it), UNKNOWN the decoder reset since (unseen words may have changed it). */
enum { RSXF_ZP_UNKNOWN = 0, RSXF_ZP_COUNTING = 1, RSXF_ZP_OFF = 2, RSXF_ZP_FORCED = 3, RSXF_ZP_PAUSED = 4,
       RSXF_ZP_INVALID = 5, RSXF_ZP_UNTOUCHED = 6 };
/* UCODE: a program's bytes in parts of at most RSXF_UCODE_PART bytes; header bytes. */
enum { RSXF_UCODE_PART = 2048, RSXF_UCODE_HEADER = 32, RSXF_UCODE_LAST = 1, RSXF_SENT_MAX = 256 };
/* CONFIG decoder version: 1 draws and programs; 2 also the 3D register shadow (BASELINE and STATE records); 3 also VP
 * constants (VPCONST), FP embedded constants (FPCONST) and uniform names (UNIFORM). */
enum { RSXF_DECODER = 5 }; /* 4: also SECOND and UCODE; 5: also ZPASS state, zcull events and ZCULL_REGION */
/* VP constant slots; VPCONST entries per record; FP constants per draw and uniforms per program, at most. */
enum { RSXF_VP_CONSTS = 468, RSXF_VPC_PER_REC = 128, RSXF_FPC_MAX = 32, RSXF_UNI_MAX = 32, RSXF_UNI_NAME = 32,
       RSXF_VPC_ENTRY = 24, RSXF_FPC_ENTRY = 24, RSXF_UNI_ENTRY = 40 };
enum { RSXF_VPC_RESET = 1, RSXF_VPC_BASELINE = 2, RSXF_FPC_TRUNCATED = 1, RSXF_UNI_TRUNCATED = 1 };
/* FP constant provenance, per component (FPCONST entry bytes 4..7): INLINE the last inline transfer the decoder saw
 * write that word (not exact: a CPU write after it, or a write the decoder could not see, is not reflected), CONTAINER
 * the container's value (no FIFO write to that word seen; a CPU patch would not be seen, so it may differ at GPU
 * time), UNKNOWN (the decoder reset, its inline-write map was full, an M2MF copy wrote the word's range, or a
 * different program was placed at that address after an inline write there). */
enum { RSXF_FPC_INLINE = 1, RSXF_FPC_CONTAINER = 2, RSXF_FPC_UNKNOWN = 3 };
enum { RSXF_UNI_VP = 1, RSXF_UNI_FP = 2 };
enum { RSXF_INLINE_MAP = 4096, RSXF_TAINT_RANGES = 8 };
/* Data-port destination cursors: VP instruction load slot, VP constant id, inline transfer (subchannel 3 destination
 * DMA and offset, subchannel 5 start pixel and size), M2MF copy (destination DMA, offset, pitch, line length, count). */
enum { RSXF_PORT_VP_SLOT = 1, RSXF_PORT_VPC_ID = 2, RSXF_PORT_S3_DMA = 4, RSXF_PORT_S3_OFFSET = 8, RSXF_PORT_S5_POINT = 16,
       RSXF_PORT_S5_SIZE = 32, RSXF_PORT_INLINE = 4 | 8 | 16 | 32, RSXF_PORT_M2MF_DMA = 64, RSXF_PORT_M2MF_OUT = 128,
       RSXF_PORT_M2MF_PITCH = 256, RSXF_PORT_M2MF_LEN = 512, RSXF_PORT_M2MF_COUNT = 1024,
       RSXF_PORT_M2MF = 64 | 128 | 256 | 512 | 1024 };
/* Register records: at most this many (register, value) pairs each (16 + 8 x 255 bytes). */
enum { RSXF_REG_PAIRS_MAX = 255, RSXF_REG_HEADER = 16, RSXF_STATE_RESET = 1 };
/* The largest per-draw unit (it never spans blocks): STATE for every register, VPCONST for every slot, UNIFORM for two
 * programs defined at the draw, FPCONST, the definitions and DRAW (RSXF_UNIT_DEFS bytes at most). A staging block is
 * at least RSXF_STAGING_KB_MIN KiB, so every unit fits one empty block. */
enum { RSXF_UNIT_DEFS = 512,
       RSXF_UNIT_WORST = ((2048 + RSXF_REG_PAIRS_MAX - 1) / RSXF_REG_PAIRS_MAX) * RSXF_REG_HEADER + 8 * 2048 +
                         ((RSXF_VP_CONSTS + RSXF_VPC_PER_REC - 1) / RSXF_VPC_PER_REC) * RSXF_REG_HEADER +
                         RSXF_VPC_ENTRY * RSXF_VP_CONSTS + 2 * (RSXF_REG_HEADER + RSXF_UNI_ENTRY * RSXF_UNI_MAX) +
                         RSXF_REG_HEADER + RSXF_FPC_ENTRY * RSXF_FPC_MAX + RSXF_UNIT_DEFS,
       RSXF_STAGING_KB_MIN = 32 };
typedef char rsxf_unit_fits_min_block[(RSXF_DRAWS_HEADER + RSXF_UNIT_WORST <= RSXF_STAGING_KB_MIN * 1024) ? 1 : -1];
enum { RSXF_AVAILABLE = 0, RSXF_NO_AUTO = 1, RSXF_NO_RING = 2, RSXF_NO_DECODER = 3 };
enum { RSXF_K_ARRAY = 1, RSXF_K_INDEX = 2, RSXF_K_CLEAR = 3, RSXF_K_INLINE_ARRAY = 7, RSXF_K_INLINE_INDEX = 8,
       RSXF_K_IMMEDIATE = 9 };
enum { RSXF_DF_HOOKED = 1, RSXF_DF_DECODED = 2, RSXF_DF_MISMATCH = 4, RSXF_DF_STATE_UNKNOWN = 8,
       /* decoder 5: INVALIDATE_ZCULL / CLEAR_ZCULL_SURFACE written since the previous draw (one-shot events) */
       RSXF_DF_ZCULL_INVALIDATED = 16, RSXF_DF_ZCULL_CLEARED = 32 };
enum { RSXF_UNTIMED_OUTSIDE_RANGE = 32, RSXF_UNTIMED_RING_FULL = 33, RSXF_UNTIMED_RING_STATE = 34,
       RSXF_UNTIMED_NOT_HOOKED = 64, RSXF_UNTIMED_NO_WINDOW = 65 };
enum { RSXF_PROFILE_VP = 1, RSXF_PROFILE_FP = 2 };
enum { RSXF_SRC_STREAM = 1, RSXF_SRC_CONTAINER = 2, RSXF_SRC_MEMORY = 3, RSXF_SRC_APP = 4 };
/* NOT_REVALIDATED: the identity comes from a bind before the capture was armed (the container may have been
 * rewritten in place since); a bind while armed always hashes every instruction byte. */
enum { RSXF_PF_UNTERMINATED = 1, RSXF_PF_UNREADABLE = 2, RSXF_PF_NOT_REVALIDATED = 4 };
enum { RSXF_OUT_DONE = 1, RSXF_OUT_TRUNCATED = 2, RSXF_OUT_ABANDONED = 3 };
enum { RSXF_R_NONE = 0, RSXF_R_STAGING = 1, RSXF_R_PROGRAMS = 2, RSXF_R_LABELS = 3, RSXF_R_SHUTDOWN = 4,
       RSXF_R_RESET = 5, RSXF_R_DECODER = 6, RSXF_R_CANCELLED = 7 };
#define RSXF_NO_ORDINAL 0xffffffffu

typedef struct rsxf_wire_config {
    uint32_t availability, block_bytes, blocks, programs, labels, label_bytes, fp_bytes_cap;
} rsxf_wire_config;
typedef struct rsxf_wire_begin {
    uint32_t id, request, owner, window, vp_start, fp_address, known;
    uint16_t source, flags;
    uint64_t frame, tb;
    const char *name;
    uint32_t name_len;
} rsxf_wire_begin;
typedef struct rsxf_wire_draw {
    uint32_t ordinal, untimed, mode, first, count, index_offset, clear_mask, tag, label, vp, fp, fp_address, vp_start,
             words, zpass;
    uint16_t kind, flags;
    uint8_t index_type, index_location;
    uint64_t op_id, caller, tb;
} rsxf_wire_draw;
typedef struct rsxf_wire_zcull {
    uint32_t ordinal; /* the next draw ordinal: the region applies from that draw on */
    uint32_t index, flags, offset, width, height, cull_start, z_format, aa_format, dir, format, s_func, s_ref, s_mask;
} rsxf_wire_zcull;
typedef struct rsxf_wire_program {
    uint32_t index, instructions, ucode_bytes, const_blocks, address, first_draw;
    uint8_t profile, source;
    uint16_t flags;
    uint64_t hash;
} rsxf_wire_program;
typedef struct rsxf_wire_end {
    uint32_t id, blocks, blocks_dropped, draws_seen, draws_recorded, hooked, decoded_only, mismatches, truncated_at,
             programs, labels, labels_refused, programs_refused, reject_header, reject_reason, lost_events, untimed,
             baseline_registers, state_records, vpconst_records, fpconst_records, uniform_records, ucode_records;
    uint16_t outcome, reason;
    uint64_t bytes_streamed, bytes_dropped, tb_end, decoder_words;
} rsxf_wire_end;

/* Return the bytes written, or 0 (nothing written) when capacity is short or an argument is out of range. */
size_t ps3dk_prof__rsxf_put_config(uint8_t *out, size_t cap, const rsxf_wire_config *c);
size_t ps3dk_prof__rsxf_put_begin(uint8_t *out, size_t cap, const rsxf_wire_begin *b);
size_t ps3dk_prof__rsxf_put_draw(uint8_t *out, size_t cap, const rsxf_wire_draw *d);
size_t ps3dk_prof__rsxf_put_program(uint8_t *out, size_t cap, const rsxf_wire_program *p);
size_t ps3dk_prof__rsxf_put_zcull(uint8_t *out, size_t cap, const rsxf_wire_zcull *z);
size_t ps3dk_prof__rsxf_put_text(uint8_t *out, size_t cap, uint16_t type, uint32_t index, const char *s, uint32_t n);
size_t ps3dk_prof__rsxf_put_draws_header(uint8_t *out, size_t cap, uint32_t id, uint32_t seq, uint32_t records,
                                         uint32_t record_bytes);
size_t ps3dk_prof__rsxf_put_end(uint8_t *out, size_t cap, const rsxf_wire_end *e);
size_t ps3dk_prof__rsxf_text_bytes(uint32_t n); /* a LABEL/NAME record's length for n text bytes */
/* A BASELINE (ordinal 0, flags 0) or STATE record of n (register index, value) pairs, indices strictly increasing
 * and below RSXF_REGS; n 1..RSXF_REG_PAIRS_MAX, except a STATE record with RSXF_STATE_RESET may have none. */
size_t ps3dk_prof__rsxf_put_regs(uint8_t *out, size_t cap, uint16_t type, uint32_t ordinal, uint32_t flags,
                                 const uint16_t *index, const uint32_t *value, uint32_t n);
/* Appends at most `n` bytes of `src` (stopping at a NUL) to dst[at..cap) as valid UTF-8: every invalid byte becomes
 * '?', and a sequence that does not fit whole is left out (the text is cut on a character boundary). Returns the new
 * length. A label path and a capture name go through it, so every LABEL and NAME text is valid UTF-8. */
uint32_t ps3dk_prof__rsxf_utf8_append(char *dst, uint32_t at, uint32_t cap, const char *src, size_t n);

/* ---- canonical program hash (rsxf_wire.c; format 12.6) ------------------------------------------------------------ */
typedef struct rsxf_hash {
    uint64_t hash;
    uint32_t instructions, bytes, const_blocks, unterminated;
} rsxf_hash;
/* FP ucode as it lies in memory (halfword-swapped big-endian words), at most `bytes`. */
void ps3dk_prof__rsxf_fp_hash(const uint8_t *ucode, size_t bytes, rsxf_hash *out);
/* VP instructions as 32-bit values (4 per instruction), from the start instruction, at most `instructions`. */
void ps3dk_prof__rsxf_vp_hash(const uint32_t *words, uint32_t instructions, rsxf_hash *out);
uint64_t ps3dk_prof__rsxf_fnv(uint64_t h, const void *bytes, size_t n);
#define RSXF_FNV_OFFSET 0xcbf29ce484222325ull

/* ---- command-stream decoder (rsxf_decode.c): subchannel 0 only, owner words only ------------------------------- */
/* The 3D register file: methods 0x0000..0x1ffc as 2048 words. Data ports (VP instruction and constant upload, inline
 * vertex and index data, draw triggers, the clear trigger, the object bind) are interpreted, not stored. */
enum { RSXF_VP_SLOTS = 512, RSXF_REGS = 2048, RSXF_REG_WORDS = RSXF_REGS / 32 };
/* 1 when the method (byte address 0..0x1ffc) is shadowed as a register. */
int ps3dk_prof__rsxf_shadowed(uint32_t method);
typedef struct rsxf_draw_event {
    uint32_t kind, mode, first, count, index_offset, index_type, index_location, clear_mask, words;
    uint32_t zcull_events; /* RSXF_DF_ZCULL_* written since the previous draw or clear */
} rsxf_draw_event;
typedef struct rsxf_decoder {
    uint32_t vp[RSXF_VP_SLOTS][4];
    uint8_t vp_known[RSXF_VP_SLOTS];
    uint32_t vp_upload, vp_word, vp_start, fp_address, index_offset, index_dma;
    uint32_t known;            /* bit 0 vp_start, bit 1 fp_address, bit 2 index registers */
    /* the register shadow: the last value written, KNOWN (written since the last reset) and DIRTY (written since the
     * runtime last cleared it) bits per register; regs_cleared counts resets (every KNOWN bit cleared) */
    uint32_t reg[RSXF_REGS];
    uint32_t reg_known[RSXF_REG_WORDS], reg_dirty[RSXF_REG_WORDS];
    uint32_t regs_cleared;
    /* VP constants (VP_UPLOAD_CONST_ID + data port): values, a KNOWN component mask per slot, DIRTY bits */
    uint32_t vpc[RSXF_VP_CONSTS][4];
    uint8_t vpc_known[RSXF_VP_CONSTS];
    uint32_t vpc_dirty[(RSXF_VP_CONSTS + 31) / 32];
    uint32_t vpc_id;
    /* inline transfers into local memory (subchannel 3 surfaces-2D destination, subchannel 5 image-from-CPU data, the
     * whole packet): the last word written at each address, open addressing; a full map counts imap_overflow, a reset
     * clears it. An entry marked unk is a word whose last inline value can no longer be trusted (a different program
     * placed over it); taint holds local ranges an M2MF copy (subchannel 1) wrote, taint_all when more did than fit.
     * main_written: an inline write or copy went to main memory (not mapped). */
    uint32_t s3_dma, s3_offset, s5_x, s5_w;
    uint32_t imap_addr[RSXF_INLINE_MAP], imap_val[RSXF_INLINE_MAP]; /* addr + 1; 0 empty */
    uint8_t imap_unk[RSXF_INLINE_MAP];
    uint32_t imap_used, imap_overflow, imap_cleared;
    uint32_t m2mf_dma_out, m2mf_out, m2mf_pitch_out, m2mf_len, m2mf_count;
    /* RSXF_PORT_* bits: each destination cursor of a data port observed since attach or the last reset. Data words
     * whose destination is not fully known are dropped (port_dropped), never applied at a stale slot or address. */
    uint32_t ports_ok, port_dropped;
    uint32_t taint_lo[RSXF_TAINT_RANGES], taint_hi[RSXF_TAINT_RANGES], ntaint, taint_all, main_written, m2mf_copies;
    uint64_t vp_generation;    /* +1 per uploaded instruction */
    uint64_t words;            /* decoder cost (words examined) */
    uint32_t bad_header, bad_reason, resets; /* the first rejection (AUTO_RING_REJECT_* numbering) */
    /* ZPASS counting (QUERY_ENABLE is in the register shadow): zq_open an application QUERY_RESET of type 1 not yet
     * followed by its QUERY_GET of type 1 (a report outside [own_lo, own_hi), the profiler's own report range);
     * zq_touched any application ZPASS method seen since attach; zq_blind a decoder reset since attach (unseen words
     * may have touched it). zc_events: RSXF_DF_ZCULL_* since the last draw. */
    uint32_t own_lo, own_hi, zq_open, zq_touched, zq_blind, zc_events;
    /* The firmware-reserved prefix (AUTO's init record): io the EA of IO offset 0, start the context start's IO
     * offset. A CALL to a target below start (the firmware's per-flip default-state routine) is decoded there, one
     * level, up to its RET, so its state writes are seen instead of a reset; NULL io: every CALL resets. */
    const uint32_t *io;
    /* routine_triggers: draw and clear triggers inside routines (the firmware's default-state routine writes a clear
     * with mask 0): state only, never a draw of the application, never recorded */
    uint32_t start, routines, in_routine, routine_triggers;
    /* the open BEGIN_END group */
    uint32_t in_group, group_batches;
    rsxf_draw_event group;
    /* each completed draw or clear goes to sink at once (the registers are then those of that draw); without a sink
     * it is kept in events, and one that does not fit is counted in lost_events */
    void (*sink)(void *user, const rsxf_draw_event *e);
    void *sink_user;
    rsxf_draw_event events[16];
    uint32_t n_events, lost_events;
} rsxf_decoder;
void ps3dk_prof__rsxf_decode_init(rsxf_decoder *d);
/* Forgets every known register (unseen words may have changed them) and every data-port destination cursor (an unseen
 * word may have moved it); counts a reset; keeps the first rejection. */
void ps3dk_prof__rsxf_decode_unknown(rsxf_decoder *d, uint32_t header, uint32_t reason);
/* Decodes n complete owner words; completed draws and clears are appended to events. Returns 0 after a rejection
 * (the registers are then unknown; decoding resumes at the next word given). */
int ps3dk_prof__rsxf_decode(rsxf_decoder *d, const uint32_t *p, size_t n);
/* The local-memory word at addr: RSXF_FPC_INLINE with *v (the last inline transfer the decoder saw write it),
 * RSXF_FPC_CONTAINER (no write to it seen: the memory's own value stands), or RSXF_FPC_UNKNOWN (see the provenance
 * enum). */
uint32_t ps3dk_prof__rsxf_inline_word(const rsxf_decoder *d, uint32_t addr, uint32_t *v);
/* A different program was placed at [lo, hi) of local memory: the inline words remembered there become UNKNOWN. */
void ps3dk_prof__rsxf_inline_forget(rsxf_decoder *d, uint32_t lo, uint32_t hi);
/* The application's ZPASS counting state now (RSXF_ZP_COUNTING, _OFF, _PAUSED, _UNTOUCHED or _UNKNOWN). */
uint32_t ps3dk_prof__rsxf_zpass_state(const rsxf_decoder *d);

/* ---- runtime hooks (rsxf_runtime.c), called from auto_runtime.c -------------------------------------------------- */
struct _gcmCtxData;
struct prof;
/* 1 when the executable selected the capture (a cached weak-symbol test: one load). */
int rsxf_selected(void);
/* At AUTO attach on the owner: the owner context and whether the report ring is attached. Without the ring the
 * capture stays dormant (ps3dk_prof__rsxf_active NULL: no hook, no decoder, no state allocated). */
void rsxf_attach(void *ctx, int ringed);
/* Owner hooks: catch the decoder up to ctx->current (prewrap first resets the cursor to begin after scanning). */
void rsxf_scan(void *ctx);
void rsxf_wrap(void *ctx);
void rsxf_reset(void *ctx);
/* A covered draw/clear on the owner context: begin before AUTO's admission, admitted after it (op id or 0 and the
 * exclusion column AUTO counted), end after the application's words and AUTO's end marker. */
void rsxf_hook_begin(void *ctx, uint32_t kind, uint32_t mode, uint32_t first, uint32_t count, uint32_t offset,
                     uint32_t index_type, uint32_t location, uint32_t clear_mask, uint64_t caller, uint32_t tag);
void rsxf_hook_admitted(void *ctx, uint64_t op_id);
void rsxf_hook_end(void *ctx);
/* AUTO counted a covered visit in exclusion column c (2..11), outside a timed range (32) or ring full/state (33/34). */
void rsxf_note_column(uint32_t c);
/* The owner's frame boundary, after AUTO's own frame step: arms or closes a capture. */
void rsxf_frame(uint64_t frame, uint64_t tb, uint32_t owner, int window_open, uint32_t window_id);
/* 1 while a capture is armed: AUTO times every covered draw (overrides PS3DK_PROF_AUTO_TIMED_RANGES_ONLY). */
int rsxf_forced(void);
/* 1 while an armed capture records ISOLATED draws: AUTO writes WAIT_FOR_IDLE before each of their two markers. */
int rsxf_isolated(void);
/* At a covered draw's admission (after rsxf_hook_begin caught the decoder up): 1 when an armed PIXELS capture forces
 * ZPASS counting on around this draw (QUERY_ENABLE 1 before its begin marker, 0 after its end marker). */
int rsxf_pixel_force(void *ctx);
/* The words from the decoder's cursor to ctx->current are the profiler's own (the forced QUERY_ENABLE words): skip
 * them, so the register shadow keeps the application's value. Call right after writing them, after a scan. */
void rsxf_skip(void *ctx);
/* The forced QUERY_ENABLE 1 of the covered draw was written (its record then carries RSXF_ZP_FORCED). */
#define rsxf_pixel_forced ps3dk_prof__rsxf_pixel_forced
void rsxf_pixel_forced(void *ctx);
/* At AUTO attach: the profiler's report range [first, first + count); the application's ZPASS reports are the rest. */
void rsxf_profiler_reports(uint32_t first, uint32_t count);
/* At AUTO attach: the firmware-reserved prefix (see rsxf_decoder.io); NULL when AUTO has no init record. */
#define rsxf_firmware_prefix ps3dk_prof__rsxf_firmware_prefix
void rsxf_firmware_prefix(const uint32_t *io, uint32_t start);
/* The drainer, after AUTO's own drain step. emit(type, payload, bytes) returns 0 when the sink took it. */
typedef int (*rsxf_emit_fn)(void *user, uint32_t type, const uint8_t *data, size_t bytes);
void rsxf_drain(rsxf_emit_fn emit, void *user, int auto_available, int seal);

/* Program bind hooks (SDK headers in a PS3TC_PROF_AUTO unit; symbols in auto_runtime.c, forwarded when selected). */
void ps3dk_prof__rsxf_fp_bind(struct _gcmCtxData *ctx, const void *ucode, uint32_t bytes, uint32_t offset);
/* A program container (CgBinaryProgram) bound on the owner: kind 1 VP, 2 FP (after its fp_bind). Its uniform names
 * and resources, and an FP's constant blocks with their container values, are read here (bounded), never later. */
void ps3dk_prof__rsxf_container_bind(struct _gcmCtxData *ctx, const void *container, uint32_t kind);
/* A zcull region bound (bound 1, after a successful cellGcmBindZcull) or unbound (0, cellGcmUnbindZcull) on the owner
 * thread; binds from other threads mark the region table unknown. */
void ps3dk_prof__rsxf_zcull_bind(uint32_t bound, uint32_t index, uint32_t offset, uint32_t width, uint32_t height,
                                 uint32_t cull_start, uint32_t z_format, uint32_t aa_format, uint32_t dir,
                                 uint32_t format, uint32_t s_func, uint32_t s_ref, uint32_t s_mask);

/* ---- live link (protocol v3), read by live_api.c through a weak symbol ------------------------------------------- */
typedef struct rsxf_status {
    uint32_t id, request, state, reason, draws;   /* state: 0 none, 1 ARMED, 2 CLOSED, 3 ENCODED, 4 ABANDONED, 5 REQUESTED */
    uint64_t frame, bytes;
} rsxf_status;
typedef struct rsxf_live_ops {
    /* 0 with *id, or a PS3DK_PROF_* refusal; flags: RSXF_REQ_* (ISOLATED, FP_BYTES, BOTH; not ISOLATED with BOTH) */
    int (*request)(uint32_t request_id, uint32_t flags, const char *name, uint32_t name_len, uint32_t *id);
    /* the latest LIVE request's status (id 0: none yet); an app request never replaces it */
    void (*status)(rsxf_status *out);
    int (*available)(void); /* 1: requests can be served (selected, AUTO attached with the ring) */
} rsxf_live_ops;
const rsxf_live_ops *ps3dk_prof__rsxf_live_ops(void);
#endif
