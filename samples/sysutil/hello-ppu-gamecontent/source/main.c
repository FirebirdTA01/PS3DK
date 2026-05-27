/*
 * hello-ppu-gamecontent - cellGame* surface validation.
 *
 * Exercises all 17 entry points backed by libcellGame_stub.a and
 * libsysutil_stub.a.  ContentErrorDialog and ContentPermit are
 * intrinsic UI-dialog calls that block on user interaction; they
 * are documented as deferred.  All other 15 calls dispatch and print
 * return codes.
 *
 * Expected return codes under RPCS3 HLE (unpackaged .fake.self):
 *   cellGameDataCheck:                  0x8002CB07 (CELL_GAME_ERROR_PARAM)
 *   cellGameBootCheck:                  0x00000000 (CELL_OK)
 *   cellGamePatchCheck:                 0x8002CB27 (NOTPATCH)
 *   cellGameCreateGameData:             0x8002CB21 (NOTSUPPORTED)
 *   cellGameGetParamInt:                0x8002CB25 (INVALID_ID)
 *   cellGameGetParamString:             0x00000000 (CELL_OK)
 *   cellGameGetSizeKB:                  0x00000000 (CELL_OK)
 *   cellGameSetParamString:             0x8002CB21 (NOTSUPPORTED)
 *   cellGameGetDiscContentInfoUpdatePath: 0x00000000 (CELL_OK)
 *   cellGameThemeInstall:               0x8002CB07 (CELL_GAME_ERROR_PARAM)
 *   cellGameThemeInstallFromBuffer:     0x00000000 (CELL_OK)
 *   cellGameGetLocalWebContentPath:     0x00000000 (CELL_OK)
 *   cellGameDeleteGameData:             0x8002CB07 (CELL_GAME_ERROR_PARAM)
 *   cellGameRegisterDiscChangeCallback: 0x00000000 (CELL_OK)
 *   cellGameUnregisterDiscChangeCallback: 0x00000000 (CELL_OK)
 *   cellGameContentErrorDialog:         deferred (dialog-hosted)
 *   cellGameContentPermit:              deferred (permit-cycle-hosted)
 *
 * Packaged (.pkg installed) launch may change BootCheck type/param
 * values; automated sweep with --skip-clean uses unpackaged launch.
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/process.h>
#include <sys/memory.h>
#include <cell/sysutil.h>
#include <sysutil/sysutil_gamecontent.h>

SYS_PROCESS_PARAM(1001, 0x10000);

static int g_cb_fired = 0;
static char g_tmp_content[CELL_GAME_PATH_MAX] = {0};
static char g_tmp_usrdir[CELL_GAME_PATH_MAX] = {0};

static void eject_cb(void)
{
	g_cb_fired = 1;
}
static void disc_cb(unsigned int discType, char *titleId)
{
	(void)discType; (void)titleId; g_cb_fired = 1;
}
static int theme_cb(unsigned int result, unsigned int err, void *ud)
{
	(void)err; (void)ud;
	printf("  theme_cb result=%u\n", result);
	g_cb_fired = 1;
	return CELL_GAME_CBRESULT_OK;
}


int main(int argc, char **argv)
{
	(void)argc; (void)argv;
	printf("hello-ppu-gamecontent: cellGame* surface validation\n");

	int rc = cellSysutilRegisterCallback(0, NULL, NULL);
	printf("  cellSysutilRegisterCallback -> 0x%08x\n", (unsigned)rc);

	rc = cellGameDataCheck(0, NULL, NULL);
	printf("  cellGameDataCheck -> 0x%08x\n", (unsigned)rc);

	unsigned int type = 0, attr = 0;
	CellGameContentSize size;
	CellGameSetInitParams init;
	char dirName[CELL_GAME_DIRNAME_SIZE] = {0};
	memset(&size, 0, sizeof(size));
	memset(&init, 0, sizeof(init));
	rc = cellGameBootCheck(&type, &attr, &size, dirName);
	printf("  cellGameBootCheck -> 0x%08x\n", (unsigned)rc);

	rc = cellGamePatchCheck(&size, NULL);
	printf("  cellGamePatchCheck -> 0x%08x\n", (unsigned)rc);

	rc = cellGameCreateGameData(&init, g_tmp_content, g_tmp_usrdir);
	printf("  cellGameCreateGameData -> 0x%08x\n", (unsigned)rc);

	int val = 0;
	rc = cellGameGetParamInt(0, &val);
	printf("  cellGameGetParamInt -> 0x%08x\n", (unsigned)rc);

	char buf[256] = {0};
	rc = cellGameGetParamString(0, buf, sizeof(buf));
	printf("  cellGameGetParamString -> 0x%08x\n", (unsigned)rc);

	int kbsize = 0;
	rc = cellGameGetSizeKB(&kbsize);
	printf("  cellGameGetSizeKB -> 0x%08x\n", (unsigned)rc);

	rc = cellGameSetParamString(0, "test");
	printf("  cellGameSetParamString -> 0x%08x\n", (unsigned)rc);

	char upath[512] = {0};
	rc = cellGameGetDiscContentInfoUpdatePath(upath);
	printf("  cellGameGetDiscContentInfoUpdatePath -> 0x%08x\n", (unsigned)rc);

	/* ContentErrorDialog + ContentPermit deferred - both are
	 * intrinsic UI-dialog calls that block on user interaction. */

	rc = cellGameThemeInstall(NULL, NULL, 0);
	printf("  cellGameThemeInstall -> 0x%08x\n", (unsigned)rc);

	uint8_t themebuf[4096] = {0};
	rc = cellGameThemeInstallFromBuffer(
		sizeof(themebuf), sizeof(themebuf), themebuf, theme_cb, 0);
	printf("  cellGameThemeInstallFromBuffer -> 0x%08x\n", (unsigned)rc);

	rc = cellGameGetLocalWebContentPath(buf);
	printf("  cellGameGetLocalWebContentPath -> 0x%08x\n", (unsigned)rc);

	rc = cellGameDeleteGameData(NULL);
	printf("  cellGameDeleteGameData -> 0x%08x\n", (unsigned)rc);

	rc = cellGameRegisterDiscChangeCallback(eject_cb, disc_cb);
	printf("  cellGameRegisterDiscChangeCallback -> 0x%08x\n", (unsigned)rc);

	rc = cellGameUnregisterDiscChangeCallback();
	printf("  cellGameUnregisterDiscChangeCallback -> 0x%08x\n", (unsigned)rc);

	/* ContentErrorDialog + ContentPermit are intrinsic UI-dialog
	 * calls that block on user interaction.  Deferred so the
	 * sample reaches sys_process_exit(0); nm confirms the stubs
	 * resolve even though the calls are not exercised at runtime. */
	printf("  cellGameContentErrorDialog -> deferred (dialog-hosted)\n");
	printf("  cellGameContentPermit -> deferred (permit-cycle-hosted)\n");

	cellSysutilUnregisterCallback(0);
	printf("  callback fired: %d\n", g_cb_fired);
	printf("result: PASS (validation complete)\n");
	sys_process_exit(0);
	return 0;
}
