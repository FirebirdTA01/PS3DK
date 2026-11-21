/* gdbstub.h - libgdbstub public C99 API (normative contract 1).
 *
 * C99 host library, one process, big-endian PPU register description,
 * normal ACK mode, all-stop. No signals, non-stop, multiprocess, qXfer,
 * no-ack negotiation, Ctrl-C, hardware, or TCP in this layer.
 *
 * This file is the API contract that the real implementation and any
 * stub/replacement must satisfy. It contains no implementation narrative,
 * no version string, no per-card state.
 */
#ifndef PS3_GDBSTUB_H
#define PS3_GDBSTUB_H

#include <stddef.h>
#include <stdint.h>

/* Pinned register layout constants (71 registers, 556 bytes, 1112 hex;
 * see register-layout.json; layout table lives in gdbstub_layout.c). */
#define GDBSTUB_REG_COUNT 71u
#define GDBSTUB_REG_TOTAL_BYTES 556u
#define GDBSTUB_REG_TOTAL_HEX 1112u

/* Transport / operation status, returned separately from any count
 * (contract transport rules). */
typedef enum {
  GDBSTUB_OK = 0,      /* completed; count follows the contract */
  GDBSTUB_AGAIN,       /* no progress; count must be 0; not EOF; retryable */
  GDBSTUB_EOF,         /* read channel closed; count 0 */
  GDBSTUB_ERROR,       /* I/O or io_contract failure; count 0 */
  GDBSTUB_LIMIT,       /* would exceed a response limit; nothing emitted */
  GDBSTUB_FRAME,       /* candidate frame malformed; not accepted */
  GDBSTUB_BAD_STATE,   /* API misuse for this state; no output/change */
  GDBSTUB_NIMPL        /* not implemented: entry point unimplemented */
} gdbstub_status;

/* Session states visible to the fixture (contract "Session and transport
 * state"). GDBSTUB_STATE_NIMPL is a sentinel OUTSIDE the fixture state
 * set, returned by entry-point stub implementations, and used to keep
 * step-state assertions discriminating against unimplemented cores. */
typedef enum {
  GDBSTUB_READY = 0,
  GDBSTUB_TX,
  GDBSTUB_WAIT_ACK,
  GDBSTUB_RUNNING,
  GDBSTUB_DETACH_ACK,
  GDBSTUB_CLOSED,
  GDBSTUB_STATE_NIMPL    /* sentinel: outside the fixture state set */
} gdbstub_state;

/* Terminal reasons (session_reason strings once CLOSED). */
#define GDBSTUB_REASON_DETACHED     "detached"
#define GDBSTUB_REASON_KILL_OK      "kill_ok"
#define GDBSTUB_REASON_KILL_ERROR   "kill_error"
#define GDBSTUB_REASON_KILL_ABSENT  "kill_absent"
#define GDBSTUB_REASON_EOF          "eof"
#define GDBSTUB_REASON_IO           "io"
#define GDBSTUB_REASON_TIMEOUT      "timeout"
#define GDBSTUB_REASON_PROTOCOL     "protocol"
#define GDBSTUB_REASON_IO_CONTRACT  "io_contract"

/* Thread selector: any (wire 0), all (wire -1), or a positive uint64 ID. */
typedef struct {
  int kind;        /* 0 = any, 1 = all, 2 = id */
  uint64_t id;     /* valid when kind == 2 */
} gdbstub_thread;
#define GDBSTUB_THREAD_ANY 0
#define GDBSTUB_THREAD_ALL 1
#define GDBSTUB_THREAD_ID 2

/* One vCont action (c or s in this increment). */
typedef struct {
  char action;              /* 'c' or 's' */
  gdbstub_thread thread;    /* omitted selector = all */
} gdbstub_action;

/* --- Backend operations (typed values in/out, not frames) --------------
 * A NULL function pointer in gdbstub_backend means the callback is
 * genuinely absent for the session; the core must not invent behavior.
 *
 * The read-family ops return the number of bytes actually produced via
 * *out_len so callers can detect short reads and width mismatches.
 * Read callbacks must never write beyond capacity (read_mem: length), even
 * when reporting an invalid larger result length. The core rejects an
 * out-of-capacity reported length before reading those bytes.
 * The write-family ops take an explicit length.
 *
 * All 15 operations:
 *   stop_reason  read_regs  write_regs  read_reg  write_reg
 *   read_mem     write_mem  threads    current_thread  select_thread
 *   breakpoint   resume     vcont      detach  kill
 * ------------------------------------------------------------- */
typedef gdbstub_status (*gdbstub_op_stop_reason)(
  void *ctx, uint8_t *signal, uint64_t *thread, int *has_thread);
typedef gdbstub_status (*gdbstub_op_read_regs)(
  void *ctx, uint8_t *out, size_t out_cap, size_t *out_len);
typedef gdbstub_status (*gdbstub_op_write_regs)(
  void *ctx, const uint8_t *in, size_t in_len);
typedef gdbstub_status (*gdbstub_op_read_reg)(
  void *ctx, unsigned reg, uint8_t *out, size_t out_cap, size_t *out_len);
typedef gdbstub_status (*gdbstub_op_write_reg)(
  void *ctx, unsigned reg, const uint8_t *in, size_t in_len);
typedef gdbstub_status (*gdbstub_op_read_mem)(
  void *ctx, uint64_t addr, size_t length, uint8_t *out, size_t *out_len);
typedef gdbstub_status (*gdbstub_op_write_mem)(
  void *ctx, uint64_t addr, const uint8_t *in, size_t in_len);
typedef gdbstub_status (*gdbstub_op_threads)(
  void *ctx, int first, uint64_t *ids, size_t max_ids, size_t *n_ids);
typedef gdbstub_status (*gdbstub_op_current_thread)(void *ctx, uint64_t *id);
typedef gdbstub_status (*gdbstub_op_select_thread)(
  void *ctx, char which, gdbstub_thread thread);
typedef gdbstub_status (*gdbstub_op_breakpoint)(
  void *ctx, int insert, int type, uint64_t addr, uint64_t kind);
typedef gdbstub_status (*gdbstub_op_resume)(
  void *ctx, char action, uint64_t addr, int has_addr);
typedef gdbstub_status (*gdbstub_op_vcont)(
  void *ctx, const gdbstub_action *actions, size_t n_actions);
typedef gdbstub_status (*gdbstub_op_detach)(void *ctx);
typedef gdbstub_status (*gdbstub_op_kill)(void *ctx);

typedef struct {
  void *ctx;
  gdbstub_op_stop_reason stop_reason;
  gdbstub_op_read_regs read_regs;
  gdbstub_op_write_regs write_regs;
  gdbstub_op_read_reg read_reg;
  gdbstub_op_write_reg write_reg;
  gdbstub_op_read_mem read_mem;
  gdbstub_op_write_mem write_mem;
  gdbstub_op_threads threads;
  gdbstub_op_current_thread current_thread;
  gdbstub_op_select_thread select_thread;
  gdbstub_op_breakpoint breakpoint;
  gdbstub_op_resume resume;
  gdbstub_op_vcont vcont;
  gdbstub_op_detach detach;
  gdbstub_op_kill kill;
} gdbstub_backend;

/* --- Transport (status/count, authoritative for AGAIN and EOF) --------- */
typedef struct {
  gdbstub_status status;   /* OK, AGAIN, EOF, ERROR */
  size_t count;
} gdbstub_io_result;

typedef struct {
  void *ctx;
  gdbstub_io_result (*read)(void *ctx, uint8_t *out, size_t cap);
  gdbstub_io_result (*write)(void *ctx, const uint8_t *data, size_t len);
} gdbstub_transport;

/* --- Configurable project limits (contract defaults) ------------------- */
typedef struct {
  size_t wire;          /* input wire payload bytes, positive */
  size_t decoded;       /* decoded input bytes, positive */
  size_t response_wire; /* max response framed bytes, >= 8 */
  size_t max_actions;   /* max vCont actions, default 16 */
  uint32_t deadline_ms; /* operation deadline, default 1000 */
} gdbstub_config;

/* --- Session -----------------------------------------------------------
 * The session is a single opaque handle. The driver (test adapter or
 * real integration) feeds input via the transport read function, drives
 * the core with session_step, and inspects state / reason / last status.
 *
 * Event kinds are core-dispatchable triggers ONLY. AGAIN and EOF are not
 * events; they travel through the transport read result. */
typedef enum {
  GDBSTUB_EVT_PUMP = 0,   /* pump pending I/O + state (retry partial output) */
  GDBSTUB_EVT_STOP        /* backend delivered a stop event */
} gdbstub_event_kind;

typedef struct {
  gdbstub_event_kind kind;
  uint8_t signal;           /* valid when kind == GDBSTUB_EVT_STOP */
  uint64_t thread;          /* valid when has_thread */
  int has_thread;           /* 1 when thread is a positive ID */
} gdbstub_event;

typedef struct gdbstub_session gdbstub_session;

/* Session lifecycle and introspection entry points (6 total for the
 * session handle):
 *   gdbstub_session_create,   gdbstub_session_free,
 *   gdbstub_session_step,     gdbstub_session_state,
 *   gdbstub_session_reason,   gdbstub_session_last_status
 */
gdbstub_session *gdbstub_session_create(const gdbstub_config *cfg,
                                        const gdbstub_backend *backend,
                                        const gdbstub_transport *transport);
void gdbstub_session_free(gdbstub_session *s);

/* Drive one event to quiescence INSIDE this call: process available input,
 * ACK completion, dispatch and output until no further progress is possible
 * without new input/event or transport readiness. Do not stop after just one
 * frame when more input/progress is available. Read AGAIN ends the pump when
 * no pending output can progress; output AGAIN yields immediately. Terminal
 * closure also ends the call. The caller does not loop on an unspecified
 * progress result. STOP is supplied only once; BAD_STATE changes nothing.
 * now_ms is the caller-supplied monotonic clock, used
 * for deadline accounting on every pump and stop; the core decides expiry
 * from this value, never a pre-decided expiry event. It does not read time. */
gdbstub_status gdbstub_session_step(gdbstub_session *s,
                                    const gdbstub_event *ev,
                                    uint64_t now_ms);
gdbstub_state gdbstub_session_state(const gdbstub_session *s);
const char *gdbstub_session_reason(const gdbstub_session *s);
gdbstub_status gdbstub_session_last_status(const gdbstub_session *s);

/* --- Framing codec (independent entry points) -------------------------- */
/* Encode: escape reserved wire bytes 23/24/7D/2A with 7D + XOR 20, no
 * RLE on output, uppercase hex checksum digits. GDBSTUB_LIMIT with
 * *n == 0 if the result would exceed cap. */
gdbstub_status gdbstub_codec_encode(const uint8_t *payload, size_t len,
                                    uint8_t *out, size_t cap, size_t *n);
/* Decode: accept escaped bytes and the assigned input-side RLE extension
 * (a raw '*' repeats the preceding decoded byte C-29 MORE times, where
 * count must be printable 32..126 excluding # and $, and a predecessor
 * must exist; an escaped literal '*' stays literal). GDBSTUB_FRAME with
 * *n == 0 on a malformed frame; GDBSTUB_LIMIT with *n == 0 if the
 * decoded length would exceed cap. */
gdbstub_status gdbstub_codec_decode(const uint8_t *frame, size_t len,
                                    uint8_t *out, size_t cap, size_t *n);

/* --- Register layout (pinned data, see gdbstub_layout.c) --------------- */
unsigned gdbstub_reg_count(void);
unsigned gdbstub_reg_bytes(unsigned reg);
unsigned gdbstub_reg_total_bytes(void);
unsigned gdbstub_reg_total_hex(void);

#endif /* PS3_GDBSTUB_H */
