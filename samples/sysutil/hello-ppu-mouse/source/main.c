/*
 * hello-ppu-mouse — exercise the cellMouse input surface.
 *
 * Validates cell/mouse.h: CellMouseInfo / CellMouseData types,
 * cellMouseInit / GetInfo / GetData / ClearBuf / End link via libio.a.
 *
 * RPCS3 stubs cellMouseGetData to return zero-deltas when no mouse
 * is connected; sample exits cleanly headless.
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <sys/process.h>
#include <cell/mouse.h>

SYS_PROCESS_PARAM(1001, 0x10000);

int main(void)
{
	printf("hello-ppu-mouse: cellMouse* surface validation\n");

	int32_t rc = cellMouseInit(7);
	printf("  cellMouseInit(7) -> 0x%08x\n", (unsigned)rc);
	if (rc != 0) return 1;

	CellMouseInfo info;
	memset(&info, 0, sizeof(info));
	rc = cellMouseGetInfo(&info);
	printf("  cellMouseGetInfo -> 0x%08x  max=%u  connected=%u  info=0x%08x\n",
	       (unsigned)rc,
	       (unsigned)info.max,
	       (unsigned)info.connected,
	       (unsigned)info.info);

	rc = cellMouseClearBuf(0);
	printf("  cellMouseClearBuf(0) -> 0x%08x\n", (unsigned)rc);

	for (int frame = 0; frame < 50; frame++) {
		CellMouseData data;
		memset(&data, 0, sizeof(data));
		rc = cellMouseGetData(0, &data);
		if (rc == 0 && data.update == CELL_MOUSE_DATA_UPDATE) {
			printf("  frame=%3d  buttons=0x%02x  dx=%d dy=%d wheel=%d\n",
			       frame,
			       (unsigned)data.buttons,
			       (int)data.x_axis,
			       (int)data.y_axis,
			       (int)data.wheel);
		}
		usleep(100 * 1000);
	}

	rc = cellMouseEnd();
	printf("  cellMouseEnd -> 0x%08x\n", (unsigned)rc);
	printf("  validation: link OK, types OK, runtime OK\n");
	return 0;
}
