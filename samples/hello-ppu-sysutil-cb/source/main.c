/*
 * hello-ppu-sysutil-cb — exercise cellSysutilRegisterCallback over
 * PSL1GHT's runtime.  First Phase 6 backfill sample: Sony-named API
 * call, PSL1GHT SPRX dispatch underneath.
 *
 * Expected runtime behaviour on RPCS3:
 *   1. Program registers a callback in slot 0.
 *   2. Main loop sleeps briefly, then calls cellSysutilCheckCallback
 *      which delivers any pending XMB / system events to our handler.
 *   3. Press PS button → XMB opens → handler sees
 *      CELL_SYSUTIL_SYSTEM_MENU_OPEN, prints it.
 *   4. Close XMB → CELL_SYSUTIL_SYSTEM_MENU_CLOSE → prints.
 *   5. Quit from XMB → CELL_SYSUTIL_REQUEST_EXITGAME → loop exits.
 *
 * This sample does not draw to RSX; it only uses printf + sysutil.
 * RPCS3 will show the stdout in its terminal window.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include <sys/process.h>

#include <cell/sysutil.h>

SYS_PROCESS_PARAM(1001, 0x10000);

static volatile int g_exit_requested = 0;
static volatile int g_events_seen   = 0;

static void on_sysutil_event(uint64_t status, uint64_t param, void *userdata)
{
	(void)param;
	(void)userdata;

	g_events_seen++;

	switch (status) {
	case CELL_SYSUTIL_REQUEST_EXITGAME:
		printf("  event: EXIT_GAME requested — shutting down\n");
		g_exit_requested = 1;
		break;
	case CELL_SYSUTIL_DRAWING_BEGIN:
		printf("  event: DRAWING_BEGIN\n");
		break;
	case CELL_SYSUTIL_DRAWING_END:
		printf("  event: DRAWING_END\n");
		break;
	case CELL_SYSUTIL_SYSTEM_MENU_OPEN:
		printf("  event: SYSTEM_MENU_OPEN\n");
		break;
	case CELL_SYSUTIL_SYSTEM_MENU_CLOSE:
		printf("  event: SYSTEM_MENU_CLOSE\n");
		break;
	case CELL_SYSUTIL_BGMPLAYBACK_PLAY:
		printf("  event: BGMPLAYBACK_PLAY\n");
		break;
	case CELL_SYSUTIL_BGMPLAYBACK_STOP:
		printf("  event: BGMPLAYBACK_STOP\n");
		break;
	default:
		printf("  event: 0x%04llx (not one we handle)\n",
		       (unsigned long long)status);
		break;
	}
}

int main(int argc, char **argv)
{
	(void)argc;
	(void)argv;

	printf("hello-ppu-sysutil-cb: testing cellSysutilRegisterCallback\n");

	int rc = cellSysutilRegisterCallback(0, on_sysutil_event, NULL);
	if (rc != 0) {
		printf("  cellSysutilRegisterCallback returned 0x%08x — aborting\n",
		       (unsigned)rc);
		return 1;
	}
	printf("  callback registered on slot 0\n");

	/* Poll for a bounded duration (~30 seconds) so the process exits
	 * even when run headless.  On RPCS3 you have this much time to press
	 * the PS button / navigate XMB / quit to drive events through the
	 * handler.  10s was too tight — XMB open/close takes a few seconds
	 * of its own. */
	for (int i = 0; i < 300 && !g_exit_requested; i++) {
		cellSysutilCheckCallback();
		usleep(100 * 1000); /* 100ms */
	}

	cellSysutilUnregisterCallback(0);
	printf("  callback unregistered\n");
	printf("  events delivered: %d\n", g_events_seen);
	printf("result: %s\n", g_exit_requested ? "EXIT_GAME" : "timeout");
	return 0;
}
