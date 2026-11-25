/*
 * ps3dk-run: wire protocol 2, shared by the loader, the run hook and the host tool.
 *
 * Frame: u32 magic+version, u8 type, u32 payload length, payload, then - once a session or run is authenticated - a
 * 32-byte tag HMAC(key, direction | seq | type | length | payload).  Big-endian throughout.
 * Trusted LAN only: authentication and integrity, no confidentiality.
 */
#ifndef PS3DK_RUN_PROTO_H
#define PS3DK_RUN_PROTO_H

#include <stdint.h>

#define PRN_MAGIC          0x50524E02u   /* "PRN" + protocol version 2 */
#define PRN_LOADER_PORT    4299
#define PRN_MAX_FRAME      (1u << 20)    /* 1 MiB payload */
#define PRN_MAX_ARGC       64
#define PRN_MAX_ARGV_BYTES 4096
#define PRN_MAX_FILES      256
#define PRN_MAX_FILE_SIZE  (64u << 20)
#define PRN_MAX_TOTAL_SIZE (256u << 20)
#define PRN_MAX_PATH       200
#define PRN_MAX_LIST       1024
#define PRN_TAG            32

/* Deadlines (seconds) */
#define PRN_AUTH_S         10
#define PRN_UPLOAD_IDLE_S  30
#define PRN_CALLBACK_S     60

enum prn_type {
    /* run upload, host -> loader (authenticated) */
    PRN_HEADER = 1,      /* RETIRED (no callback address); the loader rejects it before parsing anything */
    PRN_HEADER_CB = 15,  /* u32 nfiles, u32 argc, u16 callback port, u32 timeout s, u32 callback IPv4 (0 = the peer
                            address, all ones rejected), argv: (u16 len, bytes)*,
                            files: (u16 path len, path, u64 size, 32-byte sha256)*; file 0 is the SELF */
    PRN_FILE = 2,        /* u32 file index, data (chunks in order) */
    PRN_COMMIT = 3,      /* empty */
    /* loader -> host */
    PRN_ACCEPTED = 4,    /* u32 run id */
    PRN_BUSY = 5,        /* unauthenticated: another session is active */
    PRN_REJECTED = 6,    /* reason text (unauthenticated before the session key exists) */
    PRN_SPAWNING = 7,    /* u32 run id */
    PRN_SPAWN_FAILED = 8,/* s32 error */
    /* run (child / watchdog / loader READY) -> host, under the run key */
    PRN_HELLO = 9,       /* u32 run id, 32-byte child nonce   (direction 'C', seq 0) */
    PRN_STDOUT = 10,
    PRN_STDERR = 11,
    PRN_EXIT = 12,       /* s32 status, u64 dropped bytes */
    PRN_TIMEOUT = 13,    /* u32 run id, u64 dropped bytes     (direction 'W', seq 0, own connection) */
    PRN_READY = 14,      /* u32 run id                        (direction 'R', seq 0, own connection) */
    PRN_START_FAILED = 16,/* u32 reason (PRN_SF_*): the hook could not start after HELLO; it relaunches the loader
                            instead of running the child (direction 'C', next seq, best effort) */
    /* session authentication */
    PRN_CHALLENGE = 20,  /* loader -> host, plain: u8 protocol (2), u8 role ('L'), 32-byte Ns */
    PRN_AUTH = 21,       /* host -> loader, plain: u8 role ('H'), 32-byte Nc, 32-byte Th */
    PRN_AUTH_OK = 22,    /* loader -> host, plain: 32-byte Tl */
    PRN_HELLO_OK = 23,   /* host -> child under the run key (direction 'h', seq 0): u32 run id */
    /* file service (authenticated); every request starts with u32 request id, u8 root id */
    PRN_LIST = 30,       /* req, root, u16 dir len, dir */
    PRN_LIST_RESULT = 31,/* req, u8 truncated, u8 unstable, u32 n, n x (u8 type, u64 size, u16 name len, name) */
    PRN_GET = 32,        /* req, root, u16 path len, path */
    PRN_GET_DATA = 33,   /* req, data */
    PRN_GET_END = 34,    /* req, u64 bytes, 32-byte sha256 */
    PRN_PUT = 35,        /* req, root, u16 path len, path, u64 size, 32-byte sha256, u8 policy (0 NEW, 1 REPLACE) */
    PRN_PUT_DATA = 36,   /* req, data */
    PRN_PUT_COMMIT = 37, /* req */
    PRN_DELETE = 38,     /* req, root, u16 path len, path */
    PRN_FILES_OFF = 39,  /* req: turns the file service off (the host can never turn it on) */
    PRN_FILE_OK = 40,    /* req */
    PRN_FILE_ERR = 41,   /* req, text */
    PRN_BYE = 42         /* host -> loader: end of session */
    /* 100..127 reserved for the profiler (separate design) */
};

#define PRN_ROOT_TMP 1   /* /dev_hdd0/tmp */

/* PRN_START_FAILED reasons */
#define PRN_SF_HELLO    1   /* HELLO was sent but no valid HELLO_OK came back */
#define PRN_SF_WATCHDOG 2   /* the watchdog thread could not be created */
#define PRN_SF_SENDER   3   /* the output sender thread could not be created */

/* direction bytes in tags */
#define PRN_DIR_HOST   'H'   /* host -> loader */
#define PRN_DIR_LOADER 'L'   /* loader -> host */
#define PRN_DIR_CHILD  'C'   /* child -> host */
#define PRN_DIR_HOST_C 'h'   /* host -> child */
#define PRN_DIR_WDOG   'W'   /* watchdog -> host */
#define PRN_DIR_READY  'R'   /* loader READY -> host */

/* Marker the loader appends to the child's argv; the hook strips it.  It carries no key:
 *   --ps3dk-run=<run id>:<callback ip>:<callback port>:<timeout s>
 * and the one the hook passes back to the loader:
 *   --ps3dk-ready=<run id>:<callback ip>:<callback port> */
#define PRN_MARKER       "--ps3dk-run="
#define PRN_READY_MARKER "--ps3dk-ready="

#define PRN_LOADER_SELF  "/dev_hdd0/game/PSDKLOADR/USRDIR/EBOOT.BIN"
/* the loader as a plain-signed SELF, shipped in the PKG next to the NPDRM EBOOT.BIN: what the hook relaunches when it is
   present (hardware 2026-10-17: the relaunch of EBOOT.BIN from a run dropped to the XMB) */
#define PRN_LOADER_RELOAD "/dev_hdd0/game/PSDKLOADR/USRDIR/RELOAD.SELF"
#define PRN_SPAWN_LOG    "/dev_hdd0/tmp/ps3dk-run-spawn.log"
#define PRN_RUNS_DIR     "/dev_hdd0/game/PSDKLOADR/USRDIR/runs"
#define PRN_STATE_DIR    "/dev_hdd0/game/PSDKLOADR/USRDIR/state"     /* never served */
#define PRN_KEY_FILE     PRN_STATE_DIR "/key"
#define PRN_RUNKEY_FILE  PRN_STATE_DIR "/run.key"
#define PRN_FILES_ON     PRN_STATE_DIR "/files-on"
#define PRN_TMP_ROOT     "/dev_hdd0/tmp"

#define PRN_LABEL_HOST    "ps3dk-run/2 host"
#define PRN_LABEL_LOADER  "ps3dk-run/2 loader"
#define PRN_LABEL_SESSION "ps3dk-run/2 session"
#define PRN_LABEL_RUN     "ps3dk-run/2 run"

static inline void prn_put32(uint8_t *p, uint32_t v) { p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v; }
static inline void prn_put16(uint8_t *p, uint16_t v) { p[0] = v >> 8; p[1] = v; }
static inline void prn_put64(uint8_t *p, uint64_t v) { prn_put32(p, (uint32_t)(v >> 32)); prn_put32(p + 4, (uint32_t)v); }
static inline uint32_t prn_get32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
static inline uint16_t prn_get16(const uint8_t *p) { return (uint16_t)(p[0] << 8 | p[1]); }
static inline uint64_t prn_get64(const uint8_t *p) { return (uint64_t)prn_get32(p) << 32 | prn_get32(p + 4); }

#endif
