/*
 * vsh-plugin loader: the plugin's loader path in a game process.  Loads,
 * starts, stops and unloads ps3dk_vsh_plugin twice, and checks what each
 * step reports and what the worker wrote to its log.
 *
 *   VSHP begin step=<s>            printed (and flushed) before each step that
 *                                  could hang, so the last TTY line shows where
 *                                  a console stopped
 *   VSHP PASS step=<s> ...         a step that did what it should
 *   VSHP FAIL step=<s> ...         a step that did not
 *   VSHP_ALL_PASS steps=<n>        the final line when every step passed
 *   VSHP_RESULT FAIL failed=<n>    the final line otherwise
 *
 * The module is looked for next to this executable (the directory of
 * argv[0]) and in /app_home, as <stem>.sprx, then <stem>.fake.sprx, then the
 * unsigned <stem>.prx: a .self loads the real-signed .sprx, a .fake.self the
 * .fake.sprx, and a raw .elf boot (RPCS3) the .prx.  The regression rows
 * stage one container per directory, so each row proves the one it names.
 */

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/file.h>
#include <sys/process.h>
#include <sys/prx.h>
#include <sys/timer.h>

#include "vsh_plugin_contract.h"

SYS_PROCESS_PARAM(1001, 0x10000);

#ifndef MODULE_STEM
#define MODULE_STEM "vsh_plugin"
#endif

#if defined(__LP64__)
#define VSHP_ABI "lp64"
#else
#define VSHP_ABI "ilp32"
#endif

static unsigned g_passed;
static unsigned g_failed;

static void begin(const char *step)
{
	printf("VSHP begin step=%s\n", step);
	fflush(stdout);
}

static int verdict(int ok, const char *step, const char *fmt, ...)
{
	va_list ap;
	printf("VSHP %s step=%s ", ok ? "PASS" : "FAIL", step);
	va_start(ap, fmt);
	vprintf(fmt, ap);
	va_end(ap);
	printf("\n");
	fflush(stdout);
	if (ok)
		g_passed++;
	else
		g_failed++;
	return ok;
}

static int finish(void)
{
	if (g_failed == 0) {
		printf("VSHP_ALL_PASS steps=%u abi=%s\n", g_passed, VSHP_ABI);
		fflush(stdout);
		return 0;
	}
	printf("VSHP_RESULT FAIL failed=%u passed=%u abi=%s\n", g_failed, g_passed, VSHP_ABI);
	fflush(stdout);
	return 1;
}

static sys_prx_id_t load_module(const char *argv0, char *loaded, size_t loaded_len)
{
	char dirs[2][192];
	int ndirs = 0;

	if (argv0 != NULL && argv0[0] == '/') {
		const char *slash = strrchr(argv0, '/');
		size_t n = (size_t)(slash - argv0);
		if (n > 0 && n < sizeof(dirs[0])) {
			memcpy(dirs[ndirs], argv0, n);
			dirs[ndirs][n] = '\0';
			ndirs++;
		}
	}
	if (ndirs == 0 || strcmp(dirs[0], "/app_home") != 0) {
		strcpy(dirs[ndirs], "/app_home");
		ndirs++;
	}

	static const char *const exts[3] = { ".sprx", ".fake.sprx", ".prx" };
	sys_prx_id_t last = -1;
	for (int d = 0; d < ndirs; d++) {
		for (int e = 0; e < 3; e++) {
			char path[256];
			snprintf(path, sizeof(path), "%s/%s%s", dirs[d], MODULE_STEM, exts[e]);
			sys_prx_id_t id = sys_prx_load_module(path, 0, NULL);
			printf("VSHP try path=%s rc=0x%08lx\n", path, (unsigned long)(uint32_t)id);
			fflush(stdout);
			if (id >= 0) {
				snprintf(loaded, loaded_len, "%s", path);
				return id;
			}
			last = id;
		}
	}
	return last;
}

/* The log as the worker left it: up to sizeof(buf) - 1 bytes. */
static int read_log(char *buf, size_t len)
{
	s32 fd = -1;
	u64 got = 0;

	if (sysLv2FsOpen(VSHP_LOG_PATH, SYS_O_RDONLY, &fd, 0, NULL, 0) != 0)
		return -1;
	if (sysLv2FsRead(fd, buf, len - 1, &got) != 0)
		got = 0;
	sysLv2FsClose(fd);
	buf[got] = '\0';
	return (int)got;
}

static unsigned count_lines(const char *s, const char *prefix)
{
	unsigned n = 0;
	size_t pl = strlen(prefix);

	while (*s != '\0') {
		if (strncmp(s, prefix, pl) == 0)
			n++;
		const char *nl = strchr(s, '\n');
		if (nl == NULL)
			break;
		s = nl + 1;
	}
	return n;
}

/* One load, start, run, stop, unload cycle.  Returns 0 if it got far
 * enough to unload, so the next cycle can run. */
static int cycle(int round, const char *argv0)
{
	char step[48];

	sysLv2FsUnlink(VSHP_LOG_PATH);

	snprintf(step, sizeof(step), "load-%d", round);
	begin(step);
	char loaded[256] = "";
	sys_prx_id_t id = load_module(argv0, loaded, sizeof(loaded));
	if (!verdict(id >= 0, step, "id=0x%08lx path=%s", (unsigned long)(uint32_t)id, loaded[0] ? loaded : "(none)"))
		return -1;

	/* --- start: module_start sees a caller and starts the worker ------ */
	vshp_start_report_t start;
	memset(&start, 0, sizeof(start));
	int modres = -1;
	snprintf(step, sizeof(step), "start-%d", round);
	begin(step);
	int rc = sys_prx_start_module(id, sizeof(start), &start, &modres, 0, NULL);
	if (!verdict(rc == 0 && modres == SYS_PRX_RESIDENT, step, "rc=0x%08lx modres=%d", (unsigned long)(uint32_t)rc,
	             modres))
		return -1;
	verdict(start.magic == VSHP_START_MAGIC, "start-ran", "magic=0x%08lx", (unsigned long)start.magic);
	verdict(start.caller == VSHP_CALLER_LOADER, "start-caller", "caller=%lu want=%u", (unsigned long)start.caller,
	        VSHP_CALLER_LOADER);
	verdict(start.create_rc == 0, "worker-create", "rc=0x%08lx", (unsigned long)start.create_rc);
	verdict(start.data_initial == VSHP_DATA_INITIAL, "data-initial", "got=0x%08lx want=0x%08lx",
	        (unsigned long)start.data_initial, (unsigned long)VSHP_DATA_INITIAL);

	/* --- let the worker tick a few times (one every 250 ms at first) -- */
	sys_timer_usleep(1500 * 1000);

	/* --- stop: module_stop stops the worker and joins it --------------- */
	vshp_stop_report_t stop;
	memset(&stop, 0, sizeof(stop));
	modres = -1;
	snprintf(step, sizeof(step), "stop-%d", round);
	begin(step);
	rc = sys_prx_stop_module(id, sizeof(stop), &stop, &modres, 0, NULL);
	if (verdict(rc == 0 && modres == SYS_PRX_STOP_OK, step, "rc=0x%08lx modres=%d", (unsigned long)(uint32_t)rc,
	            modres)) {
		uint32_t t = stop.worker_exit;
		verdict(stop.magic == VSHP_STOP_MAGIC, "stop-ran", "magic=0x%08lx", (unsigned long)stop.magic);
		verdict(stop.caller == VSHP_CALLER_LOADER, "stop-caller", "caller=%lu want=%u",
		        (unsigned long)stop.caller, VSHP_CALLER_LOADER);
		verdict(stop.join_rc == 0, "worker-join", "rc=0x%08lx", (unsigned long)stop.join_rc);
		verdict(t >= 2, "worker-ticks", "ticks=%lu want>=2", (unsigned long)t);
		verdict(stop.ticks_data == VSHP_DATA_INITIAL + t, "data-counter", "got=0x%08lx want=0x%08lx",
		        (unsigned long)stop.ticks_data, (unsigned long)(VSHP_DATA_INITIAL + t));
		verdict(stop.ticks_bss == t, "bss-counter", "got=%lu want=%lu", (unsigned long)stop.ticks_bss,
		        (unsigned long)t);
		verdict(stop.log_failures == 0, "log-writes", "failures=%lu", (unsigned long)stop.log_failures);

		/* --- the log: start line, one line per tick, stop line -------- */
		static char log_text[4096];
		int n = read_log(log_text, sizeof(log_text));
		char want_stop[64];
		snprintf(want_stop, sizeof(want_stop), "ps3dk-vsh-plugin: stop caller=loader ticks=%lu\n",
		         (unsigned long)t);
		verdict(n > 0 && strncmp(log_text, "ps3dk-vsh-plugin: start caller=loader ", 38) == 0, "log-start",
		        "bytes=%d", n);
		verdict(count_lines(log_text, "ps3dk-vsh-plugin: tick ") == t, "log-ticks", "lines=%u want=%lu",
		        count_lines(log_text, "ps3dk-vsh-plugin: tick "), (unsigned long)t);
		verdict(n > 0 && strstr(log_text, want_stop) != NULL, "log-stop", "want=\"%.*s\"",
		        (int)strlen(want_stop) - 1, want_stop);
	}

	/* --- unload ---------------------------------------------------------- */
	snprintf(step, sizeof(step), "unload-%d", round);
	begin(step);
	rc = sys_prx_unload_module(id, 0, NULL);
	return verdict(rc == 0, step, "rc=0x%08lx", (unsigned long)(uint32_t)rc) ? 0 : -1;
}

int main(int argc, char **argv)
{
	printf("vsh-plugin loader (%s): module %s, log %s\n", VSHP_ABI, MODULE_STEM, VSHP_LOG_PATH);
	fflush(stdout);
	const char *argv0 = argc > 0 ? argv[0] : NULL;

	/* Twice: a second load gets a fresh image (its .data counter back at
	 * the initial value), as a plugin reloaded into VSH does. */
	if (cycle(1, argv0) == 0)
		cycle(2, argv0);
	return finish();
}
