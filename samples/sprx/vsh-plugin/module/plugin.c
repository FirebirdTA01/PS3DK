/*
 * ps3dk_vsh_plugin: a module that runs as a VSH plugin, or in a game process.
 *
 * A VSH plugin loader (webMAN MOD's /loadprx.ps3 and /unloadprx.ps3, through
 * Cobra) and sys_prx_start_module / sys_prx_stop_module both run the module's
 * module_start and module_stop, but not the same way:
 *
 *   loader path  liblv2's sys_prx_start_module calls module_start on a
 *                normal thread with a TLS area and a caller to return to,
 *                and finishes the start itself; sys_prx_stop_module does
 *                the same for module_stop and sends Lv-2 the final stop
 *                report.  Both entry points just return.
 *   bare path    the plugin loader has the kernel create a thread for each
 *                entry point: no TLS area and no caller.  Returning jumps to
 *                a poisoned return address, and liblv2's
 *                sys_ppu_thread_exit reads TLS, so both entry points end the
 *                thread with the exit system call (41) itself.  The loader
 *                finishes the start, but not the stop: module_stop sends the
 *                final stop report itself, or the module never reaches the
 *                stopped state and the unload waits for ever.
 *
 * In both paths module_start only starts a worker thread with
 * sys_ppu_thread_create (a normal thread: TLS, a caller), and module_stop
 * only tells it to stop and joins it.  The worker does the plugin's job:
 * a line in VSHP_LOG_PATH per tick, each written with fsync, and a counter
 * in .data and one in .bss, which shows the module can write its own data.
 *
 * Nothing here reads TLS or calls newlib: the bare path's threads have no
 * TLS, and that path's module_start / module_stop have a 4 KiB stack.
 * See README.md for the pattern and its limits.
 */

#include <stddef.h>
#include <stdint.h>

#include <sys/file.h>
#include <sys/lv2_syscall.h>
#include <sys/moduleexport.h>
#include <sys/ppu_thread.h>
#include <sys/prx.h>
#include <sys/timer.h>

#include "vsh_plugin_contract.h"

SYS_MODULE_INFO(ps3dk_vsh_plugin, 0, 1, 0);
SYS_MODULE_START(plugin_start);
SYS_MODULE_STOP(plugin_stop);

/* --- the module's own data ---------------------------------------------------- */

static volatile uint32_t g_ticks_data = VSHP_DATA_INITIAL;  /* .data */
static volatile uint32_t g_ticks_bss;                       /* .bss  */
static volatile uint32_t g_log_failures;                    /* .bss  */
static uint32_t g_stop;          /* set by module_stop, read by the worker */
static uint32_t g_stop_caller;   /* VSHP_CALLER_* of module_stop           */
static uint32_t g_start_caller;  /* VSHP_CALLER_* of module_start          */
static sys_ppu_thread_t g_worker;
static int32_t g_log_fd = -1;

/* --- the bare path: no caller, no TLS ----------------------------------------- */

/* The return address a plugin loader's thread starts with: a poison value
 * the kernel faults on (reported by testers in issue #137). */
#define VSHP_POISON_RETURN 0xBADADD00u

/* A thread pointer (r13) is 0x7000 past the start of a TLS block. */
#define VSHP_TLS_POINTER_MIN 0x7000ull

/* Does the entry point have a usable caller?  It needs somewhere to return
 * to and a TLS area: a loader thread has both, the kernel-made thread of a
 * plugin loader neither.  Either missing means the bare path.  Reads r13
 * and the return address only, never memory. */
static uint32_t entry_caller(uint64_t return_address)
{
	uint64_t tp;

	__asm__ __volatile__("mr %0, 13" : "=r"(tp));
	if (return_address == 0 || (return_address & 3) != 0 ||
	    (uint32_t)return_address == VSHP_POISON_RETURN)
		return VSHP_CALLER_BARE;
	if (tp < VSHP_TLS_POINTER_MIN || tp > 0xffffffffull)
		return VSHP_CALLER_BARE;
	return VSHP_CALLER_LOADER;
}

/* Lv-2 system call 41: end this thread, with nothing that reads TLS. */
static void __attribute__((noreturn)) bare_thread_exit(uint64_t code)
{
	lv2syscall1(41, code);
	__builtin_unreachable();
}

/* The final stop report a plugin loader does not send: system call 482
 * (_sys_prx_stop_module) with command 2 and result 0 (stopped), on this
 * module's id.  The option block is five 64-bit words: size (0x28), command,
 * the stop entry and the result, and a second entry the kernel fills in on
 * command 1.  With command 2 and result 0 Lv-2 moves the module from
 * stopping to stopped, after which it can be unloaded; liblv2's
 * sys_prx_stop_module sends exactly this after module_stop returns, which is
 * why the loader path must not send it too.  (Behaviour as RPCS3 models it in
 * rpcs3/Emu/Cell/lv2/sys_prx.cpp, _sys_prx_stop_module; the layout is its
 * sys_prx_start_stop_module_option_t.) */
struct vshp_stop_option {
	uint64_t size;
	uint64_t cmd;
	uint64_t entry;
	uint64_t res;
	uint64_t entry2;
};

static int32_t report_stopped(void)
{
	struct vshp_stop_option opt = { sizeof(opt), 2, 0, 0, 0 };
	int32_t id;

	{
		/* _sys_prx_get_module_id_by_address: any address in the module. */
		lv2syscall1(461, (uint64_t)(uintptr_t)&g_ticks_data);
		id = (int32_t)p1;
	}
	if (id < 0)
		return id;
	{
		lv2syscall3(482, (uint64_t)(uint32_t)id, 0, (uint64_t)(uintptr_t)&opt);
		return (int32_t)p1;
	}
}

/* --- the worker's log -------------------------------------------------------------- */

struct vshp_line {
	char     text[112];
	uint32_t len;
};

static void line_char(struct vshp_line *l, char c)
{
	if (l->len < sizeof(l->text))
		l->text[l->len++] = c;
}

static void line_str(struct vshp_line *l, const char *s)
{
	while (*s != '\0')
		line_char(l, *s++);
}

static void line_dec(struct vshp_line *l, uint32_t v)
{
	char digits[10];
	int n = 0;

	do {
		digits[n++] = (char)('0' + v % 10);
		v /= 10;
	} while (v != 0);
	while (n > 0)
		line_char(l, digits[--n]);
}

static void line_hex(struct vshp_line *l, uint32_t v)
{
	line_str(l, "0x");
	for (int shift = 28; shift >= 0; shift -= 4)
		line_char(l, "0123456789abcdef"[(v >> shift) & 0xf]);
}

static const char *caller_name(uint32_t caller)
{
	return caller == VSHP_CALLER_BARE ? "bare" : "loader";
}

/* Append the line and flush it to disk; a failure is counted, not fatal. */
static void log_write(struct vshp_line *l)
{
	u64 written = 0;

	line_char(l, '\n');
	if (g_log_fd < 0) {
		g_log_failures = g_log_failures + 1;
		return;
	}
	if (sysLv2FsWrite(g_log_fd, l->text, l->len, &written) != 0 || written != l->len)
		g_log_failures = g_log_failures + 1;
	if (sysLv2FsFsync(g_log_fd) != 0)
		g_log_failures = g_log_failures + 1;
}

/* --- the worker ------------------------------------------------------------------- */

/* Ticks come every 250 ms for the first eight, then every 2 s: quick to see
 * in a loader run, quiet in VSH.  The sleep is cut into 50 ms slices so a
 * stop request is seen promptly. */
static void worker_main(uint64_t arg)
{
	struct vshp_line l;
	uint32_t ticks = 0;
	int32_t fd = -1;

	(void)arg;
	if (sysLv2FsOpen(VSHP_LOG_PATH, SYS_O_WRONLY | SYS_O_CREAT | SYS_O_APPEND, &fd, 0666, NULL, 0) == 0)
		g_log_fd = fd;

	l.len = 0;
	line_str(&l, "ps3dk-vsh-plugin: start caller=");
	line_str(&l, caller_name(g_start_caller));
	line_str(&l, " data=");
	line_hex(&l, g_ticks_data);
	line_str(&l, " bss=");
	line_dec(&l, g_ticks_bss);
	log_write(&l);

	while (!__atomic_load_n(&g_stop, __ATOMIC_ACQUIRE)) {
		uint32_t slices = ticks < 8 ? 5 : 40;
		for (uint32_t i = 0; i < slices && !__atomic_load_n(&g_stop, __ATOMIC_ACQUIRE); i++)
			sys_timer_usleep(50 * 1000);
		if (__atomic_load_n(&g_stop, __ATOMIC_ACQUIRE))
			break;

		ticks++;
		g_ticks_data = g_ticks_data + 1;
		g_ticks_bss = g_ticks_bss + 1;
		l.len = 0;
		line_str(&l, "ps3dk-vsh-plugin: tick ");
		line_dec(&l, ticks);
		line_str(&l, " data=");
		line_hex(&l, g_ticks_data);
		line_str(&l, " bss=");
		line_dec(&l, g_ticks_bss);
		log_write(&l);
	}

	l.len = 0;
	line_str(&l, "ps3dk-vsh-plugin: stop caller=");
	line_str(&l, caller_name(__atomic_load_n(&g_stop_caller, __ATOMIC_ACQUIRE)));
	line_str(&l, " ticks=");
	line_dec(&l, ticks);
	log_write(&l);
	if (g_log_fd >= 0) {
		sysLv2FsClose(g_log_fd);
		g_log_fd = -1;
	}
	/* This thread has TLS, so this is liblv2's exit; the code reaches the
	 * join in module_stop. */
	sys_ppu_thread_exit(ticks);
}

/* --- module_start / module_stop ----------------------------------------------------- */

int plugin_start(size_t args, void *argp)
{
	uint32_t caller = entry_caller((uint64_t)(uintptr_t)__builtin_return_address(0));
	vshp_start_report_t *rep = NULL;
	uint32_t data_initial = g_ticks_data;
	int rc;

	if (caller == VSHP_CALLER_LOADER && args >= sizeof(*rep) && argp != NULL)
		rep = (vshp_start_report_t *)argp;
	g_start_caller = caller;
	__atomic_store_n(&g_stop, 0, __ATOMIC_RELEASE);
	rc = sys_ppu_thread_create(&g_worker, worker_main, 0, 2000, 0x4000,
	                           SYS_PPU_THREAD_CREATE_JOINABLE, "ps3dk_vsh_plugin");
	if (rep != NULL) {
		rep->caller = caller;
		rep->create_rc = (uint32_t)rc;
		rep->data_initial = data_initial;
		rep->magic = VSHP_START_MAGIC;
	}
	/* A start that fails tells the loader the module will not stay. */
	if (caller == VSHP_CALLER_BARE)
		bare_thread_exit(rc == 0 ? SYS_PRX_RESIDENT : SYS_PRX_NO_RESIDENT);
	return rc == 0 ? SYS_PRX_RESIDENT : SYS_PRX_NO_RESIDENT;
}

int plugin_stop(size_t args, void *argp)
{
	uint32_t caller = entry_caller((uint64_t)(uintptr_t)__builtin_return_address(0));
	vshp_stop_report_t *rep = NULL;
	uint64_t worker_exit = 0;
	int rc;

	if (caller == VSHP_CALLER_LOADER && args >= sizeof(*rep) && argp != NULL)
		rep = (vshp_stop_report_t *)argp;
	__atomic_store_n(&g_stop_caller, caller, __ATOMIC_RELEASE);
	__atomic_store_n(&g_stop, 1, __ATOMIC_RELEASE);
	/* sys_ppu_thread_join is a bare system call: fine on either path. */
	rc = sys_ppu_thread_join(g_worker, &worker_exit);
	if (rep != NULL) {
		rep->caller = caller;
		rep->join_rc = (uint32_t)rc;
		rep->worker_exit = (uint32_t)worker_exit;
		rep->ticks_data = g_ticks_data;
		rep->ticks_bss = g_ticks_bss;
		rep->log_failures = g_log_failures;
		rep->magic = VSHP_STOP_MAGIC;
	}
	if (caller == VSHP_CALLER_BARE) {
		(void)report_stopped();
		bare_thread_exit(SYS_PRX_STOP_OK);
	}
	return SYS_PRX_STOP_OK;
}
