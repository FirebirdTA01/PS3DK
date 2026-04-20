/*
 * hello-ppu-gamedata — cellGameBootCheck + cellGameGetParamString probes.
 *
 * cellGameBootCheck reports how the game was launched (disc, HDD,
 * patch, gamedata, app_home) and what attributes (debug, PS Store
 * relaunch, XMB-invite launch, etc.) came in on that launch.
 * cellGameGetParamString reads entries from the running PARAM.SFO —
 * title-ID, version, per-language titles.
 *
 * ELF imports FNIDs:
 *   cellGameBootCheck      0xf52639ea
 *   cellGameGetParamString 0x3a5d726a
 *   cellGameContentPermit  0x70acec67
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <sys/process.h>

#include <cell/sysutil_gamecontent.h>

SYS_PROCESS_PARAM(1001, 0x10000);

static const char *game_type_name(unsigned int t)
{
	switch (t) {
	case CELL_GAME_GAMETYPE_SYS:      return "SYS";
	case CELL_GAME_GAMETYPE_DISC:     return "DISC";
	case CELL_GAME_GAMETYPE_HDD:      return "HDD";
	case CELL_GAME_GAMETYPE_GAMEDATA: return "GAMEDATA";
	case CELL_GAME_GAMETYPE_HOME:     return "HOME";
	default:                          return "unknown";
	}
}

int main(int argc, char **argv)
{
	(void)argc;
	(void)argv;

	printf("hello-ppu-gamedata: cellGameBootCheck + GetParamString\n");

	unsigned int type       = 0;
	unsigned int attributes = 0;
	char dirName[CELL_GAME_DIRNAME_SIZE] = {0};
	CellGameContentSize size;
	memset(&size, 0, sizeof(size));

	int rc = cellGameBootCheck(&type, &attributes, &size, dirName);
	printf("  cellGameBootCheck returned: 0x%08x\n", (unsigned)rc);
	if (rc == CELL_GAME_RET_OK) {
		printf("    type         = %u (%s)\n", type, game_type_name(type));
		printf("    attributes   = 0x%08x\n", attributes);
		printf("    dirName      = \"%s\"\n", dirName);
		printf("    hddFreeSizeKB= %d\n", size.hddFreeSizeKB);
		printf("    sizeKB       = %d\n", size.sizeKB);
		printf("    sysSizeKB    = %d\n", size.sysSizeKB);
	}

	/* cellGameContentPermit must be called after BootCheck to finalise
	 * the content mount before any GetParamString calls on the running
	 * app's PARAM.SFO.  It also gives us the final content-info +
	 * usrdir paths. */
	char contentInfoPath[CELL_GAME_PATH_MAX] = {0};
	char usrdirPath[CELL_GAME_PATH_MAX]      = {0};
	rc = cellGameContentPermit(contentInfoPath, usrdirPath);
	printf("  cellGameContentPermit returned: 0x%08x\n", (unsigned)rc);
	if (rc == CELL_GAME_RET_OK) {
		printf("    contentInfoPath = \"%s\"\n", contentInfoPath);
		printf("    usrdirPath      = \"%s\"\n", usrdirPath);
	}

	char titleId[CELL_GAME_TITLEID_SIZE + 1] = {0};
	rc = cellGameGetParamString(CELL_GAME_PARAMID_TITLE_ID,
	                            titleId, CELL_GAME_TITLEID_SIZE);
	printf("  cellGameGetParamString(TITLE_ID) = 0x%08x \"%s\"\n",
	       (unsigned)rc, titleId);

	char title[CELL_GAME_TITLE_SIZE + 1] = {0};
	rc = cellGameGetParamString(CELL_GAME_PARAMID_TITLE,
	                            title, CELL_GAME_TITLE_SIZE);
	printf("  cellGameGetParamString(TITLE)    = 0x%08x \"%s\"\n",
	       (unsigned)rc, title);

	int parentalLevel = -1;
	rc = cellGameGetParamInt(CELL_GAME_PARAMID_PARENTAL_LEVEL, &parentalLevel);
	printf("  cellGameGetParamInt(PARENTAL)    = 0x%08x value=%d\n",
	       (unsigned)rc, parentalLevel);

	printf("result: PASS\n");
	return 0;
}
