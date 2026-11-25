/* ps3dk-run: run hook (see ps3dk_run_hook.h); protocol 2: every frame tagged under the run key. */
#include "ps3dk_run_hook.h"
#include "../common/prn_hmac.h"
#include "../common/prn_net.h"
#include "../common/ps3dk_run_proto.h"

#include <net/net.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/process.h>
#include <sys/random_number.h>
#include <sys/socket.h>
#include <sys/syscalls.h>
#include <sys/systime.h>
#include <sys/file.h>
#include <sys/thread.h>
#include <unistd.h>

/* Fault injection for tests only; 0 (off) in every pinned build.  1: the watchdog thread is not created;
 * 2: the sender thread is not created; 3: HELLO goes out with a corrupted tag, so the host refuses it;
 * 4: the hook rejects the host's valid HELLO_OK (its expected tag is corrupted);
 * 5: the watchdog fires at once and owns the latch before the start fails (sender), and its relaunch is skipped as if
 *    exitspawn had returned: the start must return -1;
 * 6: as 5, but the watchdog's relaunch is real: TIMEOUT must be the only outcome;
 * 7: as 5, but the waiter's join is forced to fail (an invalid thread id) and the watchdog publishes its failed
 *    relaunch only after WDOG_PUBLISH_DELAY_MS; the waiter logs that it is still waiting before then. */
#ifndef PS3DK_RUN_HOOK_FAULT
#define PS3DK_RUN_HOOK_FAULT 0
#endif

#define WDOG_PUBLISH_DELAY_MS 3000   /* fault 7 only */

#define RING_SIZE   (256u * 1024u)
#define CHUNK_MAX   (16u * 1024u)
#define DRAIN_MS    3000

static struct {
    uint32_t run_id;
    char ip[16];
    uint16_t port;
    uint32_t timeout_s;
    int fd;
    uint8_t kr[32];                         /* run key from state/run.key (never argv) */
    uint32_t seq;                           /* next child->host sequence number (direction C) */
    /* bounded queue of whole frames */
    pthread_mutex_t lock;
    pthread_cond_t cond;
    uint8_t ring[RING_SIZE];
    size_t head, count;
    uint64_t dropped;
    int disconnected, stop;
    pthread_t sender;
    sys_ppu_thread_t watchdog;
    int have_watchdog;
    int wdog_spawn_failed;                  /* watchdog: release store after its exitspawn returned; waiters: acquire load */
    volatile int latch;                     /* one terminal outcome: EXIT, TIMEOUT or a failed start */
    _ssize_t (*orig_write)(struct _reent *, int, const void *, size_t);
    void (*orig_exit)(int);
} H = { .fd = -1 };

static int take_latch(void) { return __sync_bool_compare_and_swap(&H.latch, 0, 1); }

/* The loader relaunch, prepared once by prepare_spawn() right after the marker parses (before the watchdog exists and
   before any failure that relaunches); afterwards only read.  The watchdog path uses it without formatting, stat or file I/O. */
static struct {
    const char *target;
    char marker[64];
    const char *argv[2];
    char log_before[192], log_after[192];
    int before_len, after_len;
} S;

static int prep_line(char *buf, size_t size, const char *what)
{
    int n = snprintf(buf, size, "run %u: %s %s", (unsigned)H.run_id, what, S.target);
    if (n < 0 || n >= (int)size - 1) return 0;
    buf[n++] = 10;
    return n;
}

static void prepare_spawn(void)
{
    sysFSStat st;
    S.target = sysLv2FsStat(PRN_LOADER_RELOAD, &st) == 0 ? PRN_LOADER_RELOAD : PRN_LOADER_SELF;
    snprintf(S.marker, sizeof S.marker, PRN_READY_MARKER "%u:%s:%u", (unsigned)H.run_id, H.ip, (unsigned)H.port);
    S.argv[0] = S.marker;
    S.argv[1] = NULL;
    S.before_len = prep_line(S.log_before, sizeof S.log_before, "exitspawn");
    S.after_len = prep_line(S.log_after, sizeof S.log_after, "exitspawn returned for");
}

/* Normal-exit path only (diagnostic): appends a prepared line. */
static void spawn_log(const char *line, int len)
{
    s32 fd;
    u64 written;
    if (len <= 0) return;
    if (sysLv2FsOpen(PRN_SPAWN_LOG, SYS_O_WRONLY | SYS_O_CREAT | SYS_O_APPEND, &fd, 0666, NULL, 0) != 0) return;
    sysLv2FsWrite(fd, line, (u64)len, &written);
    sysLv2FsClose(fd);
}

static void spawn_loader(int log)
{
    if (log) spawn_log(S.log_before, S.before_len);
    exitspawn(S.target, S.argv, NULL, 0, 0, 1001, SYS_PROCESS_SPAWN_STACK_SIZE_1M);
    if (log) spawn_log(S.log_after, S.after_len);
}

/* frame header + payload + tag(key, dir, seq) sent directly (EXIT, TIMEOUT, HELLO) */
static int send_tagged(int fd, const uint8_t key[32], uint8_t dir, uint32_t seq, uint8_t type, const uint8_t *p, uint32_t n)
{
    uint8_t tag[PRN_TAG];
    prn_tag(key, dir, seq, type, p, n, tag);
    if (prn_send_frame(fd, type, p, n) != 0) return -1;
    return prn_send_all(fd, tag, sizeof tag);
}

/* Appends one whole tagged frame or drops it (counted; a dropped frame takes no sequence number, so the host sees an
 * unbroken sequence).  Never blocks on the network. */
static void enqueue(uint8_t type, const void *p, size_t n)
{
    uint8_t hdr[9], tag[PRN_TAG];
    if (n > PRN_MAX_FRAME) n = PRN_MAX_FRAME;
    prn_put32(hdr, PRN_MAGIC);
    hdr[4] = type;
    prn_put32(hdr + 5, (uint32_t)n);
    pthread_mutex_lock(&H.lock);
    if (H.disconnected || H.stop || H.count + sizeof hdr + n + sizeof tag > RING_SIZE) {
        H.dropped += n;
    } else {
        const uint8_t *parts[3] = { hdr, p, tag };
        size_t lens[3] = { sizeof hdr, n, sizeof tag };
        prn_tag(H.kr, PRN_DIR_CHILD, H.seq++, type, p, (uint32_t)n, tag);
        for (int k = 0; k < 3; k++)
            for (size_t i = 0; i < lens[k]; i++)
                H.ring[(H.head + H.count++) % RING_SIZE] = parts[k][i];
        pthread_cond_signal(&H.cond);
    }
    pthread_mutex_unlock(&H.lock);
}

static void *sender_main(void *arg)
{
    static uint8_t chunk[CHUNK_MAX];
    (void)arg;
    for (;;) {
        size_t n;
        pthread_mutex_lock(&H.lock);
        while (H.count == 0 && !H.stop) pthread_cond_wait(&H.cond, &H.lock);
        if (H.count == 0 && H.stop) { pthread_mutex_unlock(&H.lock); return NULL; }
        n = H.count < CHUNK_MAX ? H.count : CHUNK_MAX;
        for (size_t i = 0; i < n; i++) chunk[i] = H.ring[(H.head + i) % RING_SIZE];
        H.head = (H.head + n) % RING_SIZE;
        H.count -= n;
        pthread_cond_broadcast(&H.cond);
        pthread_mutex_unlock(&H.lock);
        if (prn_send_all(H.fd, chunk, n) != 0) {
            pthread_mutex_lock(&H.lock);
            H.disconnected = 1;
            H.dropped += n + H.count;
            H.count = 0;
            pthread_cond_broadcast(&H.cond);
            pthread_mutex_unlock(&H.lock);
            return NULL;
        }
    }
}

static _ssize_t hooked_write(struct _reent *r, int fd, const void *p, size_t n)
{
    if ((fd == 1 || fd == 2) && n)
        enqueue(fd == 1 ? PRN_STDOUT : PRN_STDERR, p, n);
    return H.orig_write ? H.orig_write(r, fd, p, n) : (_ssize_t)n;
}

/* wall-clock ms, read only for the fault-7 test log */
static u64 test_now_ms(void)
{
    u64 sec = 0, nsec = 0;
    sysGetCurrentTime(&sec, &nsec);
    return sec * 1000u + nsec / 1000000u;
}

/* For a caller that lost the latch to the watchdog.  The watchdog's exitspawn replaces the process when it works, so
 * this returns only when the watchdog ended without a relaunch.  A successful join (0) is that proof: the watchdog
 * owns the latch, so it ends only after its exitspawn returned.  A failed join proves nothing; then wait for the flag
 * the watchdog publishes right after a returned exitspawn (release store there, acquire load here). */
static void wait_watchdog_relaunch_failed(int test_log)
{
    u64 rv;
    /* fault 7 (test only): join an id that is not a thread, so the join itself fails */
    s32 rc = sysThreadJoin(PS3DK_RUN_HOOK_FAULT == 7 ? (sys_ppu_thread_t)~0ull : H.watchdog, &rv);
    if (rc == 0 && PS3DK_RUN_HOOK_FAULT != 7) return;
    if (PS3DK_RUN_HOOK_FAULT == 7 && test_log) {
        printf("HOOK-TEST join rc=%d (forced failure); flag %s: waiting\n", (int)rc,
               __atomic_load_n(&H.wdog_spawn_failed, __ATOMIC_ACQUIRE) ? "set" : "clear");
        fflush(stdout);
    }
    u64 t0 = test_now_ms();
    int logged = 0;
    while (!__atomic_load_n(&H.wdog_spawn_failed, __ATOMIC_ACQUIRE)) {
        sysUsleep(10000);
        if (PS3DK_RUN_HOOK_FAULT == 7 && test_log && !logged && test_now_ms() - t0 >= WDOG_PUBLISH_DELAY_MS / 2) {
            printf("HOOK-TEST still waiting at %u ms; flag clear\n", (unsigned)(test_now_ms() - t0));
            fflush(stdout);
            logged = 1;
        }
    }
    if (PS3DK_RUN_HOOK_FAULT == 7 && test_log) {
        printf("HOOK-TEST flag observed set after %u ms; returning -1\n", (unsigned)(test_now_ms() - t0));
        fflush(stdout);
    }
}

/* Runs from newlib's _exit, i.e. after atexit handlers, static destructors and stdio cleanup. */
static void hooked_exit(int status)
{
    if (!take_latch()) {
        /* The watchdog owns the outcome (TIMEOUT) and its relaunch; if that relaunch failed, exit plainly.  Nothing
         * else is sent and no second relaunch is tried. */
        wait_watchdog_relaunch_failed(0);
        if (H.orig_exit) H.orig_exit(status);
        for (;;) sysUsleep(1000000);        /* no original exit to fall back to (not reached in practice) */
    }
    /* drain (bounded), stop the sender, then the terminal frame on the same connection */
    for (int waited = 0; waited < DRAIN_MS; waited += 10) {
        int empty;
        pthread_mutex_lock(&H.lock);
        empty = H.count == 0 || H.disconnected;
        pthread_mutex_unlock(&H.lock);
        if (empty) break;
        usleep(10000);
    }
    pthread_mutex_lock(&H.lock);
    H.stop = 1;
    pthread_cond_broadcast(&H.cond);
    pthread_mutex_unlock(&H.lock);
    pthread_join(H.sender, NULL);
    {
        uint8_t p[12];
        prn_put32(p, (uint32_t)status);
        prn_put64(p + 4, H.dropped + H.count);
        if (!H.disconnected) send_tagged(H.fd, H.kr, PRN_DIR_CHILD, H.seq++, PRN_EXIT, p, sizeof p);
    }
    socketclose(H.fd);
    spawn_loader(1);
    if (H.orig_exit) H.orig_exit(status);   /* exitspawn failed: ordinary exit */
}

/* Watchdog: no stdio, no malloc, no hook locks - its own connection and a prepared frame. */
static void watchdog_main(void *arg)
{
    (void)arg;
    uint32_t limit = (PS3DK_RUN_HOOK_FAULT >= 5 && PS3DK_RUN_HOOK_FAULT <= 7) ? 0 : H.timeout_s;
    for (uint32_t s = 0; s < limit; s++) {
        sysUsleep(1000000);
        if (H.latch) sysThreadExit(0);
    }
    if (take_latch()) {
        int fd = prn_connect(H.ip, H.port, 5);
        if (fd >= 0) {
            uint8_t p[12];
            prn_put32(p, H.run_id);
            prn_put64(p + 4, H.dropped + H.count);   /* unlocked read: approximate by design */
            send_tagged(fd, H.kr, PRN_DIR_WDOG, 0, PRN_TIMEOUT, p, sizeof p);
            socketclose(fd);
        }
        if (PS3DK_RUN_HOOK_FAULT != 5 && PS3DK_RUN_HOOK_FAULT != 7)
            spawn_loader(0);   /* prepared target and argv only: no formatting, stat or file I/O here */
        if (PS3DK_RUN_HOOK_FAULT == 7) sysUsleep(WDOG_PUBLISH_DELAY_MS * 1000);   /* test only */
        /* reached only when exitspawn returned (or faults 5/7 skipped it) */
        __atomic_store_n(&H.wdog_spawn_failed, 1, __ATOMIC_RELEASE);
    }
    sysThreadExit(0);
}

static int parse_marker(const char *m)
{
    unsigned id, port, timeout;
    char ip[16];
    if (sscanf(m + strlen(PRN_MARKER), "%u:%15[0-9.]:%u:%u", &id, ip, &port, &timeout) != 4) return -1;
    if (port == 0 || port > 65535) return -1;
    H.run_id = id;
    memcpy(H.ip, ip, sizeof ip);
    H.port = (uint16_t)port;
    H.timeout_s = timeout;
    return 0;
}

/* A start that fails after the marker parsed: stop the watchdog if it exists, tell the host when a connection is up
 * (best effort), then relaunch the loader instead of letting the child's plain exit drop to the XMB.  Returns only
 * when the relaunch itself failed.  Runs before __syscalls is replaced and before the sender exists. */
static int fail_start(uint32_t reason)
{
    if (H.have_watchdog) {
        if (!take_latch()) {
            /* The watchdog fired first: it owns the outcome (TIMEOUT) and the only relaunch.  If that relaunch
             * failed, return -1 without a second outcome or a second relaunch. */
            wait_watchdog_relaunch_failed(1);
            H.have_watchdog = 0;
            if (H.fd >= 0) { socketclose(H.fd); H.fd = -1; }
            return -1;
        }
        u64 rv;
        sysThreadJoin(H.watchdog, &rv);     /* it sees the latch within a second and exits */
        H.have_watchdog = 0;
    }
    if (H.fd >= 0) {
        if (reason) {
            uint8_t p[4];
            prn_put32(p, reason);
            send_tagged(H.fd, H.kr, PRN_DIR_CHILD, H.seq++, PRN_START_FAILED, p, sizeof p);
        }
        socketclose(H.fd);
        H.fd = -1;
    }
    spawn_loader(1);
    return -1;                              /* exitspawn failed */
}

int ps3dk_run_hook_start(int *argc, const char ***argv)
{
    int at = -1;
    /* from index 0: the marker is the last argument the loader passes, wherever lv2 starts the child argv */
    for (int i = 0; i < *argc; i++)
        if ((*argv)[i] && strncmp((*argv)[i], PRN_MARKER, strlen(PRN_MARKER)) == 0) { at = i; break; }
    if (at < 0) return 0;
    if (parse_marker((*argv)[at]) != 0) return -1;
    for (int i = at; i < *argc; i++) (*argv)[i] = (*argv)[i + 1];
    (*argc)--;
    /* Every failure from here on relaunches the loader, so the relaunch is prepared first; read-only afterwards. */
    prepare_spawn();

    {   /* the run key, written by the loader for exactly this run */
        uint8_t rec[36];
        FILE *f = fopen(PRN_RUNKEY_FILE, "rb");
        int ok = f && fread(rec, 1, sizeof rec, f) == sizeof rec && prn_get32(rec) == H.run_id;
        if (f) fclose(f);
        if (!ok) return fail_start(0);      /* no key: no frame the host could authenticate */
        memcpy(H.kr, rec + 4, 32);
        memset(rec, 0, sizeof rec);
    }
    netInitialize();
    H.fd = prn_connect(H.ip, H.port, 10);
    if (H.fd < 0) return fail_start(0);
    {   /* HELLO {run id, child nonce} under Kr (C, seq 0); the host must answer HELLO_OK under Kr (h, seq 0) */
        uint8_t p[4 + 32], t, b[4], tag[PRN_TAG], want[PRN_TAG];
        uint32_t n;
        uint8_t hello_key[32];
        memcpy(hello_key, H.kr, sizeof hello_key);
        if (PS3DK_RUN_HOOK_FAULT == 3) hello_key[0] ^= 1;   /* test only: the host must refuse this HELLO */
        prn_put32(p, H.run_id);
        if (sys_get_random_number(p + 4, 32) != 0 ||
            send_tagged(H.fd, hello_key, PRN_DIR_CHILD, H.seq++, PRN_HELLO, p, sizeof p) != 0 ||
            prn_recv_header(H.fd, &t, &n) != 0 || t != PRN_HELLO_OK || n != 4 ||
            prn_recv_all(H.fd, b, 4) != 0 || prn_recv_all(H.fd, tag, sizeof tag) != 0) return fail_start(PRN_SF_HELLO);
        prn_tag(H.kr, PRN_DIR_HOST_C, 0, PRN_HELLO_OK, b, 4, want);
        if (PS3DK_RUN_HOOK_FAULT == 4) want[0] ^= 1;          /* test only: reject a HELLO_OK the host really sent */
        memset(hello_key, 0, sizeof hello_key);
        if (prn_tag_diff(tag, want) || prn_get32(b) != H.run_id) return fail_start(PRN_SF_HELLO);
    }
    /* The watchdog first: if it cannot run, nothing else has started and nothing is redirected yet.  Joinable, so a
     * later failure can stop it (latch, then join) before relaunching. */
    if (H.timeout_s) {
        if (PS3DK_RUN_HOOK_FAULT == 1 ||
            sysThreadCreate(&H.watchdog, watchdog_main, NULL, 100, 0x4000, THREAD_JOINABLE, (char *)"ps3dk-run-watchdog") != 0)
            return fail_start(PRN_SF_WATCHDOG);
        H.have_watchdog = 1;
    }
    pthread_mutex_init(&H.lock, NULL);
    pthread_cond_init(&H.cond, NULL);
    if (PS3DK_RUN_HOOK_FAULT >= 5 && PS3DK_RUN_HOOK_FAULT <= 7)   /* test only: the watchdog owns the latch first */
        while (!__atomic_load_n(&H.latch, __ATOMIC_ACQUIRE)) sysUsleep(1000);
    if (PS3DK_RUN_HOOK_FAULT == 2 || (PS3DK_RUN_HOOK_FAULT >= 5 && PS3DK_RUN_HOOK_FAULT <= 7) ||
        pthread_create(&H.sender, NULL, sender_main, NULL) != 0)
        return fail_start(PRN_SF_SENDER);
    /* nothing can fail from here on */
    H.orig_write = __syscalls.write_r;
    H.orig_exit = __syscalls.exit;
    __syscalls.write_r = hooked_write;
    __syscalls.exit = hooked_exit;
    return 1;
}
