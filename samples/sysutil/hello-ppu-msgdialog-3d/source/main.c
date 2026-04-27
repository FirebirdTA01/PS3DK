/*
 * hello-ppu-msgdialog-3d — bring up the stereoscopic-3D
 * "please remove glasses" warning splash via
 * cellMsgDialogOpenSimulViewWarning.  Wait for the user to dismiss,
 * then exit.
 *
 * RPCS3 honours the dialog (renders a normal msg-dialog with the
 * SimulView text) and dispatches the button via the standard
 * CellMsgDialogCallback.
 */

#include <stdint.h>
#include <stdio.h>
#include <unistd.h>

#include <sys/process.h>
#include <cell/sysutil.h>
#include <cell/msg_dialog.h>

SYS_PROCESS_PARAM(1001, 0x10000);

static volatile int g_button = -1;

static void dialog_cb(int button_type, void *userdata)
{
	(void)userdata;
	g_button = button_type;
	printf("  dialog-cb: button=%d\n", button_type);
}

int main(void)
{
	printf("hello-ppu-msgdialog-3d: cellMsgDialogOpenSimulViewWarning\n");

	int rc = cellSysutilRegisterCallback(0, NULL, NULL);
	(void)rc;

	rc = cellMsgDialogOpenSimulViewWarning(dialog_cb, NULL, NULL);
	printf("  cellMsgDialogOpenSimulViewWarning -> 0x%08x\n", (unsigned)rc);

	for (int i = 0; i < 600 && g_button < 0; i++) {
		cellSysutilCheckCallback();
		usleep(100 * 1000);
	}

	cellSysutilUnregisterCallback(0);
	printf("  result: button=%d\n", g_button);
	return g_button >= 0 ? 0 : 1;
}
