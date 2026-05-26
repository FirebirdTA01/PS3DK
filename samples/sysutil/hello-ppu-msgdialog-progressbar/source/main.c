/*
 * hello-ppu-msgdialog-progressbar — exercise the progress-bar dialog
 * via cellMsgDialogOpen2 + cellMsgDialogProgressBarInc/SetMsg.
 *
 * Opens a single-progressbar dialog, increments it from 0 to 100
 * in steps, then waits for user dismissal (or auto-times out).
 *
 * Exercises:
 *     cellMsgDialogOpen2            → TYPE_PROGRESSBAR_SINGLE
 *     cellMsgDialogProgressBarSetMsg
 *     cellMsgDialogProgressBarInc
 *     cellMsgDialogProgressBarReset (at end before abort)
 *     cellMsgDialogAbort
 *
 * Runtime on RPCS3 / real HW:
 *   - A system progress-bar dialog appears, filling up in real time.
 *   - User can dismiss via ESCAPE / circle.
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

	printf("hello-ppu-msgdialog-progressbar: progress-bar dialog smoke\n");

	int rc = cellSysutilRegisterCallback(0, on_sysutil_event, NULL);
	if (rc != 0) {
		printf("  cellSysutilRegisterCallback failed: 0x%08x\n", (unsigned)rc);
		return 1;
	}

	unsigned int type =
		CELL_MSGDIALOG_TYPE_SE_TYPE_NORMAL      |
		CELL_MSGDIALOG_TYPE_BG_VISIBLE          |
		CELL_MSGDIALOG_TYPE_BUTTON_TYPE_NONE    |
		CELL_MSGDIALOG_TYPE_PROGRESSBAR_SINGLE;

	rc = cellMsgDialogOpen2(type,
	                        "Progress-bar demo: running...",
	                        on_button_pressed, NULL, NULL);
	if (rc != 0) {
		printf("  cellMsgDialogOpen2 failed: 0x%08x\n", (unsigned)rc);
		cellSysutilUnregisterCallback(0);
		return 1;
	}
	printf("  progress dialog opened\n");

	/* Increment the progress bar from 0 to 100. */
	cellMsgDialogProgressBarSetMsg(CELL_MSGDIALOG_PROGRESSBAR_INDEX_SINGLE,
	                               "Starting...");
	cellMsgDialogProgressBarReset(CELL_MSGDIALOG_PROGRESSBAR_INDEX_SINGLE);

	for (int pct = 0; pct <= 100 && !g_dialog_done; pct += 10) {
		printf("  progress: %d%%\n", pct);

		char msg[CELL_MSGDIALOG_PROGRESSBAR_STRING_SIZE];
		snprintf(msg, sizeof(msg), "Progress: %d%%", pct + 10);

		cellMsgDialogProgressBarSetMsg(
			CELL_MSGDIALOG_PROGRESSBAR_INDEX_SINGLE, msg);
		cellMsgDialogProgressBarInc(
			CELL_MSGDIALOG_PROGRESSBAR_INDEX_SINGLE, 10);

		/* Pump callbacks at 10Hz while waiting. */
		for (int j = 0; j < 10 && !g_dialog_done; j++) {
			cellSysutilCheckCallback();
			usleep(100 * 1000);
		}
	}

	/* If user didn't dismiss, wait a bit then abort. */
	if (!g_dialog_done) {
		printf("  progress complete — waiting 3s for user review\n");
		for (int j = 0; j < 30 && !g_dialog_done; j++) {
			cellSysutilCheckCallback();
			usleep(100 * 1000);
		}
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
