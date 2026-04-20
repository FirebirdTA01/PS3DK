/*
 * hello-ppu-screenshot — first non-PSL1GHT-backed sysutil sample.
 *
 * Validates the nidgen-archive pipeline end-to-end:
 *   - cell/sysutil_screenshot.h compiles.
 *   - libsysutil_screenshot_stub.a (built by scripts/build-cell-stub-archives.sh)
 *     resolves cellScreenShotEnable / Disable / SetParameter at link time.
 *   - The four exports' FNIDs land in the ELF's .rodata.sceFNID:
 *       cellScreenShotEnable          0x9e33ab8f
 *       cellScreenShotDisable         0xfc6f4e74
 *       cellScreenShotSetParameter    0xd3ad63e4
 *       cellScreenShotSetOverlayImage 0x7a9c2243   (declared, not called)
 *   - cellScreenShotUtility appears in .rodata.sceResident so the PRX
 *     loader pulls in the SPRX module at runtime.
 *
 * Runtime expectation in RPCS3:
 *   1. Enable screenshots.
 *   2. Set photo title / game title / comment.
 *   3. Run a short sysutil callback loop (simulating gameplay frames).
 *   4. Disable screenshots and exit.
 *   No screenshot is actually triggered; the hardware path is XMB
 *   (Start+Select on a DUALSHOCK pad) and isn't part of the SDK API.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include <sys/process.h>

#include <cell/sysutil.h>
#include <cell/sysutil_screenshot.h>

SYS_PROCESS_PARAM(1001, 0x10000);

static volatile int g_exit_requested = 0;

static void on_sysutil_event(uint64_t status, uint64_t param, void *userdata)
{
	(void)param;
	(void)userdata;
	if (status == CELL_SYSUTIL_REQUEST_EXITGAME) {
		printf("  event: EXIT_GAME requested\n");
		g_exit_requested = 1;
	}
}

int main(int argc, char **argv)
{
	(void)argc;
	(void)argv;

	printf("hello-ppu-screenshot: stub-archive validation\n");

	int rc = cellSysutilRegisterCallback(0, on_sysutil_event, NULL);
	if (rc != 0) {
		printf("  cellSysutilRegisterCallback failed 0x%08x\n", (unsigned)rc);
		return 1;
	}

	rc = cellScreenShotEnable();
	printf("  cellScreenShotEnable -> 0x%08x\n", (unsigned)rc);
	if (rc != CELL_SCREENSHOT_OK) {
		printf("  enable failed; bailing\n");
		return 1;
	}

	CellScreenShotSetParam param = {
		.photo_title  = "PS3 Custom Toolchain test photo",
		.game_title   = "hello-ppu-screenshot",
		.game_comment = "first non-PSL1GHT-backed sysutil",
		.reserved     = NULL,
	};
	rc = cellScreenShotSetParameter(&param);
	printf("  cellScreenShotSetParameter -> 0x%08x\n", (unsigned)rc);

	/* Simulate ~60 frames of gameplay so a manual XMB screenshot
	 * (Start+Select on hardware) would catch the parameters above. */
	for (int frame = 0; frame < 60 && !g_exit_requested; frame++) {
		cellSysutilCheckCallback();
		usleep(16000);
	}

	rc = cellScreenShotDisable();
	printf("  cellScreenShotDisable -> 0x%08x\n", (unsigned)rc);

	printf("hello-ppu-screenshot: done\n");
	return 0;
}
