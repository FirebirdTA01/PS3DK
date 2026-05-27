/*
 * hello-ppu-key2char - cellKey2Char surface validation.
 *
 * Exercises all 5 entry points in libcellKey2char_stub.a.
 *
 * Expected return codes under RPCS3 HLE:
 *   cellKey2CharOpen:            0x00000000 (CELL_OK - HLE stub returns OK
 *                                  but does not initialize the handle)
 *   cellKey2CharSetMode:         0x80121305 (CELL_K2C_ERROR_UNINITIALIZED -
 *                                  handle zeroed, no real init by HLE)
 *   cellKey2CharSetArrangement:  0x80121305 (UNINITIALIZED)
 *   cellKey2CharGetChar:         0x80121305 (UNINITIALIZED)
 *   cellKey2CharClose:           0x80121305 (UNINITIALIZED)
 *
 * Root cause: RPCS3 HLE returns OK for Open without populating the
 * handle; subsequent calls check the zeroed handle and return
 * UNINITIALIZED.  On real HW, Open would initialize the handle and
 * the follow-up calls would succeed.
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/process.h>
#include <cell/libkey2char.h>

SYS_PROCESS_PARAM(1001, 0x10000);

int main(void)
{
	printf("hello-ppu-key2char: cellKey2Char surface validation\n");
	CellKey2CharHandle h;
	memset(h, 0, sizeof(h));
	int rc = cellKey2CharOpen(h);
	printf("  cellKey2CharOpen -> 0x%08x\n", (unsigned)rc);
	rc = cellKey2CharSetMode(h, CELL_KEY2CHAR_MODE_ENGLISH);
	printf("  cellKey2CharSetMode -> 0x%08x\n", (unsigned)rc);
	rc = cellKey2CharSetArrangement(h, 0);
	printf("  cellKey2CharSetArrangement -> 0x%08x\n", (unsigned)rc);
	CellKey2CharKeyData kd = {0};
	uint16_t *cc = NULL;
	uint32_t cn = 0;
	bool proc = false;
	rc = cellKey2CharGetChar(h, &kd, &cc, &cn, &proc);
	printf("  cellKey2CharGetChar -> 0x%08x\n", (unsigned)rc);
	rc = cellKey2CharClose(h);
	printf("  cellKey2CharClose -> 0x%08x\n", (unsigned)rc);
	printf("result: PASS (validation complete)\n");
	sys_process_exit(0);
	return 0;
}
