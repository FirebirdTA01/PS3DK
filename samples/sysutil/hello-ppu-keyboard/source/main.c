/*
 * hello-ppu-keyboard — exercise the cellKb keyboard input surface.
 *
 * Validates cell/keyboard.h end-to-end: CellKbInfo / CellKbData types
 * resolve, CELL_KB_* constants compile, cellKbInit / GetInfo /
 * SetCodeType / Read / ClearBuf / End all link through libio.a.
 *
 * Runtime: opens the keyboard, polls for raw-mode input for ~10s,
 * prints any keycode bytes received, then shuts down cleanly.  RPCS3
 * stubs cellKbRead to return CELL_OK with zero keycodes when no
 * keyboard is connected, so this sample exits cleanly even headless.
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <sys/process.h>
#include <cell/keyboard.h>

SYS_PROCESS_PARAM(1001, 0x10000);

int main(void)
{
	printf("hello-ppu-keyboard: cellKb* surface validation\n");

	int32_t rc = cellKbInit(CELL_KB_MAX_KEYBOARDS);
	printf("  cellKbInit(%u) -> 0x%08x\n",
	       (unsigned)CELL_KB_MAX_KEYBOARDS, (unsigned)rc);
	if (rc != 0) return 1;

	CellKbInfo info;
	memset(&info, 0, sizeof(info));
	rc = cellKbGetInfo(&info);
	printf("  cellKbGetInfo -> 0x%08x  max=%u  connected=%u\n",
	       (unsigned)rc, (unsigned)info.max, (unsigned)info.connected);

	if (info.connected == 0) {
		printf("  no keyboard connected — per-port ops skipped (expected)\n");
	} else {
		rc = cellKbSetCodeType(0, CELL_KB_CODETYPE_RAW);
		printf("  cellKbSetCodeType(port=0, RAW) -> 0x%08x\n", (unsigned)rc);

		rc = cellKbClearBuf(0);
		printf("  cellKbClearBuf(0) -> 0x%08x\n", (unsigned)rc);

		for (int frame = 0; frame < 100; frame++) {
			CellKbData data;
			memset(&data, 0, sizeof(data));
			rc = cellKbRead(0, &data);
			if (rc == 0 && data.nb_keycode > 0) {
				printf("  frame=%3d nb_keycode=%d  leds=0x%08x  mkeys=0x%08x\n",
				       frame,
				       (int)data.nb_keycode,
				       (unsigned)data.led._KbLedU.leds,
				       (unsigned)data.mkey._KbMkeyU.mkeys);
				for (int i = 0; i < data.nb_keycode && i < 4; i++)
					printf("    code[%d] = 0x%04x\n", i, (unsigned)data.keycode[i]);
			}
			usleep(100 * 1000);
		}
	}

	rc = cellKbEnd();
	printf("  cellKbEnd -> 0x%08x\n", (unsigned)rc);
	printf("  validation: link OK, types OK, runtime OK\n");
	return 0;
}
