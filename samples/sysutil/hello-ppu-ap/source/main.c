/*
 * hello-ppu-ap — second non-PSL1GHT-backed sysutil sample.
 *
 * Validates the nidgen-archive pipeline for a second library.  Calls:
 *   - cellSysutilApGetRequiredMemSize   (cheap, no side effects)
 *   - cellSysutilApOff                  (idempotent when AP is not on)
 *
 * cellSysutilApOn is intentionally NOT called: it would need a real
 * sys_memory_container of CELL_SYSUTIL_AP_MEMORY_CONTAINER_SIZE (1 MiB)
 * and a populated CellSysutilApParam (game-mode WPA + SSID).  The link
 * still pulls __cellSysutilApOn into the ELF (whole-archive include), so
 * its FNID still appears in .rodata.sceFNID — proving the loader resolves
 * all three exports, not just the two we exercise.
 *
 * Imports FNIDs:
 *   cellSysutilApGetRequiredMemSize  0x9e67e0dd
 *   cellSysutilApOn                  0x3343824c   (declared, not called)
 *   cellSysutilApOff                 0x90c2bb19
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include <sys/process.h>

#include <cell/sysutil_ap.h>

SYS_PROCESS_PARAM(1001, 0x10000);

int main(int argc, char **argv)
{
	(void)argc;
	(void)argv;

	printf("hello-ppu-ap: stub-archive validation (libsysutil_ap)\n");

	int needed = cellSysutilApGetRequiredMemSize();
	printf("  cellSysutilApGetRequiredMemSize -> %d (0x%x)\n",
	       needed, (unsigned)needed);
	if (needed == CELL_SYSUTIL_AP_MEMORY_CONTAINER_SIZE) {
		printf("  matches CELL_SYSUTIL_AP_MEMORY_CONTAINER_SIZE (1 MiB)\n");
	} else if (needed > 0) {
		printf("  non-default container size — runtime returns %d, header says %d\n",
		       needed, CELL_SYSUTIL_AP_MEMORY_CONTAINER_SIZE);
	} else {
		printf("  negative return — likely error code 0x%08x\n",
		       (unsigned)needed);
	}

	int rc = cellSysutilApOff();
	printf("  cellSysutilApOff -> 0x%08x\n", (unsigned)rc);
	if (rc == CELL_SYSUTIL_AP_ERROR_NOT_INITIALIZED) {
		printf("  expected NOT_INITIALIZED (AP was never On)\n");
	}

	printf("hello-ppu-ap: done\n");
	return 0;
}
