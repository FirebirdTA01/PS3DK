/* lv2dbg hardware probe (debugger design study, phase 1).
 *
 * Asks whether the lv2dbg calls an in-app debug stub needs answer on the
 * console it runs on: thread ids, reading another thread's register context,
 * PPU exception-handler registration and the DABR data breakpoint.  Every
 * return code is logged; nothing here faults on purpose.  The probe never
 * raises an exception and never touches the DABR-watched variable, so a
 * registered handler is never expected to run.
 *
 * Each line goes to stdout and, as a backup, to LOG_PATH.  A "CALL" line is
 * written and flushed before each call, so a hang is placed on its call.
 *
 * Exit status: 0 the probe ran every step (whatever the return codes),
 * 2 CELL_SYSMODULE_LV2DBG did not load (no lv2dbg call is made), 3 the
 * worker thread could not be created (the context steps are skipped).
 */
#include <inttypes.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <cell/sysmodule.h>
#include <sys/dbg.h>
#include <sys/ppu_thread.h>
#include <sys/process.h>
#include <sys/timer.h>

#ifdef PROBE_RUN_HOOK
#include "ps3dk_run_hook.h"
#endif

SYS_PROCESS_PARAM(1001, 0x10000);

#define LOG_PATH "/dev_hdd0/tmp/lv2dbg-probe.log"

static FILE *g_log;

static void say(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    fflush(stdout);
    if (g_log) {
        va_start(ap, fmt);
        vfprintf(g_log, fmt, ap);
        va_end(ap);
        fflush(g_log);
    }
}

static void call(const char *name)
{
    say("CALL %s\n", name);
}

static void rc_line(const char *name, int rc)
{
    say("RC %s 0x%08x\n", name, (unsigned)rc);
}

static volatile int g_worker_stop;
static volatile uint64_t g_worker_spins;
/* The DABR target.  Nothing reads or writes it while a watch is armed. */
static volatile uint64_t g_watched;

static void worker(uint64_t arg)
{
    (void)arg;
    while (!g_worker_stop) {
        g_worker_spins++;
        sys_timer_usleep(1000);
    }
    sys_ppu_thread_exit(0);
}

static void handler(uint64_t exception_type, sys_ppu_thread_t id, uint64_t dar)
{
    say("HANDLER type=0x%016" PRIx64 " thread=0x%08x dar=0x%016" PRIx64 "\n",
        exception_type, (unsigned)id, dar);
}

static void probe_threads(sys_ppu_thread_t self)
{
    sys_ppu_thread_t ids[16];
    uint64_t ids_num = 16, all_ids_num = 0;
    memset(ids, 0, sizeof ids);
    call("sys_dbg_get_ppu_thread_ids");
    int rc = sys_dbg_get_ppu_thread_ids(ids, &ids_num, &all_ids_num);
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
    call(name);
    int rc = sys_dbg_read_ppu_thread_context(target, &ctx);
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
    call("sys_dbg_get_ppu_thread_name(worker)");
    int rc = sys_dbg_get_ppu_thread_name(target, tname);
    rc_line("sys_dbg_get_ppu_thread_name(worker)", rc);
    if (rc == 0) {
        tname[sizeof tname - 1] = '\0';
        say("VAL worker name=[%s]\n", tname);
    }
    call("sys_dbg_get_ppu_thread_status(worker)");
    rc = sys_dbg_get_ppu_thread_status(target, &status);
    rc_line("sys_dbg_get_ppu_thread_status(worker)", rc);
    if (rc == 0) say("VAL worker status=%d\n", (int)status);
}

static void probe_exception_handler(void)
{
    call("sys_dbg_initialize_ppu_exception_handler");
    int init_rc = sys_dbg_initialize_ppu_exception_handler(1000);
    rc_line("sys_dbg_initialize_ppu_exception_handler", init_rc);
    if (init_rc != 0) return;
    call("sys_dbg_register_ppu_exception_handler");
    int rc = sys_dbg_register_ppu_exception_handler(handler, 0);
    rc_line("sys_dbg_register_ppu_exception_handler", rc);
    if (rc == 0) {
        call("sys_dbg_unregister_ppu_exception_handler");
        rc_line("sys_dbg_unregister_ppu_exception_handler",
                sys_dbg_unregister_ppu_exception_handler());
    }
    call("sys_dbg_finalize_ppu_exception_handler");
    rc_line("sys_dbg_finalize_ppu_exception_handler",
            sys_dbg_finalize_ppu_exception_handler());
}

static void probe_dabr(void)
{
    uint64_t addr = 0, ctrl = 0;
    uint64_t target = (uint64_t)(uintptr_t)&g_watched;
    call("sys_dbg_get_address_from_dabr(initial)");
    int rc = sys_dbg_get_address_from_dabr(&addr, &ctrl);
    rc_line("sys_dbg_get_address_from_dabr(initial)", rc);
    if (rc == 0)
        say("VAL dabr initial addr=0x%016" PRIx64 " ctrl=0x%016" PRIx64 "\n", addr, ctrl);
    say("VAL dabr target=0x%016" PRIx64 "\n", target);
    call("sys_dbg_set_address_to_dabr(write)");
    int set_rc = sys_dbg_set_address_to_dabr(target, SYS_DBG_DABR_CTRL_WRITE);
    rc_line("sys_dbg_set_address_to_dabr(write)", set_rc);
    addr = ctrl = 0;
    call("sys_dbg_get_address_from_dabr(armed)");
    rc = sys_dbg_get_address_from_dabr(&addr, &ctrl);
    rc_line("sys_dbg_get_address_from_dabr(armed)", rc);
    if (rc == 0)
        say("VAL dabr armed addr=0x%016" PRIx64 " ctrl=0x%016" PRIx64 "\n", addr, ctrl);
    call("sys_dbg_set_address_to_dabr(clear)");
    rc_line("sys_dbg_set_address_to_dabr(clear)",
            sys_dbg_set_address_to_dabr(0, SYS_DBG_DABR_CTRL_CLEAR));
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
    int status = 0;
    g_log = fopen(LOG_PATH, "w");
    say("LV2DBG_PROBE_BEGIN hook=%d log=%s abi=%u\n", hooked, g_log ? "yes" : "no",
        (unsigned)(sizeof(void *) * 8));

    sys_ppu_thread_t self = 0;
    call("sys_ppu_thread_get_id");
    rc_line("sys_ppu_thread_get_id", sys_ppu_thread_get_id(&self));
    say("VAL self=0x%08x\n", (unsigned)self);

    call("cellSysmoduleLoadModule(LV2DBG)");
    int load_rc = cellSysmoduleLoadModule(CELL_SYSMODULE_LV2DBG);
    rc_line("cellSysmoduleLoadModule(LV2DBG)", load_rc);
    if (load_rc != 0 && load_rc != (int)CELL_SYSMODULE_LOADED) {
        status = 2;
        goto done;
    }

    probe_threads(self);

    sys_ppu_thread_t worker_id = 0;
    call("sys_ppu_thread_create(worker)");
    int create_rc = sys_ppu_thread_create(&worker_id, worker, 0, 1500, 0x4000,
                                          SYS_PPU_THREAD_CREATE_JOINABLE, "probe-worker");
    rc_line("sys_ppu_thread_create(worker)", create_rc);
    if (create_rc == 0) {
        sys_timer_usleep(20000);
        say("VAL worker=0x%08x spins=%" PRIu64 "\n", (unsigned)worker_id, g_worker_spins);
        probe_threads(self);
        probe_context(worker_id, "worker");
        probe_context(self, "self");
        probe_thread_info(worker_id);
    } else {
        status = 3;
    }

    probe_exception_handler();
    probe_dabr();

    if (create_rc == 0) {
        uint64_t retval = 0;
        g_worker_stop = 1;
        call("sys_ppu_thread_join(worker)");
        rc_line("sys_ppu_thread_join(worker)", sys_ppu_thread_join(worker_id, &retval));
    }

    call("cellSysmoduleUnloadModule(LV2DBG)");
    rc_line("cellSysmoduleUnloadModule(LV2DBG)", cellSysmoduleUnloadModule(CELL_SYSMODULE_LV2DBG));

done:
    say("LV2DBG_PROBE_DONE status=%d\n", status);
    if (g_log) fclose(g_log);
    return status;
}
