/*
 * sprx-segments loader: load, start, call, stop and unload the sprx_segments
 * module, printing one verdict line per step.
 *
 *   SPRX_SEG begin step=<s>          printed (and flushed) before each step
 *                                    that could hang, so the last line on the
 *                                    TTY shows where a console stopped
 *   SPRX_SEG PASS step=<s> ...       a step that did what it should
 *   SPRX_SEG FAIL step=<s> ...       a step that did not
 *   SPRX_SEG_ALL_PASS steps=<n>      the final line when every step passed
 *   SPRX_SEG_RESULT FAIL failed=<n>  the final line otherwise
 *
 * The module is looked for in the directory named by argv[1] when that is an
 * absolute path (a launcher whose child does not get its own path in argv[0]
 * passes the module's directory this way), then next to this executable (the
 * directory of argv[0]), then in /dev_hdd0/tmp/ps3dk-sprx-segments (a fixed
 * staging directory), then in /app_home, as <stem>.sprx first and then
 * <stem>.fake.sprx; the argv it was started with is printed first
 * (SPRX_SEG argv argc=<n> [i]=<s>...):
 * a .self loads the real-signed .sprx, a .fake.self needs the .fake.sprx
 * (it gets 0x80010009 for the real one and moves on).
 */

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/process.h>
#include <sys/prx.h>

#include "segments_contract.h"

SYS_PROCESS_PARAM(1001, 0x10000);

/* A fixed directory a host can stage the module in (over FTP, say) when the launcher's argv names none. */
#define SEG_STAGING_DIR "/dev_hdd0/tmp/ps3dk-sprx-segments"

#ifndef MODULE_STEM
#define MODULE_STEM "sprx_segments"
#endif

#if defined(__LP64__)
#define SEG_ABI "lp64"
#else
#define SEG_ABI "ilp32"
#endif

static unsigned g_passed;
static unsigned g_failed;

static void begin(const char *step)
{
	printf("SPRX_SEG begin step=%s\n", step);
	fflush(stdout);
}

static int verdict(int ok, const char *step, const char *fmt, ...)
{
	va_list ap;
	printf("SPRX_SEG %s step=%s ", ok ? "PASS" : "FAIL", step);
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

static int check_u32(const char *step, uint32_t got, uint32_t want)
{
	return verdict(got == want, step, "got=0x%08lx want=0x%08lx",
	               (unsigned long)got, (unsigned long)want);
}

static int finish(void)
{
	if (g_failed == 0) {
		printf("SPRX_SEG_ALL_PASS steps=%u abi=%s\n", g_passed, SEG_ABI);
		fflush(stdout);
		return 0;
	}
	printf("SPRX_SEG_RESULT FAIL failed=%u passed=%u abi=%s\n", g_failed, g_passed, SEG_ABI);
	fflush(stdout);
	return 1;
}

/* Try <dir>/<stem>.sprx then <dir>/<stem>.fake.sprx for each directory. */
static sys_prx_id_t load_module(const char *argv1, const char *argv0, char *loaded, size_t loaded_len)
{
	char dirs[4][192];
	int ndirs = 0;

	if (argv1 != NULL && argv1[0] == '/') {
		size_t n = strlen(argv1);
		while (n > 1 && argv1[n - 1] == '/')
			n--;
		if (n < sizeof(dirs[0])) {
			memcpy(dirs[ndirs], argv1, n);
			dirs[ndirs][n] = '\0';
			ndirs++;
		}
	}
	if (argv0 != NULL && argv0[0] == '/') {
		const char *slash = strrchr(argv0, '/');
		size_t n = (size_t)(slash - argv0);
		if (n > 0 && n < sizeof(dirs[0])) {
			memcpy(dirs[ndirs], argv0, n);
			dirs[ndirs][n] = '\0';
			ndirs++;
		}
	}
	/* then the fixed staging directory, then /app_home; each directory once */
	static const char *const fixed[2] = { SEG_STAGING_DIR, "/app_home" };
	for (int f = 0; f < 2; f++) {
		int have = 0;
		for (int d = 0; d < ndirs; d++)
			have |= strcmp(dirs[d], fixed[f]) == 0;
		if (!have) {
			strcpy(dirs[ndirs], fixed[f]);
			ndirs++;
		}
	}

	static const char *const exts[2] = { ".sprx", ".fake.sprx" };
	sys_prx_id_t last = -1;
	for (int d = 0; d < ndirs; d++) {
		for (int e = 0; e < 2; e++) {
			char path[256];
			snprintf(path, sizeof(path), "%s/%s%s", dirs[d], MODULE_STEM, exts[e]);
			sys_prx_id_t id = sys_prx_load_module(path, 0, NULL);
			printf("SPRX_SEG try path=%s rc=0x%08lx\n", path, (unsigned long)(uint32_t)id);
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

int main(int argc, char **argv)
{
	printf("sprx-segments loader (%s): module %s\n", SEG_ABI, MODULE_STEM);
	/* what the process was started with: which of the searched directories it could name */
	printf("SPRX_SEG argv argc=%d", argc);
	for (int i = 0; i < argc; i++)
		printf(" [%d]=%s", i, argv[i] ? argv[i] : "(null)");
	printf("\n");
	fflush(stdout);

	/* --- load --------------------------------------------------------- */
	begin("load");
	char loaded[256] = "";
	sys_prx_id_t id = load_module(argc > 1 ? argv[1] : NULL, argc > 0 ? argv[0] : NULL, loaded, sizeof(loaded));
	if (!verdict(id >= 0, "load", "id=0x%08lx path=%s", (unsigned long)(uint32_t)id,
	             loaded[0] ? loaded : "(none)"))
		return finish();

	/* --- start: module_start writes .data and .bss -------------------- */
	seg_start_report_t start;
	memset(&start, 0, sizeof(start));
	int modres = -1;
	begin("start");
	int rc = sys_prx_start_module(id, sizeof(start), &start, &modres, 0, NULL);
	if (!verdict(rc == 0 && modres == SYS_PRX_RESIDENT, "start", "rc=0x%08lx modres=%d",
	             (unsigned long)(uint32_t)rc, modres))
		return finish();

	verdict(start.magic == SEG_START_MAGIC, "start-ran", "magic=0x%08lx",
	        (unsigned long)start.magic);
	check_u32("data-initial", start.data_before, SEG_DATA_INITIAL);
	check_u32("data-write", start.data_after, SEG_DATA_STARTED);
	check_u32("bss-initial", start.bss_before, 0);
	check_u32("bss-write", start.bss_after, SEG_BSS_STARTED);
	verdict(start.block_bad == 0, "bss-block", "bad_words=%lu of %u",
	        (unsigned long)start.block_bad, SEG_BSS_BLOCK_WORDS);
	check_u32("rodata-read", start.rodata_sum, SEG_RODATA_SUM);

	/* --- calls into both libraries, through the stub archives ---------- */
	begin("calls");
	int sum = seg_calc_add(10, 20);
	verdict(sum == 10 + 20 + 0x42, "seg_calc_add", "got=%d want=%d", sum, 10 + 20 + 0x42);
	int scaled = seg_calc_scale(3, 4);
	verdict(scaled == 15, "seg_calc_scale", "got=%d want=15", scaled);
	check_u32("seg_state_data", seg_state_data(), SEG_DATA_STARTED);
	check_u32("seg_state_bss", seg_state_bss(), SEG_BSS_STARTED);
	uint32_t b1 = seg_state_bump();
	uint32_t b2 = seg_state_bump();
	verdict(b1 == 1 && b2 == 2, "seg_state_bump", "got=%lu,%lu want=1,2",
	        (unsigned long)b1, (unsigned long)b2);

	/* --- stop: module_stop writes .data again -------------------------- */
	seg_stop_report_t stop;
	memset(&stop, 0, sizeof(stop));
	modres = -1;
	begin("stop");
	rc = sys_prx_stop_module(id, sizeof(stop), &stop, &modres, 0, NULL);
	if (verdict(rc == 0 && modres == SYS_PRX_STOP_OK, "stop", "rc=0x%08lx modres=%d",
	            (unsigned long)(uint32_t)rc, modres)) {
		verdict(stop.magic == SEG_STOP_MAGIC, "stop-ran", "magic=0x%08lx",
		        (unsigned long)stop.magic);
		check_u32("stop-data-seen", stop.data_before, SEG_DATA_STARTED);
		check_u32("stop-data-write", stop.data_after, SEG_DATA_STOPPED);
		check_u32("stop-bumps", stop.bumps, 2);
	}

	/* --- unload -------------------------------------------------------- */
	begin("unload");
	rc = sys_prx_unload_module(id, 0, NULL);
	verdict(rc == 0, "unload", "rc=0x%08lx", (unsigned long)(uint32_t)rc);

	return finish();
}
