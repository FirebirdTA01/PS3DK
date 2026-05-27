/*
 * hello-ppu-msgdialog-errcode - exercise cellMsgDialogOpenErrorCode.
 *
 * Opens an error-code dialog that displays a formatted error string
 * for the given code.  Uses 0x80020001 as an example (no real
 * meaning; the system will format and display whatever string
 * matches that code, if any).
 *
 * Exercises:
 *     cellMsgDialogOpenErrorCode  -> .rodata.sceFNID via libsysutil_stub.a
 *
 * Runtime on RPCS3 / real HW:
 *   - A system error dialog appears with the formatted code.
 *   - Pressing X dismisses via the callback with CELL_MSGDIALOG_BUTTON_OK.
 *   - printf goes to RPCS3's TTY.log (~/.cache/rpcs3/TTY.log).
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

	switch (button_type) {
	case CELL_MSGDIALOG_BUTTON_OK:
		printf("  dialog: OK pressed\n");
		break;
	case CELL_MSGDIALOG_BUTTON_ESCAPE:
		printf("  dialog: ESCAPE (cancel) pressed\n");
		break;
	default:
		printf("  dialog: button=%d\n", button_type);
		break;
	}
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

	printf("hello-ppu-msgdialog-errcode: cellMsgDialogOpenErrorCode validation\n");

	int rc = cellSysutilRegisterCallback(0, on_sysutil_event, NULL);
	if (rc != 0) {
		printf("  cellSysutilRegisterCallback failed: 0x%08x\n", (unsigned)rc);
		return 1;
	}

	/* Open error-code dialog with example code 0x80020001.
	 * expected: 0x00000000 (CELL_OK) - dialog opens */
	rc = cellMsgDialogOpenErrorCode(0x80020001,
	                                on_button_pressed, NULL, NULL);
	if (rc != 0) {
		printf("  cellMsgDialogOpenErrorCode failed: 0x%08x\n", (unsigned)rc);
		cellSysutilUnregisterCallback(0);
		return 1;
	}
	printf("  error-code dialog opened (manual: press X to dismiss)\n");

	/* Wait for button press.  If user does not press within 60 s,
	 * abort the dialog - this is a manual-interactive sample.
	 * Automated runs will time out, which is expected. */
	for (int i = 0; i < 600 && !g_dialog_done; i++) {
		cellSysutilCheckCallback();
		usleep(100 * 1000);
	}

	if (!g_dialog_done) {
		printf("  timed out (expected in automated run) - aborting\n");
		int rcAbort = cellMsgDialogAbort();
		/* expected: 0x00000000 (CELL_OK) - abort returns OK */
		printf("  cellMsgDialogAbort -> 0x%08x\n", (unsigned)rcAbort);
	}

	cellSysutilUnregisterCallback(0);
	printf("  final button: %d\n", g_last_button);
	/* Automated run: TIMEOUT is expected (no user X-press).
	 * Manual run with X-press: PASS. */
	printf("result: %s\n", g_dialog_done ? "PASS" : "TIMEOUT (expected in automated run)");
	return 0;
}
