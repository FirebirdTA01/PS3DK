/*
 * hello-ppu-msgdialog — exercise cellMsgDialogOpen2 through PSL1GHT's
 * runtime.  Opens a Yes/No dialog, waits for the user to pick a
 * button, prints which one they pressed.
 *
 * Exercises the <cell/msg_dialog.h> forwarder:
 *     cellMsgDialogOpen2  → msgDialogOpen2 → msgDialogOpen2Ex
 *     .rodata.sceFNID imports FNID 0x7603d3db
 *
 * Runtime on RPCS3 / real HW:
 *   - A Yes/No system dialog appears with our prompt string.
 *   - Pressing X on Yes or No invokes on_button_pressed() with the
 *     corresponding CELL_MSGDIALOG_BUTTON_YES / _NO code.
 *   - Pressing O / circle / back gives CELL_MSGDIALOG_BUTTON_ESCAPE.
 *   - The printf goes to RPCS3's TTY.log (~/.cache/rpcs3/TTY.log).
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
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

	switch (button_type) {
	case CELL_MSGDIALOG_BUTTON_YES:
		printf("  dialog: YES pressed\n");
		break;
	case CELL_MSGDIALOG_BUTTON_NO:
		printf("  dialog: NO pressed\n");
		break;
	case CELL_MSGDIALOG_BUTTON_ESCAPE:
		printf("  dialog: ESCAPE (cancel) pressed\n");
		break;
	default:
		printf("  dialog: button=%d (unexpected)\n", button_type);
		break;
	}
}

static void on_sysutil_event(uint64_t status, uint64_t param, void *userdata)
{
	(void)param;
	(void)userdata;
	if (status == CELL_SYSUTIL_REQUEST_EXITGAME) {
		printf("  event: EXIT_GAME — treating as dialog cancel\n");
		g_dialog_done = 1;
	}
}

int main(int argc, char **argv)
{
	(void)argc;
	(void)argv;

	printf("hello-ppu-msgdialog: cellMsgDialogOpen2 round-trip\n");

	int rc = cellSysutilRegisterCallback(0, on_sysutil_event, NULL);
	if (rc != 0) {
		printf("  cellSysutilRegisterCallback failed: 0x%08x\n", (unsigned)rc);
		return 1;
	}

	unsigned int type =
		CELL_MSGDIALOG_TYPE_SE_TYPE_NORMAL      |
		CELL_MSGDIALOG_TYPE_BG_VISIBLE          |
		CELL_MSGDIALOG_TYPE_BUTTON_TYPE_YESNO   |
		CELL_MSGDIALOG_TYPE_DEFAULT_CURSOR_YES;

	rc = cellMsgDialogOpen2(type,
	                        "hello-ppu-msgdialog: press Yes or No",
	                        on_button_pressed, NULL, NULL);
	if (rc != 0) {
		printf("  cellMsgDialogOpen2 failed: 0x%08x\n", (unsigned)rc);
		cellSysutilUnregisterCallback(0);
		return 1;
	}
	printf("  dialog opened — waiting for button press\n");

	/* Pump sysutil events so the dialog's callback actually fires.
	 * Up to 60 seconds, then we give up. */
	for (int i = 0; i < 600 && !g_dialog_done; i++) {
		cellSysutilCheckCallback();
		usleep(100 * 1000); /* 100ms */
	}

	if (!g_dialog_done) {
		printf("  timed out — aborting dialog\n");
		cellMsgDialogAbort();
	}

	cellSysutilUnregisterCallback(0);
	printf("  final button: %d\n", g_last_button);
	printf("result: %s\n", g_dialog_done ? "PASS" : "TIMEOUT");
	return g_dialog_done ? 0 : 1;
}
