/*
 * hello-ppu-msgdialog-string - exercise a text-only dialog via
 * cellMsgDialogOpen2 with TYPE_BUTTON_TYPE_NONE.
 *
 * Opens a text-display dialog with no interactive buttons.  The
 * dialog auto-dismisses after a 5-second timer via cellMsgDialogClose.
 *
 * Exercises:
 *     cellMsgDialogOpen2  -> TYPE_BUTTON_TYPE_NONE (non-interactive)
 *     cellMsgDialogClose  -> programmatic close with float delay
 *
 * Runtime on RPCS3 / real HW:
 *   - A system text dialog appears with our message.
 *   - After ~5 seconds, cellMsgDialogClose schedules dismissal.
 *   - printf goes to RPCS3's TTY.log.
 */

#include <stdint.h>
#include <stdio.h>
#include <unistd.h>

#include <sys/process.h>

#include <cell/sysutil.h>
#include <cell/msg_dialog.h>

SYS_PROCESS_PARAM(1001, 0x10000);

static volatile int g_dialog_done = 0;
static volatile int g_last_button = CELL_MSGDIALOG_BUTTON_INVALID;

static void on_button_pressed(int button_type, void *userdata)
{
	(void)userdata;
	g_last_button = button_type;
	g_dialog_done = 1;
	printf("  dialog: button=%d\n", button_type);
}

static void on_sysutil_event(uint64_t status, uint64_t param, void *userdata)
{
	(void)param;
	(void)userdata;
	printf("  event: status=0x%08llx\n", (unsigned long long)status);
}

int main(int argc, char **argv)
{
	(void)argc;
	(void)argv;

	printf("hello-ppu-msgdialog-string: text-only dialog validation\n");

	int rc = cellSysutilRegisterCallback(0, on_sysutil_event, NULL);
	if (rc != 0) {
		printf("  cellSysutilRegisterCallback failed: 0x%08x\n", (unsigned)rc);
		return 1;
	}

	unsigned int type =
		CELL_MSGDIALOG_TYPE_SE_TYPE_NORMAL       |
		CELL_MSGDIALOG_TYPE_BG_VISIBLE           |
		CELL_MSGDIALOG_TYPE_BUTTON_TYPE_NONE     |
		CELL_MSGDIALOG_TYPE_DISABLE_CANCEL_ON;

	rc = cellMsgDialogOpen2(type,
	                        "Text-display dialog: auto-closing in 5s...",
	                        on_button_pressed, NULL, NULL);
	if (rc != 0) {
		printf("  cellMsgDialogOpen2 failed: 0x%08x\n", (unsigned)rc);
		cellSysutilUnregisterCallback(0);
		return 1;
	}
	printf("  text-only dialog opened - closing in 5 seconds\n");

	/* Wait 5s, pumping callbacks. */
	for (int i = 0; i < 50 && !g_dialog_done; i++) {
		cellSysutilCheckCallback();
		usleep(100 * 1000);
	}

	/* Programmatic close with a short fade delay.
	 * expected: 0x00000000 (CELL_OK) */
	printf("  calling cellMsgDialogClose(0.5f)\n");
	int rcClose = cellMsgDialogClose(0.5f);
	printf("  cellMsgDialogClose -> 0x%08x\n", (unsigned)rcClose);

	/* Pump callbacks a bit more so the close processes. */
	for (int j = 0; j < 20 && !g_dialog_done; j++) {
		cellSysutilCheckCallback();
		usleep(100 * 1000);
	}

	if (!g_dialog_done) {
		printf("  timed out (expected in automated run)\n");
		int rcAbort = cellMsgDialogAbort();
		/* expected: 0x00000000 (CELL_OK) */
		printf("  cellMsgDialogAbort -> 0x%08x\n", (unsigned)rcAbort);
	} else {
		printf("  dialog dismissed via callback\n");
	}

	cellSysutilUnregisterCallback(0);
	printf("  final button: %d\n", g_last_button);
	/* Automated: TIMEOUT expected (callbacks may not fire under RPCS3).
	 * Manual run: PASS. */
	printf("result: %s\n", g_dialog_done ? "PASS" : "TIMEOUT (expected in automated)");
	return 0;
}
