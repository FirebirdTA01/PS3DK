/*
 * hello-ppu-sysutil-queries - the small query APIs of four sysutil
 * libraries that now have headers:
 *
 *   cellSysutilGetLicenseArea           <sysutil/sysutil_licensearea.h>
 *   cellRemotePlayGetStatus / volume    <sysutil/sysutil_remoteplay.h>
 *   cellGameGetBootGameInfo, data paths <sysutil/sysutil_game_exec.h>
 *   cellSysconfBtGetDeviceList          <sysutil/sysutil_sysconf_ext.h>
 *
 * Each library's system module is loaded first and unloaded after.  The
 * program prints what each call returns; it PASSes when every call returns
 * and the license area is one of CELL_SYSUTIL_LICENSE_AREA_*.
 */
#include <stdio.h>
#include <string.h>
#include <sys/process.h>
#include <cell/sysmodule.h>
#include <sysutil/sysutil_licensearea.h>
#include <sysutil/sysutil_remoteplay.h>
#include <sysutil/sysutil_game_exec.h>
#include <sysutil/sysutil_sysconf_ext.h>

SYS_PROCESS_PARAM(1001, 0x10000);

static const char *area_name(int area)
{
	switch (area) {
	case CELL_SYSUTIL_LICENSE_AREA_J: return "Japan";
	case CELL_SYSUTIL_LICENSE_AREA_A: return "Americas";
	case CELL_SYSUTIL_LICENSE_AREA_E: return "Europe";
	case CELL_SYSUTIL_LICENSE_AREA_H: return "Asia";
	case CELL_SYSUTIL_LICENSE_AREA_K: return "Korea";
	case CELL_SYSUTIL_LICENSE_AREA_C: return "China";
	case CELL_SYSUTIL_LICENSE_AREA_OTHER: return "other";
	default: return NULL;
	}
}

static const char *remoteplay_name(int status)
{
	static const char *const names[] = {"loading", "wait", "running", "unloading", "fatal error", "premo end"};
	return status >= 0 && status <= CELL_REMOTEPLAY_STATUS_PREMOEND ? names[status] : "error";
}

static CellSysconfBtDeviceList bt;   /* 1536 bytes: static, not on the stack */

int main(void)
{
	static const unsigned short modules[] = {
		CELL_SYSMODULE_SYSUTIL_LICENSEAREA, CELL_SYSMODULE_SYSUTIL_GAME_EXEC,
		CELL_SYSMODULE_SYSUTIL_SYSCONF_EXT,
	};
	int ok = 1;

	printf("hello-ppu-sysutil-queries\n");
	for (unsigned i = 0; i < sizeof modules / sizeof modules[0]; i++) {
		int rc = cellSysmoduleLoadModule(modules[i]);
		printf("  load module 0x%04x -> 0x%08x\n", modules[i], (unsigned)rc);
	}

	int area = cellSysutilGetLicenseArea();
	const char *an = area_name(area);
	printf("  license area: %d (%s)\n", area, an ? an : "NOT A LICENSE AREA");
	if (!an)
		ok = 0;

	int status = cellRemotePlayGetStatus();
	float volume = -1.0f;
	int rc = cellRemotePlayGetComparativeVolume(&volume);
	printf("  remote play: status %d (%s), comparative volume rc 0x%08x value %.2f\n",
	       status, remoteplay_name(status), (unsigned)rc, (double)volume);

	unsigned int type = 0, execdata = 0;
	char dir[CELL_GAME_DIRNAME_SIZE];
	memset(dir, 0, sizeof dir);
	rc = cellGameGetBootGameInfo(&type, dir, &execdata);
	printf("  boot game info: rc 0x%08x type %u dir \"%s\" execdata 0x%08x\n",
	       (unsigned)rc, type, dir, execdata);

	char path[CELL_GAME_PATH_MAX];
	memset(path, 0, sizeof path);
	rc = cellGameGetHomeDataExportPath(path);
	printf("  HOME data export path: rc 0x%08x \"%s\"\n", (unsigned)rc, path);

	rc = cellSysconfBtGetDeviceList(&bt);
	int available = 0, named = 0;
	if (rc == 0)
		for (int i = 0; i < 16; i++) {
			if (bt.device[i].state == CELL_SYSCONF_BT_DEVICE_STATE_AVAILABLE) available++;
			if (bt.device[i].name[0]) named++;
		}
	printf("  bluetooth devices: rc 0x%08x, %d named, %d available\n", (unsigned)rc, named, available);

	for (unsigned i = 0; i < sizeof modules / sizeof modules[0]; i++)
		cellSysmoduleUnloadModule(modules[i]);

	printf("hello-ppu-sysutil-queries: %s\n", ok ? "PASS" : "FAIL");
	return ok ? 0 : 1;
}
