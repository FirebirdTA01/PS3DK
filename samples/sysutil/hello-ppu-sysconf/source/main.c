/*
 * hello-ppu-sysconf — open the audio-device configuration sub-dialog
 * (cellSysconfOpen) and wait for it to dispatch a result via callback.
 * Also exercises cellSysconfAbort by calling it on timeout (will
 * normally return PARAM if no dialog is open, but proves it links).
 *
 * Expected on RPCS3: dialog opens, user dismisses, callback fires
 * with CELL_SYSCONF_RESULT_OK or CANCELED.
 */

#include <stdint.h>
#include <stdio.h>
#include <unistd.h>

#include <sys/memory.h>
#include <sys/process.h>
#include <cell/sysutil.h>
#include <cell/sysutil_sysconf.h>

SYS_PROCESS_PARAM(1001, 0x10000);

#define SYSCONF_HEAP_BYTES (4 * 1024 * 1024)

static volatile int g_dispatched = 0;

static void sysconf_cb(int result, void *userdata)
{
	(void)userdata;
	g_dispatched = 1;
	const char *kind = result == CELL_SYSCONF_RESULT_OK       ? "OK"
	                : result == CELL_SYSCONF_RESULT_CANCELED ? "CANCELED"
	                : "?";
	printf("  sysconf callback: result=%s (%d)\n", kind, result);
}

int main(void)
{
	printf("hello-ppu-sysconf: cellSysconfOpen(AUDIO_DEVICE_SETTINGS)\n");

	int rc = cellSysutilRegisterCallback(0, NULL, NULL);
	(void)rc;  /* sysutil callback isn't strictly required for sysconf */

	sys_memory_container_t mem = 0;
	rc = sysMemContainerCreate(&mem, SYSCONF_HEAP_BYTES);
	printf("  sysMemContainerCreate -> 0x%08x\n", (unsigned)rc);
	if (rc != 0) return 1;

	rc = cellSysconfOpen(CELL_SYSCONF_TYPE_AUDIO_DEVICE_SETTINGS,
	                     sysconf_cb, NULL, NULL, mem);
	printf("  cellSysconfOpen -> 0x%08x\n", (unsigned)rc);

	for (int i = 0; i < 300 && !g_dispatched; i++) {
		cellSysutilCheckCallback();
		usleep(100 * 1000);
	}

	if (!g_dispatched) {
		rc = cellSysconfAbort();
		printf("  cellSysconfAbort (timeout) -> 0x%08x\n", (unsigned)rc);
	}

	sysMemContainerDestroy(mem);
	cellSysutilUnregisterCallback(0);
	return g_dispatched ? 0 : 1;
}
