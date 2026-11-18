/* Live link, protocol versions 1 and 2: the wire schema shared by the agent,
 * its tests and the viewer. Design: docs/design/ps3dk-prof-viewer.md,
 * "Phase 2". All integers are little-endian. Nothing here changes the
 * capture format: DATA carries the capture file's bytes unchanged.
 *
 * Version 2 adds capture triggers (CAPTURE_BEGIN, CAPTURE_END,
 * CAPTURE_FRAMES and their CAPTURE_ACK). The viewer names the version in
 * CLIENT_HELLO; an agent serves every version from LIVE_VERSION_MIN to
 * LIVE_VERSION and answers in the version the viewer asked for, so a v1
 * viewer sees exactly the v1 protocol. A v1 agent closes a v2 hello (BAD
 * HELLO) without a reply: a v2 viewer then reconnects once asking for v1 and
 * keeps its triggers off. No v2 frame is ever sent on a v1 session. */
#ifndef PS3DK_PROF_LIVE_PROTO_H
#define PS3DK_PROF_LIVE_PROTO_H
#include <stdint.h>

/* Frame header: u16 type, u16 flags (must be 0), u32 payload length. */
#define LIVE_HDR_BYTES 8u
#define LIVE_MAX_PAYLOAD 65536u

/* Viewer -> agent. Payload exactly 8 bytes: u32 magic, u16 version
 * (LIVE_VERSION_MIN..LIVE_VERSION), u16 0. */
#define LIVE_T_CLIENT_HELLO 1u
#define LIVE_MAGIC 0x564c3350u /* "P3LV" */
#define LIVE_VERSION_MIN 1u
#define LIVE_VERSION 2u      /* the newest version */

/* Agent -> viewer, once, first frame after a valid CLIENT_HELLO:
 *   u16 protocol version (the one CLIENT_HELLO asked for), u16 agent version,
 *   u32 capabilities (version 1: 0; version 2: LIVE_CAP_* bits, unknown bits
 *   ignored by the viewer),
 *   u64 session id (the capture's init timebase value),
 *   then three strings, each u16 length + bytes:
 *     app (1..LIVE_MAX_APP), build id (0..LIVE_MAX_BUILD_ID),
 *     file name (1..LIVE_MAX_NAME, the capture's base name, no '/').
 * The identity is for display; version 1 has no resume. */
#define LIVE_T_HELLO 2u
#define LIVE_AGENT_VERSION 2u
#define LIVE_CAP_TRIGGERS 1u /* the agent applies CAPTURE_* commands (it serves a runtime) */
#define LIVE_MAX_APP 32u
#define LIVE_MAX_BUILD_ID 512u
#define LIVE_MAX_NAME 255u

/* Agent -> viewer: u64 absolute file offset, then 1..LIVE_MAX_DATA bytes.
 * The first DATA has offset 0; each next one starts where the last ended.
 * The offset counts inside the payload limit. */
#define LIVE_T_DATA 3u
#define LIVE_MAX_DATA (LIVE_MAX_PAYLOAD - 8u)

/* Agent -> viewer, last frame of a stream, 24 bytes:
 *   u8 io_finalized, u8 trailer_written, u8 join_failed, u8 0,
 *   i32 shutdown_result,
 *   u64 accepted_bytes (the finalization record's value),
 *   u64 sent_bytes (bytes this stream delivered in DATA).
 * The first four fields come from the runtime's finalization record.
 * accepted_bytes is the whole file's size only when io_finalized is 1; END
 * proves transport completion only, never a capture verdict. */
#define LIVE_T_END 4u
#define LIVE_END_BYTES 24u

/* Viewer -> agent, empty: at most LIVE_MAX_PINGS_PER_S a second. */
#define LIVE_T_PING 5u
/* Agent -> viewer, empty: one per PING. */
#define LIVE_T_PONG 6u
#define LIVE_MAX_PINGS_PER_S 4u

/* Agent -> second viewer, then close: u32 reason (LIVE_BUSY_*). Best-effort:
 * one non-blocking send attempt, then the connection is closed at once, so
 * a refused viewer may see only the close. */
#define LIVE_T_BUSY 7u
#define LIVE_BUSY_STREAMING 1u

/* Version 2 only, viewer -> agent, while streaming (after HELLO, before END).
 * At most ONE command is outstanding: the viewer sends the next only after
 * the CAPTURE_ACK of the last. A command on a v1 session, a second one while
 * one is outstanding, a request id of 0, a nonzero reserved field, a name
 * length outside 1..LIVE_MAX_WINDOW_NAME, a frame count outside
 * 1..LIVE_MAX_WINDOW_FRAMES, or a payload length that is not exactly the
 * layout's closes the link (a protocol drop, as in v1).
 *   CAPTURE_BEGIN:  u32 request id, u16 name length, u16 0, name bytes.
 *                   A MANUAL window named <name>, from the next frame mark.
 *   CAPTURE_END:    u32 request id. Closes the window this link opened.
 *   CAPTURE_FRAMES: u32 request id, u32 frames, u16 name length, u16 0,
 *                   name bytes. A FRAMES window of that many frames; it
 *                   closes itself (reason COUNT).
 * The name is the window's name in the capture, recorded as for a window the
 * app opens; it must be UTF-8 without NUL (else the ACK refuses EINVAL). */
#define LIVE_T_CAPTURE_BEGIN 8u
#define LIVE_T_CAPTURE_END 9u
#define LIVE_T_CAPTURE_FRAMES 10u
#define LIVE_MAX_WINDOW_NAME 63u
#define LIVE_MAX_WINDOW_FRAMES 1000000u
#define LIVE_CMD_MAX_BYTES (12u + LIVE_MAX_WINDOW_NAME)

/* Version 2 only, agent -> viewer, exactly one per command, 16 bytes:
 *   u32 request id, i32 result (0, or the PS3DK_PROF_* refusal),
 *   u32 window id (the window opened or closed; 0 when refused),
 *   u16 command type, u16 0.
 * A refusal changes nothing in the capture. The agent answers within its
 * command deadline (a request no frame mark took in time is withdrawn and
 * refused PS3DK_PROF_ETIMEDOUT) unless the link ends first; END is never
 * followed by an ACK. */
#define LIVE_T_CAPTURE_ACK 11u
#define LIVE_ACK_BYTES 16u

/* 64..79 are reserved for TTY and 80..95 for debugger traffic. Reserving
 * them defines no security or scheduling design for those channels. */
#define LIVE_T_RESERVED_TTY_FIRST 64u
#define LIVE_T_RESERVED_DEBUG_LAST 95u

/* A capture larger than this is not streamed past the cap. */
#define LIVE_DEFAULT_CAP_BYTES (1ull << 30)

static inline void live_put16(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static inline void live_put32(uint8_t *p, uint32_t v) { live_put16(p, v); live_put16(p + 2, v >> 16); }
static inline void live_put64(uint8_t *p, uint64_t v) { live_put32(p, (uint32_t)v); live_put32(p + 4, (uint32_t)(v >> 32)); }
static inline uint32_t live_get16(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8; }
static inline uint32_t live_get32(const uint8_t *p) { return live_get16(p) | live_get16(p + 2) << 16; }
static inline uint64_t live_get64(const uint8_t *p) { return live_get32(p) | (uint64_t)live_get32(p + 4) << 32; }

#endif
