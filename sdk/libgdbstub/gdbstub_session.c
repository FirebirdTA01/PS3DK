/* gdbstub_session.c - remote serial protocol session layer (contract 1).
 *
 * C99 (compiled -std=c99 -pedantic). Implements the six session entry
 * points declared in gdbstub.h on top of the framing codec
 * (gdbstub_codec.c) and the pinned register layout (gdbstub_layout.c).
 * Normal ACK mode, all-stop, one serialized request at a time.
 *
 * Storage: every buffer is allocated once in gdbstub_session_create and is
 * sized from the configured limits (wire W, decoded D, response R, maximum
 * vCont actions); nothing grows afterwards.
 *
 * Input: bytes read from the transport are buffered and fed one at a time
 * to a frame recognizer. Outside a frame, READY ignores everything except
 * '$'; WAIT_ACK and DETACH_ACK also take '+' (reply acknowledged) and '-'
 * (retransmit the retained reply). Inside a frame the raw body (at most W
 * bytes) is collected up to '#', then two checksum characters; the complete
 * frame is verified and decoded (cap D) by gdbstub_codec_decode. A raw '$'
 * always starts a new candidate frame: an unfinished candidate it
 * interrupts is rejected first. A rejected frame (bad checksum or digits,
 * malformed escape/RLE, W or D overflow) gets one '-' and is not
 * dispatched; on W overflow the rest of that frame is skipped.
 *
 * Output: a pending single-byte ACK/NACK and a retained framed reply with a
 * write offset. ACK and reply are separate writes; a partial write resumes
 * at the first unsent byte. While any output byte is pending the visible
 * state is TX and no input is consumed or dispatched. A NACK from the peer
 * resends the whole retained reply.
 *
 * Request flow (READY): a valid frame queues '+'; once the '+' is fully
 * written the request is parsed, validated and dispatched (at most one
 * backend call), and the reply is built completely, framed into the
 * retained buffer and written. Destination: WAIT_ACK (DETACH_ACK after a
 * successful D), RUNNING after an accepted resume (no reply), CLOSED for k.
 * In WAIT_ACK, DETACH_ACK and RUNNING an exact duplicate of the current
 * request (same transmitted body bytes) is answered with '+' plus the
 * retained reply ('+' only while RUNNING) and never dispatched again; any
 * other valid request closes the session with reason "protocol".
 *
 * Deadline: one fixed deadline per request. It starts at the first '$' of
 * a candidate frame seen in READY and covers the frame, the ACK, the
 * operation, reply output, retransmissions, RUNNING and the reply ACK. It
 * is cleared when the request completes (reply ACK received) or when the
 * candidate frame is rejected in READY. The caller supplies now_ms; the
 * deadline has expired when now_ms - start > deadline_ms. Expiry is checked
 * once at the start of every step and closes with reason "timeout".
 *
 * Step result: GDBSTUB_OK when the call reached quiescence (input
 * exhausted, or the session closed during the call; the reason tells why),
 * GDBSTUB_AGAIN when it yielded with output still pending (transport write
 * AGAIN) or after the per-call pump budget, GDBSTUB_BAD_STATE for misuse
 * (NULL arguments, unknown event kind, any step after CLOSED, a STOP event
 * unless the visible state is RUNNING, a STOP with has_thread set and
 * thread 0); BAD_STATE changes nothing. last_status returns the result of
 * the most recent step that was not rejected with BAD_STATE.
 *
 * Every loop is bounded by a buffer length, a 16-digit hex width, or the
 * per-call pump budget below; the core never reads a clock.
 */
#include "gdbstub.h"
#include <stdlib.h>
#include <string.h>

#define SESSION_IN_CAP 512u
/* Upper bound on pump iterations per step. Each iteration writes, reads or
 * consumes at least one byte, or dispatches one request. */
#define SESSION_PUMP_BUDGET 65536u

/* Frame recognizer states. */
enum {
  FR_IDLE = 0,
  FR_BODY,
  FR_CSUM1,
  FR_CSUM2,
  FR_SKIP_BODY,
  FR_SKIP_CSUM1,
  FR_SKIP_CSUM2
};

/* Dispatch results. */
typedef enum {
  RES_EMPTY = 0,
  RES_E01,
  RES_E05,
  RES_OK,
  RES_PAYLOAD,
  RES_RUNNING,
  RES_DETACHED,
  RES_CLOSED
} cmd_result;

/* Transport progress results. */
enum { IO_PROGRESS = 0, IO_BLOCKED, IO_IDLE };

struct gdbstub_session {
  gdbstub_config cfg;
  gdbstub_backend be;
  gdbstub_transport tr;

  gdbstub_state base;   /* READY, WAIT_ACK, RUNNING or DETACH_ACK */
  int closed;
  const char *reason;
  gdbstub_status last;
  uint64_t now;

  uint8_t in[SESSION_IN_CAP];
  size_t in_len, in_pos;

  int fstate;
  uint8_t *frame;       /* '$' body '#' c c, at most W + 4 bytes */
  size_t flen;

  uint8_t *req;         /* transmitted body of the current request (W) */
  size_t req_len;
  int have_req;

  uint8_t *dec;         /* decoded payload (D) */
  size_t dec_len;
  int dispatch_pending;

  uint8_t ack;
  int ack_pending;

  uint8_t *reply;       /* retained framed reply (R) */
  size_t reply_len, reply_off;
  int reply_pending;
  int have_reply;

  uint8_t *payload;     /* unframed reply builder storage (R) */
  uint8_t *data;        /* binary scratch for registers and memory */
  size_t data_cap;
  uint64_t *ids;
  size_t ids_cap;
  gdbstub_action *actions;
  size_t actions_cap;

  int dl_active;
  uint64_t dl_start;
};

/* ---- small helpers ---------------------------------------------------- */

static int hexval(uint8_t c)
{
  if (c >= '0' && c <= '9')
    return (int)(c - '0');
  if (c >= 'A' && c <= 'F')
    return (int)(c - 'A' + 10);
  if (c >= 'a' && c <= 'f')
    return (int)(c - 'a' + 10);
  return -1;
}

static char hexdig(unsigned v)
{
  return (char)(v < 10u ? '0' + (int)v : 'A' + (int)v - 10);
}

/* Nonempty ASCII hex, no sign or prefix, uint64 overflow detected while
 * accumulating. Returns 1 on success. */
static int parse_u64(const uint8_t *p, size_t n, uint64_t *out)
{
  uint64_t v = 0u;
  size_t i;
  if (n == 0u)
    return 0;
  for (i = 0u; i < n; i++) {
    int d = hexval(p[i]);
    if (d < 0)
      return 0;
    if (v > (UINT64_MAX - (uint64_t)d) / 16u)
      return 0;
    v = v * 16u + (uint64_t)d;
  }
  *out = v;
  return 1;
}

/* Exactly want bytes of hex (2 * want characters, either case). */
static int parse_hex_data(const uint8_t *p, size_t n, uint8_t *out,
                          size_t want)
{
  size_t i;
  if (n % 2u != 0u || n / 2u != want)
    return 0;
  for (i = 0u; i < want; i++) {
    int hi = hexval(p[2u * i]);
    int lo = hexval(p[2u * i + 1u]);
    if (hi < 0 || lo < 0)
      return 0;
    out[i] = (uint8_t)(hi * 16 + lo);
  }
  return 1;
}

/* Thread selector: "-1" = all, 0 = any, positive hex = id. */
static int parse_selector(const uint8_t *p, size_t n, gdbstub_thread *t)
{
  uint64_t v;
  if (n == 2u && p[0] == '-' && p[1] == '1') {
    t->kind = GDBSTUB_THREAD_ALL;
    t->id = 0u;
    return 1;
  }
  if (!parse_u64(p, n, &v))
    return 0;
  if (v == 0u) {
    t->kind = GDBSTUB_THREAD_ANY;
    t->id = 0u;
  } else {
    t->kind = GDBSTUB_THREAD_ID;
    t->id = v;
  }
  return 1;
}

/* Index of the first c in p[from, n), or n. */
static size_t find_byte(const uint8_t *p, size_t from, size_t n, uint8_t c)
{
  size_t i;
  for (i = from; i < n; i++)
    if (p[i] == c)
      return i;
  return n;
}

static int is_lit(const uint8_t *p, size_t n, const char *lit)
{
  size_t k = strlen(lit);
  return n == k && memcmp(p, lit, k) == 0;
}

static int has_prefix(const uint8_t *p, size_t n, const char *lit)
{
  size_t k = strlen(lit);
  return n >= k && memcmp(p, lit, k) == 0;
}

/* ---- reply builder ---------------------------------------------------- */

typedef struct {
  uint8_t *p;
  size_t cap;
  size_t n;
  int over;
} outbuf;

static void ob_bytes(outbuf *b, const char *src, size_t n)
{
  if (b->over)
    return;
  if (n > b->cap - b->n) {
    b->over = 1;
    return;
  }
  memcpy(b->p + b->n, src, n);
  b->n += n;
}

static void ob_str(outbuf *b, const char *s)
{
  ob_bytes(b, s, strlen(s));
}

/* Minimal uppercase hex, "0" for zero. */
static void ob_hex_u64(outbuf *b, uint64_t v)
{
  char t[16];
  size_t k = 0u;
  do {
    t[15u - k] = hexdig((unsigned)(v & 15u));
    v >>= 4u;
    k++;
  } while (v != 0u && k < 16u);
  ob_bytes(b, t + 16u - k, k);
}

static void ob_hex_u8(outbuf *b, uint8_t v)
{
  char t[2];
  t[0] = hexdig((unsigned)(v >> 4u));
  t[1] = hexdig((unsigned)(v & 15u));
  ob_bytes(b, t, 2u);
}

static void ob_hex_data(outbuf *b, const uint8_t *d, size_t n)
{
  size_t i;
  for (i = 0u; i < n && !b->over; i++)
    ob_hex_u8(b, d[i]);
}

/* ---- session state helpers ------------------------------------------- */

static void session_close(gdbstub_session *s, const char *reason)
{
  s->closed = 1;
  s->reason = reason;
  s->in_len = s->in_pos = 0u;
  s->fstate = FR_IDLE;
  s->flen = 0u;
  s->have_req = 0;
  s->req_len = 0u;
  s->dec_len = 0u;
  s->dispatch_pending = 0;
  s->ack_pending = 0;
  s->reply_len = s->reply_off = 0u;
  s->reply_pending = 0;
  s->have_reply = 0;
  s->dl_active = 0;
}

static int output_pending(const gdbstub_session *s)
{
  return s->ack_pending || s->reply_pending || s->dispatch_pending;
}

/* Frame the payload into the retained reply. A payload that does not fit
 * the response limit R becomes E05 (R >= 8 always fits "$E05#AA"). */
static void reply_set(gdbstub_session *s, const uint8_t *p, size_t n)
{
  size_t got = 0u;
  if (gdbstub_codec_encode(p, n, s->reply, s->cfg.response_wire, &got) !=
      GDBSTUB_OK) {
    got = 0u;
    if (gdbstub_codec_encode((const uint8_t *)"E05", 3u, s->reply,
                             s->cfg.response_wire, &got) != GDBSTUB_OK)
      got = 0u;
  }
  s->reply_len = got;
  s->reply_off = 0u;
  s->have_reply = got > 0u;
  s->reply_pending = got > 0u;
}

static void reply_str(gdbstub_session *s, const char *str)
{
  reply_set(s, (const uint8_t *)str, strlen(str));
}

/* ---- packet handlers -------------------------------------------------- */

static cmd_result cmd_supported(gdbstub_session *s, outbuf *b)
{
  size_t lim = s->cfg.wire < s->cfg.decoded ? s->cfg.wire : s->cfg.decoded;
  ob_str(b, "PacketSize=");
  ob_hex_u64(b, (uint64_t)lim);
  return RES_PAYLOAD;
}

static cmd_result cmd_stop_reason(gdbstub_session *s, outbuf *b)
{
  uint8_t sig = 0u;
  uint64_t tid = 0u;
  int has = 0;
  if (!s->be.stop_reason)
    return RES_EMPTY;
  if (s->be.stop_reason(s->be.ctx, &sig, &tid, &has) != GDBSTUB_OK)
    return RES_E05;
  if (has && tid == 0u)
    return RES_E05;
  ob_str(b, has ? "T" : "S");
  ob_hex_u8(b, sig);
  if (has) {
    ob_str(b, "thread:");
    ob_hex_u64(b, tid);
    ob_str(b, ";");
  }
  return RES_PAYLOAD;
}

static cmd_result cmd_read_regs(gdbstub_session *s, outbuf *b)
{
  size_t len = 0u;
  if (!s->be.read_regs)
    return RES_EMPTY;
  if (s->be.read_regs(s->be.ctx, s->data, GDBSTUB_REG_TOTAL_BYTES, &len) !=
      GDBSTUB_OK)
    return RES_E05;
  if (len != GDBSTUB_REG_TOTAL_BYTES)
    return RES_E05;
  ob_hex_data(b, s->data, len);
  return RES_PAYLOAD;
}

static cmd_result cmd_write_regs(gdbstub_session *s, const uint8_t *p,
                                 size_t n)
{
  if (!parse_hex_data(p, n, s->data, GDBSTUB_REG_TOTAL_BYTES))
    return RES_E01;
  if (!s->be.write_regs)
    return RES_EMPTY;
  if (s->be.write_regs(s->be.ctx, s->data, GDBSTUB_REG_TOTAL_BYTES) !=
      GDBSTUB_OK)
    return RES_E05;
  return RES_OK;
}

/* Register index field: hex, below the pinned count. */
static int parse_reg(const uint8_t *p, size_t n, unsigned *reg)
{
  uint64_t v;
  if (!parse_u64(p, n, &v) || v >= (uint64_t)gdbstub_reg_count())
    return 0;
  *reg = (unsigned)v;
  return 1;
}

static cmd_result cmd_read_reg(gdbstub_session *s, const uint8_t *p,
                               size_t n, outbuf *b)
{
  unsigned reg;
  size_t width, len = 0u;
  if (!parse_reg(p, n, &reg))
    return RES_E01;
  if (!s->be.read_reg)
    return RES_EMPTY;
  width = gdbstub_reg_bytes(reg);
  if (s->be.read_reg(s->be.ctx, reg, s->data, width, &len) != GDBSTUB_OK)
    return RES_E05;
  if (len != width)
    return RES_E05;
  ob_hex_data(b, s->data, len);
  return RES_PAYLOAD;
}

static cmd_result cmd_write_reg(gdbstub_session *s, const uint8_t *p,
                                size_t n)
{
  unsigned reg;
  size_t eq = find_byte(p, 0u, n, '=');
  size_t width;
  if (eq == n || !parse_reg(p, eq, &reg))
    return RES_E01;
  width = gdbstub_reg_bytes(reg);
  if (!parse_hex_data(p + eq + 1u, n - eq - 1u, s->data, width))
    return RES_E01;
  if (!s->be.write_reg)
    return RES_EMPTY;
  if (s->be.write_reg(s->be.ctx, reg, s->data, width) != GDBSTUB_OK)
    return RES_E05;
  return RES_OK;
}

/* "ADDR,LEN" with addr + len within uint64. */
static int parse_range(const uint8_t *p, size_t n, uint64_t *addr,
                       uint64_t *len)
{
  size_t comma = find_byte(p, 0u, n, ',');
  if (comma == n)
    return 0;
  if (!parse_u64(p, comma, addr))
    return 0;
  if (!parse_u64(p + comma + 1u, n - comma - 1u, len))
    return 0;
  if (*len > UINT64_MAX - *addr)
    return 0;
  return 1;
}

static cmd_result cmd_read_mem(gdbstub_session *s, const uint8_t *p,
                               size_t n, outbuf *b)
{
  uint64_t addr, len;
  size_t got = 0u;
  if (!parse_range(p, n, &addr, &len))
    return RES_E01;
  /* 2 * len + 4 <= R, checked without overflow. */
  if (len > (uint64_t)((s->cfg.response_wire - 4u) / 2u) ||
      len > (uint64_t)s->data_cap)
    return RES_E01;
  if (!s->be.read_mem)
    return RES_EMPTY;
  if (s->be.read_mem(s->be.ctx, addr, (size_t)len, s->data, &got) !=
      GDBSTUB_OK)
    return RES_E05;
  if (got != (size_t)len)
    return RES_E05;
  ob_hex_data(b, s->data, got);
  return RES_PAYLOAD;
}

static cmd_result cmd_write_mem(gdbstub_session *s, const uint8_t *p,
                                size_t n)
{
  uint64_t addr, len;
  size_t colon = find_byte(p, 0u, n, ':');
  size_t hexn;
  if (colon == n || !parse_range(p, colon, &addr, &len))
    return RES_E01;
  hexn = n - colon - 1u;
  if (hexn % 2u != 0u || len != (uint64_t)(hexn / 2u) ||
      hexn / 2u > s->data_cap)
    return RES_E01;
  if (!parse_hex_data(p + colon + 1u, hexn, s->data, hexn / 2u))
    return RES_E01;
  if (!s->be.write_mem)
    return RES_EMPTY;
  if (s->be.write_mem(s->be.ctx, addr, s->data, hexn / 2u) != GDBSTUB_OK)
    return RES_E05;
  return RES_OK;
}

static cmd_result cmd_threads(gdbstub_session *s, int first, outbuf *b)
{
  size_t cnt = 0u, i;
  if (!s->be.threads)
    return RES_EMPTY;
  if (s->be.threads(s->be.ctx, first, s->ids, s->ids_cap, &cnt) !=
      GDBSTUB_OK)
    return RES_E05;
  if (cnt > s->ids_cap)
    return RES_E05;
  for (i = 0u; i < cnt; i++)
    if (s->ids[i] == 0u)
      return RES_E05;
  if (cnt == 0u) {
    ob_str(b, "l");
    return RES_PAYLOAD;
  }
  ob_str(b, "m");
  for (i = 0u; i < cnt && !b->over; i++) {
    if (i > 0u)
      ob_str(b, ",");
    ob_hex_u64(b, s->ids[i]);
  }
  return RES_PAYLOAD;
}

static cmd_result cmd_current_thread(gdbstub_session *s, outbuf *b)
{
  uint64_t id = 0u;
  if (!s->be.current_thread)
    return RES_EMPTY;
  if (s->be.current_thread(s->be.ctx, &id) != GDBSTUB_OK || id == 0u)
    return RES_E05;
  ob_str(b, "QC");
  ob_hex_u64(b, id);
  return RES_PAYLOAD;
}

static cmd_result cmd_select(gdbstub_session *s, const uint8_t *p, size_t n)
{
  gdbstub_thread t;
  char which;
  if (n == 0u)
    return RES_E01;
  if (p[0] != 'g' && p[0] != 'c')
    return RES_EMPTY; /* other H subcommands are out of scope */
  which = (char)p[0];
  if (!parse_selector(p + 1u, n - 1u, &t))
    return RES_E01;
  if (!s->be.select_thread)
    return RES_EMPTY;
  if (s->be.select_thread(s->be.ctx, which, t) != GDBSTUB_OK)
    return RES_E05;
  return RES_OK;
}

/* Z/z TYPE,ADDR,KIND. Type other than 0 and ";" extensions are empty; an
 * extra comma field is malformed. */
static cmd_result cmd_breakpoint(gdbstub_session *s, int insert,
                                 const uint8_t *p, size_t n)
{
  uint64_t type, addr, kind;
  size_t c1 = find_byte(p, 0u, n, ',');
  size_t c2, end;
  if (!parse_u64(p, c1, &type))
    return RES_E01;
  if (type != 0u)
    return RES_EMPTY;
  if (c1 == n)
    return RES_E01;
  c2 = find_byte(p, c1 + 1u, n, ',');
  if (c2 == n || !parse_u64(p + c1 + 1u, c2 - c1 - 1u, &addr))
    return RES_E01;
  end = c2 + 1u;
  while (end < n && p[end] != ',' && p[end] != ';')
    end++;
  if (!parse_u64(p + c2 + 1u, end - c2 - 1u, &kind))
    return RES_E01;
  if (end < n && p[end] == ';')
    return RES_EMPTY; /* conditions/commands extension: out of scope */
  if (end < n)
    return RES_E01; /* extra comma field */
  if (!s->be.breakpoint)
    return RES_EMPTY;
  if (s->be.breakpoint(s->be.ctx, insert, 0, addr, kind) != GDBSTUB_OK)
    return RES_E05;
  return RES_OK;
}

static cmd_result cmd_resume(gdbstub_session *s, char action,
                             const uint8_t *p, size_t n)
{
  uint64_t addr = 0u;
  int has = 0;
  if (n > 0u) {
    if (!parse_u64(p, n, &addr))
      return RES_E01;
    has = 1;
  }
  if (!s->be.resume)
    return RES_EMPTY;
  if (s->be.resume(s->be.ctx, action, addr, has) != GDBSTUB_OK)
    return RES_E05;
  return RES_RUNNING;
}

/* vCont;ACTION[:SELECTOR]... (p, n start after "vCont;"). All actions are
 * validated before any call: an unknown action makes the whole command
 * empty; otherwise a malformed c/s action, an empty token or too many
 * actions is E01. */
static cmd_result cmd_vcont(gdbstub_session *s, const uint8_t *p, size_t n)
{
  size_t i, j, count = 0u;
  int unknown = 0, malformed = 0;
  /* j >= i on every token, so this runs at most n + 1 times. */
  for (i = 0u; i <= n; i = j + 1u) {
    size_t tl;
    j = find_byte(p, i, n, ';');
    tl = j - i;
    if (tl == 0u) {
      malformed = 1;
    } else if (p[i] != 'c' && p[i] != 's') {
      unknown = 1;
    } else {
      gdbstub_thread t;
      t.kind = GDBSTUB_THREAD_ALL;
      t.id = 0u;
      if (tl > 1u &&
          (p[i + 1u] != ':' || !parse_selector(p + i + 2u, tl - 2u, &t)))
        malformed = 1;
      if (count < s->actions_cap) {
        s->actions[count].action = (char)p[i];
        s->actions[count].thread = t;
      }
    }
    count++;
    if (j >= n)
      break;
  }
  if (unknown)
    return RES_EMPTY;
  if (malformed || count > s->cfg.max_actions || count > s->actions_cap)
    return RES_E01;
  if (!s->be.vcont)
    return RES_EMPTY;
  if (s->be.vcont(s->be.ctx, s->actions, count) != GDBSTUB_OK)
    return RES_E05;
  return RES_RUNNING;
}

static cmd_result cmd_detach(gdbstub_session *s)
{
  if (!s->be.detach)
    return RES_EMPTY;
  if (s->be.detach(s->be.ctx) != GDBSTUB_OK)
    return RES_E05;
  return RES_DETACHED;
}

static cmd_result cmd_kill(gdbstub_session *s)
{
  const char *reason = GDBSTUB_REASON_KILL_ABSENT;
  if (s->be.kill)
    reason = s->be.kill(s->be.ctx) == GDBSTUB_OK ? GDBSTUB_REASON_KILL_OK
                                                 : GDBSTUB_REASON_KILL_ERROR;
  session_close(s, reason);
  return RES_CLOSED;
}

static cmd_result dispatch_cmd(gdbstub_session *s, const uint8_t *p,
                               size_t n, outbuf *b)
{
  if (n == 0u)
    return RES_EMPTY;
  switch (p[0]) {
  case '?':
    return n == 1u ? cmd_stop_reason(s, b) : RES_E01;
  case 'g':
    return n == 1u ? cmd_read_regs(s, b) : RES_E01;
  case 'G':
    return cmd_write_regs(s, p + 1u, n - 1u);
  case 'p':
    return cmd_read_reg(s, p + 1u, n - 1u, b);
  case 'P':
    return cmd_write_reg(s, p + 1u, n - 1u);
  case 'm':
    return cmd_read_mem(s, p + 1u, n - 1u, b);
  case 'M':
    return cmd_write_mem(s, p + 1u, n - 1u);
  case 'H':
    return cmd_select(s, p + 1u, n - 1u);
  case 'Z':
  case 'z':
    return cmd_breakpoint(s, p[0] == 'Z', p + 1u, n - 1u);
  case 'c':
  case 's':
    return cmd_resume(s, (char)p[0], p + 1u, n - 1u);
  case 'D':
    return n == 1u ? cmd_detach(s) : RES_E01;
  case 'k':
    return n == 1u ? cmd_kill(s) : RES_E01;
  case 'q':
    if (is_lit(p, n, "qSupported") || has_prefix(p, n, "qSupported:"))
      return cmd_supported(s, b);
    if (is_lit(p, n, "qfThreadInfo"))
      return cmd_threads(s, 1, b);
    if (is_lit(p, n, "qsThreadInfo"))
      return cmd_threads(s, 0, b);
    if (is_lit(p, n, "qC"))
      return cmd_current_thread(s, b);
    return RES_EMPTY;
  case 'v':
    if (is_lit(p, n, "vCont?")) {
      if (!s->be.vcont)
        return RES_EMPTY;
      ob_str(b, "vCont;c;s");
      return RES_PAYLOAD;
    }
    if (has_prefix(p, n, "vCont;"))
      return cmd_vcont(s, p + 6u, n - 6u);
    return RES_EMPTY;
  default:
    return RES_EMPTY;
  }
}

/* Runs once the request's '+' has been written completely. */
static void dispatch(gdbstub_session *s)
{
  outbuf b;
  cmd_result r;
  s->dispatch_pending = 0;
  b.p = s->payload;
  b.cap = s->cfg.response_wire - 4u;
  b.n = 0u;
  b.over = 0;
  r = dispatch_cmd(s, s->dec, s->dec_len, &b);
  switch (r) {
  case RES_EMPTY:
    reply_set(s, s->payload, 0u);
    s->base = GDBSTUB_WAIT_ACK;
    break;
  case RES_E01:
    reply_str(s, "E01");
    s->base = GDBSTUB_WAIT_ACK;
    break;
  case RES_E05:
    reply_str(s, "E05");
    s->base = GDBSTUB_WAIT_ACK;
    break;
  case RES_OK:
    reply_str(s, "OK");
    s->base = GDBSTUB_WAIT_ACK;
    break;
  case RES_PAYLOAD:
    if (b.over)
      reply_str(s, "E05");
    else
      reply_set(s, b.p, b.n);
    s->base = GDBSTUB_WAIT_ACK;
    break;
  case RES_DETACHED:
    reply_str(s, "OK");
    s->base = GDBSTUB_DETACH_ACK;
    break;
  case RES_RUNNING:
    s->have_reply = 0;
    s->reply_len = s->reply_off = 0u;
    s->base = GDBSTUB_RUNNING;
    break;
  case RES_CLOSED:
    break;
  }
}

/* ---- input ------------------------------------------------------------ */

/* Reject the current candidate frame with one NACK. */
static void frame_reject(gdbstub_session *s, int skip_rest)
{
  s->ack = '-';
  s->ack_pending = 1;
  s->fstate = skip_rest ? FR_SKIP_BODY : FR_IDLE;
  s->flen = 0u;
  if (s->base == GDBSTUB_READY)
    s->dl_active = 0;
}

static void frame_complete(gdbstub_session *s)
{
  size_t got = 0u;
  const uint8_t *body = s->frame + 1u;
  size_t bodyn = s->flen - 4u;
  if (gdbstub_codec_decode(s->frame, s->flen, s->dec, s->cfg.decoded,
                           &got) != GDBSTUB_OK) {
    frame_reject(s, 0);
    return;
  }
  s->fstate = FR_IDLE;
  s->flen = 0u;
  if (s->base == GDBSTUB_READY) {
    memcpy(s->req, body, bodyn);
    s->req_len = bodyn;
    s->have_req = 1;
    s->dec_len = got;
    s->ack = '+';
    s->ack_pending = 1;
    s->dispatch_pending = 1;
    return;
  }
  if (!s->have_req || bodyn != s->req_len ||
      memcmp(body, s->req, bodyn) != 0) {
    session_close(s, GDBSTUB_REASON_PROTOCOL);
    return;
  }
  /* Exact duplicate of the current request. */
  s->ack = '+';
  s->ack_pending = 1;
  if (s->base != GDBSTUB_RUNNING && s->have_reply) {
    s->reply_off = 0u;
    s->reply_pending = 1;
  }
}

/* Feed one byte. Returns 1 if consumed, 0 if it must be fed again (a '$'
 * that interrupted a candidate frame). */
static int feed(gdbstub_session *s, uint8_t c)
{
  switch (s->fstate) {
  case FR_IDLE:
    if (c == '$') {
      s->fstate = FR_BODY;
      s->frame[0] = c;
      s->flen = 1u;
      if (s->base == GDBSTUB_READY && !s->dl_active) {
        s->dl_active = 1;
        s->dl_start = s->now;
      }
    } else if (c == '+') {
      if (s->base == GDBSTUB_WAIT_ACK) {
        s->base = GDBSTUB_READY;
        s->have_reply = 0;
        s->reply_len = s->reply_off = 0u;
        s->have_req = 0;
        s->req_len = 0u;
        s->dl_active = 0;
      } else if (s->base == GDBSTUB_DETACH_ACK) {
        session_close(s, GDBSTUB_REASON_DETACHED);
      }
    } else if (c == '-') {
      if ((s->base == GDBSTUB_WAIT_ACK || s->base == GDBSTUB_DETACH_ACK) &&
          s->have_reply) {
        s->reply_off = 0u;
        s->reply_pending = 1;
      }
    }
    return 1;
  case FR_BODY:
    if (c == '$') {
      frame_reject(s, 0);
      return 0;
    }
    if (c == '#') {
      s->frame[s->flen++] = c;
      s->fstate = FR_CSUM1;
      return 1;
    }
    if (s->flen - 1u >= s->cfg.wire) {
      frame_reject(s, 1);
      return 1;
    }
    s->frame[s->flen++] = c;
    return 1;
  case FR_CSUM1:
    if (c == '$') {
      frame_reject(s, 0);
      return 0;
    }
    s->frame[s->flen++] = c;
    s->fstate = FR_CSUM2;
    return 1;
  case FR_CSUM2:
    if (c == '$') {
      frame_reject(s, 0);
      return 0;
    }
    s->frame[s->flen++] = c;
    frame_complete(s);
    return 1;
  case FR_SKIP_BODY:
    if (c == '$') {
      s->fstate = FR_IDLE;
      return 0;
    }
    if (c == '#')
      s->fstate = FR_SKIP_CSUM1;
    return 1;
  case FR_SKIP_CSUM1:
    if (c == '$') {
      s->fstate = FR_IDLE;
      return 0;
    }
    s->fstate = FR_SKIP_CSUM2;
    return 1;
  default: /* FR_SKIP_CSUM2 */
    s->fstate = FR_IDLE;
    return c == '$' ? 0 : 1;
  }
}

/* ---- transport -------------------------------------------------------- */

static int write_some(gdbstub_session *s)
{
  const uint8_t *p;
  size_t len;
  gdbstub_io_result io;
  if (s->ack_pending) {
    p = &s->ack;
    len = 1u;
  } else {
    p = s->reply + s->reply_off;
    len = s->reply_len - s->reply_off;
  }
  io = s->tr.write(s->tr.ctx, p, len);
  switch (io.status) {
  case GDBSTUB_OK:
    if (io.count == 0u || io.count > len) {
      session_close(s, GDBSTUB_REASON_IO_CONTRACT);
      return IO_PROGRESS;
    }
    if (s->ack_pending) {
      s->ack_pending = 0;
    } else {
      s->reply_off += io.count;
      if (s->reply_off == s->reply_len)
        s->reply_pending = 0;
    }
    return IO_PROGRESS;
  case GDBSTUB_AGAIN:
    if (io.count != 0u) {
      session_close(s, GDBSTUB_REASON_IO_CONTRACT);
      return IO_PROGRESS;
    }
    return IO_BLOCKED;
  case GDBSTUB_ERROR:
    session_close(s, io.count == 0u ? GDBSTUB_REASON_IO
                                    : GDBSTUB_REASON_IO_CONTRACT);
    return IO_PROGRESS;
  default:
    session_close(s, GDBSTUB_REASON_IO_CONTRACT);
    return IO_PROGRESS;
  }
}

static int read_some(gdbstub_session *s)
{
  gdbstub_io_result io = s->tr.read(s->tr.ctx, s->in, SESSION_IN_CAP);
  switch (io.status) {
  case GDBSTUB_OK:
    if (io.count == 0u || io.count > SESSION_IN_CAP) {
      session_close(s, GDBSTUB_REASON_IO_CONTRACT);
      return IO_PROGRESS;
    }
    s->in_len = io.count;
    s->in_pos = 0u;
    return IO_PROGRESS;
  case GDBSTUB_AGAIN:
    if (io.count != 0u) {
      session_close(s, GDBSTUB_REASON_IO_CONTRACT);
      return IO_PROGRESS;
    }
    return IO_IDLE;
  case GDBSTUB_EOF:
    session_close(s, io.count == 0u ? GDBSTUB_REASON_EOF
                                    : GDBSTUB_REASON_IO_CONTRACT);
    return IO_PROGRESS;
  case GDBSTUB_ERROR:
    session_close(s, io.count == 0u ? GDBSTUB_REASON_IO
                                    : GDBSTUB_REASON_IO_CONTRACT);
    return IO_PROGRESS;
  default:
    session_close(s, GDBSTUB_REASON_IO_CONTRACT);
    return IO_PROGRESS;
  }
}

static gdbstub_status pump(gdbstub_session *s)
{
  unsigned budget;
  for (budget = 0u; budget < SESSION_PUMP_BUDGET; budget++) {
    if (s->closed)
      return GDBSTUB_OK;
    if (s->ack_pending || s->reply_pending) {
      if (write_some(s) == IO_BLOCKED)
        return GDBSTUB_AGAIN;
      continue;
    }
    if (s->dispatch_pending) {
      dispatch(s);
      continue;
    }
    if (s->in_pos < s->in_len) {
      /* feed() leaves a byte unconsumed only after queueing a NACK (which
       * ends this loop) or after leaving a skip state for FR_IDLE, where
       * the byte is consumed next: at most two passes per byte. */
      while (s->in_pos < s->in_len && !s->closed && !output_pending(s)) {
        if (feed(s, s->in[s->in_pos]))
          s->in_pos++;
      }
      continue;
    }
    if (read_some(s) == IO_IDLE)
      return GDBSTUB_OK;
  }
  return GDBSTUB_AGAIN;
}

/* ---- public entry points --------------------------------------------- */

gdbstub_session *gdbstub_session_create(const gdbstub_config *cfg,
                                        const gdbstub_backend *backend,
                                        const gdbstub_transport *transport)
{
  gdbstub_session *s;
  gdbstub_config c;
  size_t data_cap, ids_cap, actions_cap;

  if (!transport || !transport->read || !transport->write)
    return 0;
  if (cfg) {
    c = *cfg;
  } else {
    c.wire = 2048u;
    c.decoded = 2048u;
    c.response_wire = 2048u;
    c.max_actions = 16u;
    c.deadline_ms = 1000u;
  }
  if (c.wire == 0u || c.wire > SIZE_MAX - 4u || c.decoded == 0u ||
      c.response_wire < 8u || c.max_actions == 0u)
    return 0;

  /* Binary scratch: full register block, m reply data ((R - 4) / 2) and M
   * data (at most D / 2). */
  data_cap = GDBSTUB_REG_TOTAL_BYTES;
  if ((c.response_wire - 4u) / 2u > data_cap)
    data_cap = (c.response_wire - 4u) / 2u;
  if (c.decoded / 2u > data_cap)
    data_cap = c.decoded / 2u;
  /* Thread list: "m" + n IDs + (n - 1) commas fits R - 4 only if
   * n <= (R - 4) / 2. */
  ids_cap = (c.response_wire - 4u) / 2u;
  if (ids_cap > SIZE_MAX / sizeof(uint64_t))
    return 0;
  /* Each vCont action needs at least two decoded bytes (";c"). */
  actions_cap = c.decoded / 2u + 1u;
  if (c.max_actions < actions_cap)
    actions_cap = c.max_actions;
  if (actions_cap > SIZE_MAX / sizeof(gdbstub_action))
    return 0;

  s = (gdbstub_session *)calloc(1u, sizeof *s);
  if (!s)
    return 0;
  s->cfg = c;
  if (backend)
    s->be = *backend;
  s->tr = *transport;
  s->base = GDBSTUB_READY;
  s->last = GDBSTUB_OK;
  s->fstate = FR_IDLE;
  s->data_cap = data_cap;
  s->ids_cap = ids_cap;
  s->actions_cap = actions_cap;
  s->frame = (uint8_t *)malloc(c.wire + 4u);
  s->req = (uint8_t *)malloc(c.wire);
  s->dec = (uint8_t *)malloc(c.decoded);
  s->reply = (uint8_t *)malloc(c.response_wire);
  s->payload = (uint8_t *)malloc(c.response_wire);
  s->data = (uint8_t *)malloc(data_cap);
  s->ids = (uint64_t *)malloc(ids_cap * sizeof(uint64_t));
  s->actions = (gdbstub_action *)malloc(actions_cap * sizeof(gdbstub_action));
  if (!s->frame || !s->req || !s->dec || !s->reply || !s->payload ||
      !s->data || !s->ids || !s->actions) {
    gdbstub_session_free(s);
    return 0;
  }
  return s;
}

void gdbstub_session_free(gdbstub_session *s)
{
  if (!s)
    return;
  free(s->frame);
  free(s->req);
  free(s->dec);
  free(s->reply);
  free(s->payload);
  free(s->data);
  free(s->ids);
  free(s->actions);
  free(s);
}

gdbstub_status gdbstub_session_step(gdbstub_session *s,
                                    const gdbstub_event *ev, uint64_t now_ms)
{
  gdbstub_status rc;
  if (!s || !ev || s->closed)
    return GDBSTUB_BAD_STATE;
  if (ev->kind != GDBSTUB_EVT_PUMP && ev->kind != GDBSTUB_EVT_STOP)
    return GDBSTUB_BAD_STATE;
  if (ev->kind == GDBSTUB_EVT_STOP &&
      (gdbstub_session_state(s) != GDBSTUB_RUNNING ||
       (ev->has_thread && ev->thread == 0u)))
    return GDBSTUB_BAD_STATE;

  s->now = now_ms;
  if (s->dl_active && now_ms >= s->dl_start &&
      now_ms - s->dl_start > (uint64_t)s->cfg.deadline_ms) {
    session_close(s, GDBSTUB_REASON_TIMEOUT);
    s->last = GDBSTUB_OK;
    return GDBSTUB_OK;
  }

  if (ev->kind == GDBSTUB_EVT_STOP) {
    outbuf b;
    b.p = s->payload;
    b.cap = s->cfg.response_wire - 4u;
    b.n = 0u;
    b.over = 0;
    ob_str(&b, ev->has_thread ? "T" : "S");
    ob_hex_u8(&b, ev->signal);
    if (ev->has_thread) {
      ob_str(&b, "thread:");
      ob_hex_u64(&b, ev->thread);
      ob_str(&b, ";");
    }
    if (b.over)
      reply_str(s, "E05");
    else
      reply_set(s, b.p, b.n);
    s->base = GDBSTUB_WAIT_ACK;
  }

  rc = pump(s);
  s->last = rc;
  return rc;
}

gdbstub_state gdbstub_session_state(const gdbstub_session *s)
{
  if (!s || s->closed)
    return GDBSTUB_CLOSED;
  if (output_pending(s))
    return GDBSTUB_TX;
  return s->base;
}

const char *gdbstub_session_reason(const gdbstub_session *s)
{
  return s && s->closed ? s->reason : 0;
}

gdbstub_status gdbstub_session_last_status(const gdbstub_session *s)
{
  return s ? s->last : GDBSTUB_BAD_STATE;
}
