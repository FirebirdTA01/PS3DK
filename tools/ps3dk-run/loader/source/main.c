/*
 * ps3dk-loader: network SELF launcher (protocol 2: paired key, authenticated
 * frames, switchable file service).  Isolated LAN only: authentication and integrity, no confidentiality.
 *
 * The main thread draws the status console; the network thread reports READY for the run that spawned it back,
 * cleans the staging area, then listens on PRN_LOADER_PORT and serves one upload session at a time.
 */
#include "loader.h"
#include "../../common/prn_net.h"
#include "../../common/ps3dk_run_proto.h"

#include <arpa/inet.h>
#include <cell/dbgfont.h>
#include <cell/gcm.h>
#include <cell/libnetctl.h>
#include <cell/sysmodule.h>
#include <malloc.h>
#include <net/net.h>
#include <netinet/in.h>
#include <pthread.h>
#include <rsx/rsx.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/process.h>
#include <errno.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <cell/sysutil.h>
#include <io/pad.h>
#include <sysutil/video.h>
#include <time.h>
#include <unistd.h>

SYS_PROCESS_PARAM(1001, 0x100000);

#define CB_SIZE     0x100000
#define HOST_SIZE   (32 * 1024 * 1024)
#define LINES       14

static pthread_mutex_t status_lock = PTHREAD_MUTEX_INITIALIZER;
static char lines[LINES][96];
static int nlines;
static char header[96] = "ps3dk-loader: starting network";
static volatile int stop_render, render_stopped;
static int quit;                               /* main thread only (the sysutil callback runs inside CheckCallback) */

/* Shutdown lifetime.  The network worker owns every socket it opens and the network subsystem itself: it alone
 * closes them and tears the network down, and only then publishes net_done.  Every wait it makes is a slice of at
 * most LOADER_SLICE_MS that rechecks the stop request, so a stop ends accept(), an upload in progress or the reject
 * drain within a slice.  stopping and spawning share life_lock: once stopping is set no run is spawned, and a run
 * already spawning replaces the process. */
static pthread_mutex_t life_lock = PTHREAD_MUTEX_INITIALIZER;
static int stopping, spawning;                 /* under life_lock */
static int net_done;                           /* __atomic: set (release) by the worker after its own cleanup */
#define LOADER_STOP_WAIT_MS 8000               /* longest bounded worker wait: a 5 s READY connect plus slices */
#define LOADER_TEST_STOP    "/dev_hdd0/tmp/ps3dk-loader-test-stop"

int loader_stopping(void)
{
    int s;
    pthread_mutex_lock(&life_lock);
    s = stopping;
    pthread_mutex_unlock(&life_lock);
    return s;
}

int loader_begin_spawn(void)
{
    int ok;
    pthread_mutex_lock(&life_lock);
    ok = !stopping;
    if (ok) spawning = 1;
    pthread_mutex_unlock(&life_lock);
    return ok;
}

static int request_stop(void)
{
    int s;
    pthread_mutex_lock(&life_lock);
    stopping = 1;
    s = spawning;
    pthread_mutex_unlock(&life_lock);
    return s;
}
static int g_argc;
static const char **g_argv;


/* BOOT DIAGNOSTIC build (hardware 2026-10-17 21:16: a0cf29d3 hung the console at launch, black screen): every startup
 * step and the first iterations of each loop are appended to /dev_hdd0/tmp/ps3dk-loader-boot.log with lv2 file
 * syscalls (open/write/close per line, nothing buffered), so the last line names the last step reached. */
#include <sys/file.h>
#include <sys/sys_time.h>
#define BOOT_LOG "/dev_hdd0/tmp/ps3dk-loader-boot.log"
static void boot_log(const char *fmt, ...)
{
    static int truncated;
    char line[160];
    s32 fd;
    u64 written;
    va_list ap;
    int n = snprintf(line, sizeof line, "%llu ", (unsigned long long)sys_time_get_system_time());
    va_start(ap, fmt);
    n += vsnprintf(line + n, sizeof line - (size_t)n - 1, fmt, ap);
    va_end(ap);
    if (n < 0 || n >= (int)sizeof line - 1) n = (int)sizeof line - 2;
    line[n++] = 10;
    if (sysLv2FsOpen(BOOT_LOG, SYS_O_WRONLY | SYS_O_CREAT | (truncated ? SYS_O_APPEND : SYS_O_TRUNC), &fd, 0666, NULL, 0) != 0) return;
    truncated = 1;
    sysLv2FsWrite(fd, line, (u64)n, &written);
    sysLv2FsClose(fd);
}

void loader_status(const char *fmt, ...)
{
    va_list ap;
    char buf[96];
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    printf("ps3dk-loader: %s\n", buf);
    pthread_mutex_lock(&status_lock);
    if (nlines == LINES) { memmove(lines[0], lines[1], sizeof lines[0] * (LINES - 1)); nlines--; }
    memcpy(lines[nlines++], buf, sizeof buf);
    pthread_mutex_unlock(&status_lock);
}

uint32_t loader_next_run_id(void)
{
    static uint32_t seq;
    return (uint32_t)time(NULL) * 16u + (seq++ & 15u);
}

void loader_before_spawn(void)
{
    stop_render = 1;
    for (int i = 0; i < 200 && !render_stopped; i++) usleep(5000);
}

void loader_after_failed_spawn(void)
{
    pthread_mutex_lock(&life_lock);
    spawning = 0;
    pthread_mutex_unlock(&life_lock);
    render_stopped = 0;
    stop_render = 0;
}

/* READY for the run that spawned the loader back (--ps3dk-ready=<id>:<ip>:<port>), tagged under that run's key from
 * state/run.key (direction 'R', seq 0); the key file is deleted afterwards.  Returns 1 when a ready marker was seen. */
static int send_ready(void)
{
    int seen = 0;
    for (int i = 0; i < g_argc; i++) {
        unsigned id, port;
        char ip[16];
        uint8_t rec[36], p[4], tag[PRN_TAG];
        FILE *f;
        if (!g_argv[i] || strncmp(g_argv[i], PRN_READY_MARKER, strlen(PRN_READY_MARKER)) != 0) continue;
        if (sscanf(g_argv[i] + strlen(PRN_READY_MARKER), "%u:%15[0-9.]:%u", &id, ip, &port) != 3) continue;
        seen = 1;
        f = fopen(PRN_RUNKEY_FILE, "rb");
        if (!f || fread(rec, 1, sizeof rec, f) != sizeof rec || prn_get32(rec) != id) {
            if (f) fclose(f);
            loader_status("READY for run %u: no run key for it - not sent", id);
            continue;
        }
        fclose(f);
        int fd = prn_connect(ip, (uint16_t)port, 5);
        if (fd >= 0) {
            prn_put32(p, id);
            prn_tag(rec + 4, PRN_DIR_READY, 0, PRN_READY, p, 4, tag);
            if (prn_send_frame(fd, PRN_READY, p, 4) == 0) prn_send_all(fd, tag, sizeof tag);
            socketclose(fd);
            loader_status("READY sent for run %u to %s:%u", id, ip, port);
        } else {
            loader_status("READY for run %u: host %s:%u not reachable", id, ip, port);
        }
        memset(rec, 0, sizeof rec);
    }
    unlink(PRN_RUNKEY_FILE);
    return seen;
}

/* One session at a time: the listener answers BUSY while a session thread runs.  The network worker owns the session
 * thread (joinable, never detached): it reaps a finished one before starting the next, and joins a live one before its
 * own network teardown.  The session thread owns and closes its socket; its receives are stop-aware (prn_stop_check),
 * so a stop ends it within a slice. */
static int session_active;                     /* __atomic: set by the worker, cleared by the session at its end */
struct session_arg { int fd; char ip[16]; };

static void *session_main(void *v)
{
    struct session_arg *a = v;
    loader_session(a->fd, a->ip);              /* returns unless a run was spawned */
    socketclose(a->fd);
    free(a);
    __atomic_store_n(&session_active, 0, __ATOMIC_RELEASE);
    return NULL;
}

static void *net_main(void *arg)
{
    struct sockaddr_in a;
    union CellNetCtlInfo info;
    int ls = -1, one = 1, netctl = 0, returning;
    const char *phase = "init: network";       /* where a stop found the worker (shutdown evidence) */
    pthread_t st;
    int have_st = 0;
    (void)arg;
    prn_stop_check = loader_stopping;          /* every session receive becomes a stop-aware sliced wait */
    prn_stop_idle_ms = PRN_UPLOAD_IDLE_S * 1000;
    boot_log("worker: entered");
    { int nr = netInitialize(); boot_log("worker: netInitialize rc %d", nr); }
    if (loader_stopping()) goto out;
    phase = "init: netctl";
    cellSysmoduleLoadModule(CELL_SYSMODULE_NETCTL);
    netctl = cellNetCtlInit() == 0;
    boot_log("worker: netctl init ok=%d", netctl);
    memset(&info, 0, sizeof info);
    if (cellNetCtlGetInfo(CELL_NET_CTL_INFO_IP_ADDRESS, &info) != 0) strcpy(info.ip_address, "(no IP)");
    snprintf(header, sizeof header, "ps3dk-loader  %s:%d  - trusted LAN only", info.ip_address, PRN_LOADER_PORT);
    {
        uint8_t k[32];
        if (loader_key(k) == 0) loader_status("paired, key fingerprint %s", loader_fingerprint());
        else loader_status("NOT PAIRED: put PS3DKRUN.KEY on a USB stick and press TRIANGLE");
        memset(k, 0, sizeof k);
    }
    for (int i = 0; i < g_argc; i++) loader_status("argv[%d] = %s", i, g_argv[i] ? g_argv[i] : "(NULL)");
    if (loader_stopping()) goto out;
    phase = "init: READY and staging cleanup";
    boot_log("worker: before send_ready");
    returning = send_ready();                      /* bounded: 5 s connect timeout */
    loader_state_init(returning);
    boot_log("worker: before clean_runs");
    loader_clean_runs();
    boot_log("worker: after clean_runs");
    if (loader_stopping()) goto out;
    phase = "serving";

    ls = socket(AF_INET, SOCK_STREAM, 0);
    if (ls >= 0) setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_port = htons(PRN_LOADER_PORT);
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    if (ls < 0 || bind(ls, (struct sockaddr *)&a, sizeof a) != 0 || listen(ls, 4) != 0) {
        loader_status("cannot listen on port %d", PRN_LOADER_PORT);
        goto out;
    }
    loader_status("listening");
    boot_log("worker: listening on fd %d", ls);
    while (!loader_stopping()) {
        struct sockaddr_in peer;
        socklen_t pl = sizeof peer;
        struct timeval tv = { 0, LOADER_SLICE_MS * 1000 };
        fd_set r;
        FD_ZERO(&r);
        FD_SET(ls, &r);
        int n = select(ls + 1, &r, NULL, NULL, &tv);
        { static int k; if (k < 6 || (k % 240) == 0) boot_log("worker: select #%d -> %d errno %d", k, n, n < 0 ? errno : 0); k++; }
        if (n < 0) { usleep(LOADER_SLICE_MS * 1000); continue; }
        if (n == 0 || !FD_ISSET(ls, &r)) continue;
        int fd = accept(ls, (struct sockaddr *)&peer, &pl);
        if (fd < 0) continue;
        if (__atomic_load_n(&session_active, __ATOMIC_ACQUIRE)) {
            prn_send_frame(fd, PRN_BUSY, NULL, 0);
            socketclose(fd);
            loader_status("BUSY: refused a second connection");
            continue;
        }
        if (have_st) {                          /* reap the finished session thread */
            int jr = pthread_join(st, NULL);
            have_st = 0;
            if (jr != 0) { loader_status("session join failed (rc %d); not serving", jr); socketclose(fd); goto out; }
        }
        struct session_arg *sa = malloc(sizeof *sa);
        if (!sa) { socketclose(fd); continue; }
        sa->fd = fd;
        snprintf(sa->ip, sizeof sa->ip, "%s", inet_ntoa(peer.sin_addr));
        __atomic_store_n(&session_active, 1, __ATOMIC_RELEASE);
        if (pthread_create(&st, NULL, session_main, sa) != 0) {
            __atomic_store_n(&session_active, 0, __ATOMIC_RELEASE);
            socketclose(fd);
            free(sa);
            continue;
        }
        have_st = 1;
    }
out:
    /* the session thread first (its receives end within a slice of the stop), then the worker's own cleanup, then (and
       only then) completion is published; a failed join leaves the network up and completion unpublished */
    if (have_st) {
        int jr = pthread_join(st, NULL);
        if (jr != 0) { loader_status("SHUTDOWN: session join failed (rc %d)", jr); return NULL; }
    }
    if (ls >= 0) socketclose(ls);
    if (netctl) cellNetCtlTerm();
    netDeinitialize();
    loader_status("network worker stopped (%s)", phase);
    __atomic_store_n(&net_done, 1, __ATOMIC_RELEASE);
    return NULL;
}

/* ---- display (from samples/dbgfont/hello-ppu-dbgfont-console) ---- */
typedef struct { uint32_t *ptr; uint32_t offset; uint16_t width, height; } display_buffer;
static uint32_t depth_pitch, depth_offset;

static int make_buffer(display_buffer *b, uint16_t w, uint16_t h, int id)
{
    int pitch = (int)(w * sizeof(uint32_t));
    b->ptr = (uint32_t *)rsxMemalign(64, pitch * h);
    if (!b->ptr || cellGcmAddressToOffset(b->ptr, &b->offset) != 0) return 0;
    if (cellGcmSetDisplayBuffer((uint8_t)id, b->offset, pitch, w, h) != 0) return 0;
    b->width = w; b->height = h;
    return 1;
}

static void set_target(CellGcmContextData *ctx, display_buffer *b)
{
    CellGcmSurface sf;
    memset(&sf, 0, sizeof sf);
    sf.colorFormat = GCM_SURFACE_X8R8G8B8; sf.colorTarget = GCM_SURFACE_TARGET_0;
    sf.colorLocation[0] = sf.colorLocation[1] = sf.colorLocation[2] = sf.colorLocation[3] = GCM_LOCATION_RSX;
    sf.colorOffset[0] = b->offset; sf.colorPitch[0] = depth_pitch;
    sf.colorPitch[1] = sf.colorPitch[2] = sf.colorPitch[3] = 64;
    sf.depthFormat = GCM_SURFACE_ZETA_Z16; sf.depthLocation = GCM_LOCATION_RSX;
    sf.depthOffset = depth_offset; sf.depthPitch = depth_pitch;
    sf.type = GCM_SURFACE_TYPE_LINEAR; sf.antiAlias = GCM_SURFACE_CENTER_1;
    sf.width = b->width; sf.height = b->height;
    cellGcmSetSurface(ctx, &sf);
    float s[4] = { b->width / 2.0f, -(float)b->height / 2.0f, 0.5f, 0.0f };
    float o[4] = { b->width / 2.0f, b->height / 2.0f, 0.5f, 0.0f };
    cellGcmSetViewport(ctx, 0, 0, b->width, b->height, 0.0f, 1.0f, s, o);
    cellGcmSetScissor(ctx, 0, 0, b->width, b->height);
}

static void on_sysutil(uint64_t status, uint64_t param, void *user)
{
    (void)param; (void)user;
    if (status == CELL_SYSUTIL_REQUEST_EXITGAME) quit = 1;
}

int main(int argc, const char *argv[])
{
    videoState state; videoConfiguration vcfg; videoResolution res;
    display_buffer bufs[2];
    pthread_t net;
    g_argc = argc; g_argv = argv;

    boot_log("main: entered argc %d", argc);
    void *host = memalign(1024 * 1024, HOST_SIZE);
    if (!host || cellGcmInit(CB_SIZE, HOST_SIZE, host) != 0) return 1;
    if (videoGetState(0, 0, &state) != 0 || state.state != 0) return 1;
    if (videoGetResolution(state.displayMode.resolution, &res) != 0) return 1;
    memset(&vcfg, 0, sizeof vcfg);
    vcfg.resolution = state.displayMode.resolution;
    vcfg.format = VIDEO_BUFFER_FORMAT_XRGB;
    vcfg.pitch = res.width * sizeof(uint32_t);
    vcfg.aspect = state.displayMode.aspect;
    boot_log("main: gcm init done");
    if (videoConfigure(0, &vcfg, NULL, 0) != 0) return 1;
    boot_log("main: video configured %dx%d", res.width, res.height);
    cellGcmSetFlipMode(GCM_FLIP_VSYNC);
    depth_pitch = res.width * sizeof(uint32_t);
    cellGcmAddressToOffset(rsxMemalign(64, res.height * depth_pitch * 2), &depth_offset);
    cellGcmResetFlipStatus();
    for (int i = 0; i < 2; i++) if (!make_buffer(&bufs[i], res.width, res.height, i)) return 1;

    size_t dbg_size = CELL_DBGFONT_FRAGMENT_SIZE + CELL_DBGFONT_TEXTURE_SIZE + 9000 * CELL_DBGFONT_VERTEX_SIZE;
    void *dbg = rsxMemalign(128, dbg_size);
    CellDbgFontConfigGcm cfg;
    memset(&cfg, 0, sizeof cfg);
    cfg.localBufAddr = (sys_addr_t)(uintptr_t)dbg;
    cfg.localBufSize = (uint32_t)dbg_size;
    cfg.option = CELL_DBGFONT_VERTEX_LOCAL | CELL_DBGFONT_TEXTURE_LOCAL;
    if (cellDbgFontInitGcm(&cfg) != 0) return 1;
    boot_log("main: dbgfont init done");
    cellSysutilRegisterCallback(0, on_sysutil, NULL);
    ioPadInit(7);
    int net_rc = pthread_create(&net, NULL, net_main, NULL);
    boot_log("main: worker create rc %d", net_rc);
    if (net_rc != 0) loader_status("network worker not started (rc %d)", net_rc);

    CellGcmContextData *ctx = CELL_GCM_CURRENT;
    int cur = 0;
    unsigned frame = 0;
    unsigned prev_tri = 0, prev_sq = 0, prev_ci = 0;
    while (!quit) {
        /* local test trigger (RPCS3 shutdown tests): the file stands in for the XMB's quit request */
        if (frame < 4 || (frame % 600) == 0) boot_log("main: frame %u", frame);
        if ((frame++ & 31) == 0) {             /* from the first frame: a stop can land during worker init */
            if (frame < 3) boot_log("main: trigger stat");
            struct stat st;
            if (stat(LOADER_TEST_STOP, &st) == 0) { unlink(LOADER_TEST_STOP); loader_status("test stop requested"); quit = 1; break; }
        }
        {   /* pad 0, edge-triggered: TRIANGLE import key, SQUARE revoke key, CIRCLE toggle the file service */
            padInfo pi;
            padData pd;
            if (ioPadGetInfo(&pi) == 0 && pi.status[0] && ioPadGetData(0, &pd) == 0 && pd.len > 0) {
                if (pd.BTN_TRIANGLE && !prev_tri) loader_import_key();
                if (pd.BTN_SQUARE && !prev_sq) loader_revoke_key();
                if (pd.BTN_CIRCLE && !prev_ci) loader_set_files(!files_on, "loader screen");
                prev_tri = pd.BTN_TRIANGLE; prev_sq = pd.BTN_SQUARE; prev_ci = pd.BTN_CIRCLE;
            }
        }
        if (stop_render) {                       /* a spawn is in progress: GPU idle until it fails or replaces us */
            if (!render_stopped) { cellGcmFinish(ctx, 1); render_stopped = 1; }
            usleep(10000);
            continue;
        }
        set_target(ctx, &bufs[cur]);
        cellGcmSetClearColor(ctx, 0xff102030);
        cellGcmSetClearSurface(ctx, GCM_CLEAR_R | GCM_CLEAR_G | GCM_CLEAR_B | GCM_CLEAR_A);
        cellDbgFontPrintf(0.05f, 0.06f, 1.0f, 0xff40ff40u, "%s", header);
        cellDbgFontPrintf(0.05f, 0.10f, 0.8f, 0xffffff40u, "key %s   file service %s   [TRIANGLE import key  SQUARE revoke  CIRCLE files]",
                          loader_fingerprint(), files_on ? "ON" : "OFF");
        pthread_mutex_lock(&status_lock);
        for (int i = 0; i < nlines; i++)
            cellDbgFontPrintf(0.05f, 0.16f + 0.05f * (float)i, 0.8f, 0xffffffffu, "%s", lines[i]);
        pthread_mutex_unlock(&status_lock);
        cellDbgFontDrawGcm();
        cellGcmFlush(ctx);
        if (cellGcmSetFlip(ctx, (uint8_t)cur) == 0) { cellGcmFlush(ctx); cellGcmSetWaitFlip(ctx); }
        if (frame < 4) boot_log("main: flip queued");
        cur ^= 1;
        while (cellGcmGetFlipStatus() != 0) usleep(200);
        cellGcmResetFlipStatus();
        cellSysutilCheckCallback();
    }
    cellGcmFinish(ctx, 1);
    cellDbgFontExitGcm();
    /* Quit (hardware 2026-10-17: an XMB quit hung the console while the old worker sat in accept() with the network
       up; suspected, not proven).  Request the stop and wait for the worker to finish its own cleanup; main never
       tears the network down.  A worker that does not stop in time is reported, and nothing is torn down under it. */
    if (net_rc == 0) {
        int spawn_in_progress = request_stop(), waited = 0;
        while (!__atomic_load_n(&net_done, __ATOMIC_ACQUIRE) && waited < LOADER_STOP_WAIT_MS) { usleep(10000); waited += 10; }
        if (!__atomic_load_n(&net_done, __ATOMIC_ACQUIRE)) {
            loader_status("SHUTDOWN FAILED: network worker still running after %d ms%s", waited,
                          spawn_in_progress ? " (a run is being spawned)" : "");
            return 2;
        }
        int jr = pthread_join(net, NULL);
        loader_status("shutdown: worker stopped after %d ms, join rc %d", waited, jr);
        if (jr != 0) return 2;
    }
    cellSysutilUnregisterCallback(0);
    loader_status("shutdown complete");
    return 0;
}
