/*
 * hello-ppu-syscache — mount the per-title cache area on /dev_hdd1
 * (cellSysCacheMount), read back the assigned path, optionally clear
 * it (cellSysCacheClear), then exit.
 *
 * Expected runtime behaviour on RPCS3:
 *   1. Mount returns either CELL_SYSCACHE_RET_OK_CLEARED (first run
 *      with this cacheId) or CELL_SYSCACHE_RET_OK_RELAYED (cache
 *      reused from prior run).
 *   2. Path printed is something like /dev_hdd1/cache/<cacheId>/.
 *   3. Re-mounting after Clear returns RET_OK_CLEARED.
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <sys/process.h>
#include <cell/sysutil_syscache.h>

SYS_PROCESS_PARAM(1001, 0x10000);

int main(void)
{
	printf("hello-ppu-syscache: testing cellSysCacheMount/Clear\n");

	CellSysCacheParam param;
	memset(&param, 0, sizeof(param));
	strncpy(param.cacheId, "PS3TC0001", CELL_SYSCACHE_ID_SIZE - 1);

	int rc = cellSysCacheMount(&param);
	const char *kind = "?";
	if (rc == CELL_SYSCACHE_RET_OK_CLEARED) kind = "CLEARED";
	else if (rc == CELL_SYSCACHE_RET_OK_RELAYED) kind = "RELAYED";
	printf("  mount(cacheId=%s) -> %s (rc=0x%08x)\n",
	       param.cacheId, kind, (unsigned)rc);
	if (rc < 0) {
		printf("  mount failed; aborting\n");
		return 1;
	}
	printf("  cache path: %s\n", param.getCachePath);

	rc = cellSysCacheClear();
	printf("  cellSysCacheClear -> rc=0x%08x\n", (unsigned)rc);

	memset(&param, 0, sizeof(param));
	strncpy(param.cacheId, "PS3TC0001", CELL_SYSCACHE_ID_SIZE - 1);
	rc = cellSysCacheMount(&param);
	printf("  remount after clear -> rc=0x%08x (expect CLEARED=%d)\n",
	       (unsigned)rc, CELL_SYSCACHE_RET_OK_CLEARED);
	return 0;
}
