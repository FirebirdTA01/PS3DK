/* lv2dbg hardware probe (debugger design study, phase 1).
 *
 * Asks whether the lv2dbg calls an in-app debug stub needs answer on the
 * console it runs on: thread ids, reading another thread's register context,
 * PPU exception-handler registration and the DABR data breakpoint.  Every
 * return code is logged; nothing here faults on purpose.  The probe never
 * raises an exception and never touches the DABR-watched variable, so a
 * registered handler is never expected to run.
 *
 * Each line goes to stdout and to a log under /dev_hdd0/tmp.  An existing log
 * is never overwritten: the probe takes the first free name of
 * lv2dbg-probe.log, lv2dbg-probe-1.log, ...  A "CALL" line is written and
 * flushed to the log before each call, so a hang is placed on its call; when
 * that line cannot be made durable, no further lv2dbg call is made.
 *
 * Exit status is a set of bits (0 = the observation sequence completed;
 * steps skipped for a missing dependency are recorded as SKIP lines):
 *   0x02  CELL_SYSMODULE_LV2DBG did not load (no lv2dbg call is made)
 *   0x04  the worker thread could not be created or never became ready
 *   0x08  the log could not be created, written, flushed or closed
 *   0x10  cleanup incomplete: a failed unregister, finalize, DABR restore,
 *         thread join or module unload
 * Return codes from an lv2dbg call that is unavailable are observations, not
 * failures.
 *
 * PROBE_SIM (test builds only) replaces the lv2dbg calls and the log with
 * simulations, to exercise the failure paths; see README.md.
 */
#include <errno.h>
#include <inttypes.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include <cell/sysmodule.h>
#include <sys/dbg.h>
#include <sys/ppu_thread.h>
#include <sys/process.h>
#include <sys/timer.h>

#ifdef PROBE_RUN_HOOK
#include "ps3dk_run_hook.h"
#endif

SYS_PROCESS_PARAM(1001, 0x10000);

#define LOG_DIR "/dev_hdd0/tmp"
#define LOG_BASE "lv2dbg-probe"

#define ST_MODULE   0x02
#define ST_WORKER   0x04
#define ST_LOG      0x08
#define ST_CLEANUP  0x10

/* PROBE_SIM values (preprocessor constants: they select code with #if). */
#define SIM_NONE         0
#define SIM_OK           1
#define SIM_LOG_OPEN     2
#define SIM_LOG_WRITE    3
#define SIM_DABR_ACTIVE  4
#define SIM_CLEANUP_FAIL 5
#define SIM_LOG_EXISTS   6
#ifndef PROBE_SIM
#define PROBE_SIM SIM_NONE
#endif

static FILE *g_log;
static char g_log_path[64];
static int g_status;
static int g_log_failed;
static unsigned g_log_writes;

static void mark(int bit) { g_status |= bit; }

static void log_failure(const char *what)
{
    if (!g_log_failed)
        printf("LOGFAIL %s errno=%d path=%s\n", what, errno, g_log_path);
    g_log_failed = 1;
    mark(ST_LOG);
}

/* Writes one line to stdout and the log.  Returns 1 when the log copy is
   written and flushed, 0 otherwise. */
static int say(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    fflush(stdout);
    if (!g_log || g_log_failed) return 0;
    va_start(ap, fmt);
    int w = vfprintf(g_log, fmt, ap);
    va_end(ap);
    int f = fflush(g_log);
    g_log_writes++;
    if (PROBE_SIM == SIM_LOG_WRITE && g_log_writes > 6) w = -1;
    if (w < 0) { log_failure("write"); return 0; }
    if (f != 0) { log_failure("flush"); return 0; }
    return 1;
}

/* Announces a call; returns 0 (and the call must not be made) when the
   announcement did not reach the log. */
static int call(const char *name)
{
    if (!say("CALL %s\n", name)) {
        say("SKIP %s: CALL line not durable\n", name);
        return 0;
    }
    return 1;
}

/* Announces a cleanup call, which is made whether or not the announcement
   reached the log. */
static void cleanup_call(const char *name)
{
    if (!say("CALL %s\n", name))
        say("NOTE %s: CALL line not durable; cleanup attempted\n", name);
}

static void rc_line(const char *name, int rc)
{
    say("RC %s 0x%08x\n", name, (unsigned)rc);
}

static void skip(const char *name, const char *why)
{
    say("SKIP %s: %s\n", name, why);
}

static int open_log(void)
{
    for (int n = 0; n < 100; n++) {
        if (n == 0) snprintf(g_log_path, sizeof g_log_path, LOG_DIR "/" LOG_BASE ".log");
        else snprintf(g_log_path, sizeof g_log_path, LOG_DIR "/" LOG_BASE "-%d.log", n);
        /* An existing name is skipped before the exclusive open, so that
           preservation does not rest on how the file system treats
           O_CREAT|O_EXCL|O_TRUNC (what "wx" passes). */
        struct stat st;
        if (PROBE_SIM != SIM_LOG_OPEN && stat(g_log_path, &st) == 0) continue;
        errno = 0;
        FILE *f = PROBE_SIM == SIM_LOG_OPEN ? NULL : fopen(g_log_path, "wx");
        if (f) { g_log = f; return 1; }
        if (PROBE_SIM == SIM_LOG_OPEN) { errno = EACCES; break; }
        if (errno != EEXIST) break;
    }
    log_failure("open");
    return 0;
}

/* ---- lv2dbg calls, or their simulation ---------------------------------- */

#if PROBE_SIM == SIM_OK || PROBE_SIM == SIM_DABR_ACTIVE || PROBE_SIM == SIM_CLEANUP_FAIL
static uint64_t sim_dabr_addr, sim_dabr_ctrl;
static int sim_rc(const char *what)
{
    if (PROBE_SIM == SIM_CLEANUP_FAIL &&
        (!strcmp(what, "unregister") || !strcmp(what, "finalize") || !strcmp(what, "dabr-restore")))
        return (int)0x80010002;
    return 0;
}
static int sim_get_ppu_thread_ids(sys_ppu_thread_t *ids, uint64_t *n, uint64_t *all)
{ ids[0] = 0x01000000; *n = 1; *all = 1; return 0; }
static int sim_read_ppu_thread_context(sys_ppu_thread_t id, sys_dbg_ppu_thread_context_t *c)
{ (void)id; memset(c, 0, sizeof *c); c->pc = 0x10000; return 0; }
static int sim_get_ppu_thread_name(sys_ppu_thread_t id, char *name)
{ (void)id; strcpy(name, "probe-worker"); return 0; }
static int sim_get_ppu_thread_status(sys_ppu_thread_t id, sys_dbg_ppu_thread_status_t *s)
{ (void)id; *s = PPU_THREAD_STATUS_SLEEP; return 0; }
static int sim_initialize_ppu_exception_handler(int prio) { (void)prio; return sim_rc("init"); }
static int sim_register_ppu_exception_handler(dbg_exception_handler_t cb, uint64_t f)
{ (void)cb; (void)f; return sim_rc("register"); }
static int sim_unregister_ppu_exception_handler(void) { return sim_rc("unregister"); }
static int sim_finalize_ppu_exception_handler(void) { return sim_rc("finalize"); }
static int sim_get_address_from_dabr(uint64_t *a, uint64_t *c)
{
    static int first = 1;
    if (first && PROBE_SIM == SIM_DABR_ACTIVE) {
        sim_dabr_addr = 0x10001000; sim_dabr_ctrl = SYS_DBG_DABR_CTRL_READ;
    }
    first = 0;
    *a = sim_dabr_addr; *c = sim_dabr_ctrl;
    return 0;
}
static int sim_set_address_to_dabr(uint64_t a, uint64_t c)
{
    static int calls;
    /* The second set is the probe's restore. */
    if (++calls == 2 && sim_rc("dabr-restore")) return sim_rc("dabr-restore");
    sim_dabr_addr = a; sim_dabr_ctrl = c;
    return 0;
}
#define DBG(name) sim_##name
#else
#define DBG(name) sys_dbg_##name
#endif

/* ---- the worker --------------------------------------------------------- */

static atomic_int g_worker_ready;
static atomic_int g_worker_stop;
static atomic_ullong g_worker_spins;

static void worker(uint64_t arg)
{
    (void)arg;
    atomic_store(&g_worker_ready, 1);
    while (!atomic_load(&g_worker_stop)) {
        atomic_fetch_add(&g_worker_spins, 1);
        sys_timer_usleep(1000);
    }
    sys_ppu_thread_exit(0);
}

static void handler(uint64_t exception_type, sys_ppu_thread_t id, uint64_t dar)
{
    say("HANDLER type=0x%016" PRIx64 " thread=0x%08x dar=0x%016" PRIx64 "\n",
        exception_type, (unsigned)id, dar);
}

/* The DABR target.  Nothing reads or writes it while a watch is armed. */
static volatile uint64_t g_watched;

/* ---- probe steps -------------------------------------------------------- */

static void probe_threads(sys_ppu_thread_t self)
{
    sys_ppu_thread_t ids[16];
    uint64_t ids_num = 16, all_ids_num = 0;
    memset(ids, 0, sizeof ids);
    if (!call("sys_dbg_get_ppu_thread_ids")) return;
    int rc = DBG(get_ppu_thread_ids)(ids, &ids_num, &all_ids_num);
    rc_line("sys_dbg_get_ppu_thread_ids", rc);
    if (rc == 0) {
        say("VAL ids_num=%" PRIu64 " all_ids_num=%" PRIu64 "\n", ids_num, all_ids_num);
        for (uint64_t i = 0; i < ids_num && i < 16; i++)
            say("VAL ppu[%" PRIu64 "]=0x%08x%s\n", i, (unsigned)ids[i],
                ids[i] == self ? " (self)" : "");
    }
}

static void probe_context(sys_ppu_thread_t target, const char *label)
{
    static sys_dbg_ppu_thread_context_t ctx;
    char name[64];
    memset(&ctx, 0, sizeof ctx);
    snprintf(name, sizeof name, "sys_dbg_read_ppu_thread_context(%s)", label);
    if (!call(name)) return;
    int rc = DBG(read_ppu_thread_context)(target, &ctx);
    rc_line(name, rc);
    if (rc == 0)
        say("VAL %s pc=0x%016" PRIx64 " lr=0x%016" PRIx64 " r1=0x%016" PRIx64
            " r2=0x%016" PRIx64 "\n", label, ctx.pc, ctx.lr, ctx.gpr[1], ctx.gpr[2]);
}

static void probe_thread_info(sys_ppu_thread_t target)
{
    char tname[32];
    sys_dbg_ppu_thread_status_t status = PPU_THREAD_STATUS_UNKNOWN;
    memset(tname, 0, sizeof tname);
    if (!call("sys_dbg_get_ppu_thread_name(worker)")) return;
    int rc = DBG(get_ppu_thread_name)(target, tname);
    rc_line("sys_dbg_get_ppu_thread_name(worker)", rc);
    if (rc == 0) {
        tname[sizeof tname - 1] = '\0';
        say("VAL worker name=[%s]\n", tname);
    }
    if (!call("sys_dbg_get_ppu_thread_status(worker)")) return;
    rc = DBG(get_ppu_thread_status)(target, &status);
    rc_line("sys_dbg_get_ppu_thread_status(worker)", rc);
    if (rc == 0) say("VAL worker status=%d\n", (int)status);
}

static void probe_exception_handler(void)
{
    if (!call("sys_dbg_initialize_ppu_exception_handler")) return;
    int rc = DBG(initialize_ppu_exception_handler)(1000);
    rc_line("sys_dbg_initialize_ppu_exception_handler", rc);
    if (rc != 0) {
        skip("sys_dbg_register_ppu_exception_handler", "initialize failed");
        skip("sys_dbg_finalize_ppu_exception_handler", "initialize failed");
        return;
    }
    int registered = 0;
    if (call("sys_dbg_register_ppu_exception_handler")) {
        rc = DBG(register_ppu_exception_handler)(handler, 0);
        rc_line("sys_dbg_register_ppu_exception_handler", rc);
        registered = rc == 0;
    }
    if (registered) {
        cleanup_call("sys_dbg_unregister_ppu_exception_handler");
        rc = DBG(unregister_ppu_exception_handler)();
        rc_line("sys_dbg_unregister_ppu_exception_handler", rc);
        if (rc != 0) { mark(ST_CLEANUP); say("CLEANUP-INCOMPLETE unregister\n"); }
    } else {
        skip("sys_dbg_unregister_ppu_exception_handler", "not registered");
    }
    cleanup_call("sys_dbg_finalize_ppu_exception_handler");
    rc = DBG(finalize_ppu_exception_handler)();
    rc_line("sys_dbg_finalize_ppu_exception_handler", rc);
    if (rc != 0) { mark(ST_CLEANUP); say("CLEANUP-INCOMPLETE finalize\n"); }
}

static void probe_dabr(void)
{
    uint64_t addr = 0, ctrl = 0, prior_addr, prior_ctrl;
    uint64_t target = (uint64_t)(uintptr_t)&g_watched;
    if (!call("sys_dbg_get_address_from_dabr(initial)")) return;
    int rc = DBG(get_address_from_dabr)(&addr, &ctrl);
    rc_line("sys_dbg_get_address_from_dabr(initial)", rc);
    if (rc != 0) {
        skip("sys_dbg_set_address_to_dabr", "initial read failed");
        return;
    }
    say("VAL dabr initial addr=0x%016" PRIx64 " ctrl=0x%016" PRIx64 "\n", addr, ctrl);
    if ((ctrl & (SYS_DBG_DABR_CTRL_READ | SYS_DBG_DABR_CTRL_WRITE)) != 0) {
        skip("sys_dbg_set_address_to_dabr", "a watch is already active");
        return;
    }
    prior_addr = addr;
    prior_ctrl = ctrl;
    say("VAL dabr target=0x%016" PRIx64 "\n", target);
    if (!call("sys_dbg_set_address_to_dabr(write)")) return;
    rc = DBG(set_address_to_dabr)(target, SYS_DBG_DABR_CTRL_WRITE);
    rc_line("sys_dbg_set_address_to_dabr(write)", rc);
    if (rc == 0 && call("sys_dbg_get_address_from_dabr(armed)")) {
        addr = ctrl = 0;
        int r = DBG(get_address_from_dabr)(&addr, &ctrl);
        rc_line("sys_dbg_get_address_from_dabr(armed)", r);
        if (r == 0)
            say("VAL dabr armed addr=0x%016" PRIx64 " ctrl=0x%016" PRIx64 "\n", addr, ctrl);
    }
    /* Restore the exact prior state and read it back, even when the set
       reported failure or a CALL line was not durable. */
    cleanup_call("sys_dbg_set_address_to_dabr(restore)");
    int restore_rc = DBG(set_address_to_dabr)(prior_addr, prior_ctrl);
    rc_line("sys_dbg_set_address_to_dabr(restore)", restore_rc);
    cleanup_call("sys_dbg_get_address_from_dabr(after)");
    addr = ctrl = 0;
    int after_rc = DBG(get_address_from_dabr)(&addr, &ctrl);
    rc_line("sys_dbg_get_address_from_dabr(after)", after_rc);
    if (after_rc == 0)
        say("VAL dabr after addr=0x%016" PRIx64 " ctrl=0x%016" PRIx64 "\n", addr, ctrl);
    if (restore_rc != 0 || after_rc != 0 || addr != prior_addr || ctrl != prior_ctrl) {
        mark(ST_CLEANUP);
        say("CLEANUP-INCOMPLETE dabr-restore\n");
    }
}

int main(int argc, const char *argv[])
{
#ifdef PROBE_RUN_HOOK
    int hooked = ps3dk_run_hook_start(&argc, &argv);
#else
    int hooked = 0;
    (void)argc;
    (void)argv;
#endif
#if PROBE_SIM == SIM_LOG_EXISTS
    /* An earlier log at the first name: it must survive.  Also records what
       a direct "wx" open of an existing file does here. */
    static const char marker[] = "EARLIER-LOG-MARKER\n";
    const char *first = LOG_DIR "/" LOG_BASE ".log";
    FILE *pre = fopen(first, "w");
    if (pre) { fputs(marker, pre); fclose(pre); }
    errno = 0;
    FILE *x = fopen(first, "wx");
    printf("SIMCHECK direct-wx-on-existing %s errno=%d\n", x ? "opened" : "refused", errno);
    if (x) fclose(x);
    pre = fopen(first, "w");
    if (pre) { fputs(marker, pre); fclose(pre); }
#endif
    int have_log = open_log();
#if PROBE_SIM == SIM_LOG_EXISTS
    {
        char buf[64] = {0};
        FILE *chk = fopen(first, "r");
        size_t n = chk ? fread(buf, 1, sizeof buf - 1, chk) : 0;
        if (chk) fclose(chk);
        say("SIMCHECK earlier-log %s path-used=%s\n",
            (n == sizeof marker - 1 && !memcmp(buf, marker, n)) ? "intact" : "CHANGED", g_log_path);
    }
#endif
    say("LV2DBG_PROBE_BEGIN hook=%d log=%s abi=%u sim=%d\n", hooked,
        have_log ? g_log_path : "none", (unsigned)(sizeof(void *) * 8), PROBE_SIM);

    sys_ppu_thread_t self = 0;
    rc_line("sys_ppu_thread_get_id", sys_ppu_thread_get_id(&self));
    say("VAL self=0x%08x\n", (unsigned)self);

    int loaded_here = 0;
    if (!call("cellSysmoduleLoadModule(LV2DBG)")) goto done;
    int load_rc = cellSysmoduleLoadModule(CELL_SYSMODULE_LV2DBG);
    rc_line("cellSysmoduleLoadModule(LV2DBG)", load_rc);
    if (load_rc == 0) {
        loaded_here = 1;
    } else if (load_rc != (int)CELL_SYSMODULE_LOADED) {
        mark(ST_MODULE);
        goto done;
    }

    probe_threads(self);

    sys_ppu_thread_t worker_id = 0;
    int have_worker = 0;
    if (call("sys_ppu_thread_create(worker)")) {
        int create_rc = sys_ppu_thread_create(&worker_id, worker, 0, 1500, 0x4000,
                                              SYS_PPU_THREAD_CREATE_JOINABLE, "probe-worker");
        rc_line("sys_ppu_thread_create(worker)", create_rc);
        have_worker = create_rc == 0;
        if (!have_worker) mark(ST_WORKER);
    }
    if (have_worker) {
        /* Bounded wait (2 s) for the worker to report ready and spin once. */
        int ready = 0;
        for (int i = 0; i < 2000 && !ready; i++) {
            ready = atomic_load(&g_worker_ready) && atomic_load(&g_worker_spins) > 0;
            if (!ready) sys_timer_usleep(1000);
        }
        say("VAL worker=0x%08x ready=%d spins=%" PRIu64 "\n", (unsigned)worker_id, ready,
            (uint64_t)atomic_load(&g_worker_spins));
        if (ready) {
            probe_threads(self);
            probe_context(worker_id, "worker");
            probe_context(self, "self");
            probe_thread_info(worker_id);
        } else {
            mark(ST_WORKER);
            skip("worker context steps", "worker not ready within 2 s");
        }
    } else {
        skip("worker context steps", "no worker");
    }

    probe_exception_handler();
    probe_dabr();

    if (have_worker) {
        uint64_t retval = 0;
        atomic_store(&g_worker_stop, 1);
        cleanup_call("sys_ppu_thread_join(worker)");
        int join_rc = sys_ppu_thread_join(worker_id, &retval);
        rc_line("sys_ppu_thread_join(worker)", join_rc);
        if (join_rc != 0) { mark(ST_CLEANUP); say("CLEANUP-INCOMPLETE join\n"); }
    }

    if (loaded_here) {
        cleanup_call("cellSysmoduleUnloadModule(LV2DBG)");
        int unload_rc = cellSysmoduleUnloadModule(CELL_SYSMODULE_LV2DBG);
        rc_line("cellSysmoduleUnloadModule(LV2DBG)", unload_rc);
        if (unload_rc != 0) { mark(ST_CLEANUP); say("CLEANUP-INCOMPLETE unload\n"); }
    } else {
        skip("cellSysmoduleUnloadModule(LV2DBG)", "module was not loaded by the probe");
    }

done:
    say("LV2DBG_PROBE_END status=0x%02x\n", g_status);
    if (g_log) {
        errno = 0;
        if (fclose(g_log) != 0) log_failure("close");
        g_log = NULL;
    }
    /* stdout only: the log is closed. */
    printf("LV2DBG_PROBE_DONE status=0x%02x log=%s\n", g_status,
           (have_log && !g_log_failed) ? "ok" : "failed");
    fflush(stdout);
    return g_status;
}
