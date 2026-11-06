/* Live link, protocol version 1: the wire schema shared by the agent and its
 * tests. Design: docs/design/ps3dk-prof-viewer.md, "Phase 2". All integers
 * are little-endian. Nothing here changes the capture format: DATA carries
 * the capture file's bytes unchanged. */
#ifndef PS3DK_PROF_LIVE_PROTO_H
#define PS3DK_PROF_LIVE_PROTO_H
#include <stdint.h>

/* Frame header: u16 type, u16 flags (must be 0), u32 payload length. */
#define LIVE_HDR_BYTES 8u
#define LIVE_MAX_PAYLOAD 65536u

/* Viewer -> agent. Payload exactly 8 bytes: u32 magic, u16 version, u16 0. */
#define LIVE_T_CLIENT_HELLO 1u
#define LIVE_MAGIC 0x564c3350u /* "P3LV" */
#define LIVE_VERSION 1u

/* Agent -> viewer, once, first frame after a valid CLIENT_HELLO:
 *   u16 protocol version, u16 agent version, u32 0,
 *   u64 session id (the capture's init timebase value),
 *   then three strings, each u16 length + bytes:
 *     app (1..LIVE_MAX_APP), build id (0..LIVE_MAX_BUILD_ID),
 *     file name (1..LIVE_MAX_NAME, the capture's base name, no '/').
 * The identity is for display; version 1 has no resume. */
#define LIVE_T_HELLO 2u
#define LIVE_AGENT_VERSION 1u
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
