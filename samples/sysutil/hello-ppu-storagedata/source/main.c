/*
 * hello-ppu-storagedata — drive cellStorageDataImport / Export /
 * ImportMove with sentinel paths.  No removable media is required;
 * RPCS3 returns ACCESS_ERROR / PARAM and we just verify the symbols
 * link and the SPRX dispatch path is reachable.
 *
 * For real-world use, srcMediaFile / dstHddDir / dstMediaDir / etc.
 * point at /dev_usb000 (USB stick) and /dev_hdd0/.../USRDIR/ paths,
 * and the title parameter shows up in the XMB copy dialog.
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <sys/memory.h>
#include <sys/process.h>
#include <cell/sysutil.h>
#include <cell/sysutil_storagedata.h>

SYS_PROCESS_PARAM(1001, 0x10000);

#define STORAGE_HEAP_BYTES (4 * 1024 * 1024)

static volatile int g_finish_count = 0;

static void on_finish(int result, void *userdata)
{
	(void)userdata;
	g_finish_count++;
	const char *kind = result == CELL_STORAGEDATA_RET_OK     ? "OK"
	                : result == CELL_STORAGEDATA_RET_CANCEL ? "CANCEL"
	                : "?";
	printf("  finish-cb: result=%s (rc=0x%08x)\n", kind, (unsigned)result);
}

int main(void)
{
	printf("hello-ppu-storagedata: cellStorageData{Import,Export,ImportMove}\n");

	int rc = cellSysutilRegisterCallback(0, NULL, NULL);
	(void)rc;

	sys_memory_container_t mem = 0;
	rc = sysMemContainerCreate(&mem, STORAGE_HEAP_BYTES);
	printf("  sysMemContainerCreate -> 0x%08x\n", (unsigned)rc);
	if (rc != 0) return 1;

	CellStorageDataSetParam param;
	memset(&param, 0, sizeof(param));
	param.fileSizeMax = 1024 * 1024;
	param.title = (char *)"PS3TC StorageData smoke";

	char src_media[]  = "/dev_usb000/PS3TC/IMPORT.BIN";
	char dst_hdd[]    = "/dev_hdd0/game/PS3TC0001/USRDIR";
	char src_hdd[]    = "/dev_hdd0/game/PS3TC0001/USRDIR/EXPORT.BIN";
	char dst_media[]  = "/dev_usb000/PS3TC";

	rc = cellStorageDataImport(CELL_STORAGEDATA_VERSION_CURRENT,
	                           src_media, dst_hdd,
	                           &param, on_finish, mem, NULL);
	printf("  cellStorageDataImport -> 0x%08x\n", (unsigned)rc);

	rc = cellStorageDataExport(CELL_STORAGEDATA_VERSION_CURRENT,
	                           src_hdd, dst_media,
	                           &param, on_finish, mem, NULL);
	printf("  cellStorageDataExport -> 0x%08x\n", (unsigned)rc);

	rc = cellStorageDataImportMove(CELL_STORAGEDATA_VERSION_CURRENT,
	                               src_media, dst_hdd,
	                               &param, on_finish, mem, NULL);
	printf("  cellStorageDataImportMove -> 0x%08x\n", (unsigned)rc);

	/* drain any callbacks that did fire (some RPCS3 builds dispatch
	 * the finish-cb with ACCESS_ERROR rather than failing synchronously) */
	for (int i = 0; i < 20; i++) {
		cellSysutilCheckCallback();
		usleep(100 * 1000);
	}

	cellSysutilUnregisterCallback(0);
	sysMemContainerDestroy(mem);
	printf("  finish callbacks: %d\n", g_finish_count);
	return 0;
}
