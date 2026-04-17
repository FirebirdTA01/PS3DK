/*
 * hello-ppu-sysparam — exercise cellSysutilGetSystemParam{Int,String}.
 * Third Phase 6 backfill sample.  Queries a handful of system
 * parameters and prints them to TTY.log.  No UI — runs to completion
 * in a few milliseconds.
 *
 * ELF imports Sony FNIDs:
 *   cellSysutilGetSystemParamInt    0x40e895d3
 *   cellSysutilGetSystemParamString 0x938013a0
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include <sys/process.h>

#include <cell/sysutil.h>
#include <cell/sysutil_sysparam.h>

SYS_PROCESS_PARAM(1001, 0x10000);

static const char *lang_name(int id)
{
	switch (id) {
	case CELL_SYSUTIL_LANG_JAPANESE:       return "Japanese";
	case CELL_SYSUTIL_LANG_ENGLISH_US:     return "English (US)";
	case CELL_SYSUTIL_LANG_FRENCH:         return "French";
	case CELL_SYSUTIL_LANG_SPANISH:        return "Spanish";
	case CELL_SYSUTIL_LANG_GERMAN:         return "German";
	case CELL_SYSUTIL_LANG_ITALIAN:        return "Italian";
	case CELL_SYSUTIL_LANG_DUTCH:          return "Dutch";
	case CELL_SYSUTIL_LANG_PORTUGUESE_PT:  return "Portuguese (PT)";
	case CELL_SYSUTIL_LANG_RUSSIAN:        return "Russian";
	case CELL_SYSUTIL_LANG_KOREAN:         return "Korean";
	case CELL_SYSUTIL_LANG_CHINESE_T:      return "Chinese (Traditional)";
	case CELL_SYSUTIL_LANG_CHINESE_S:      return "Chinese (Simplified)";
	case CELL_SYSUTIL_LANG_FINNISH:        return "Finnish";
	case CELL_SYSUTIL_LANG_SWEDISH:        return "Swedish";
	case CELL_SYSUTIL_LANG_DANISH:         return "Danish";
	case CELL_SYSUTIL_LANG_NORWEGIAN:      return "Norwegian";
	case CELL_SYSUTIL_LANG_POLISH:         return "Polish";
	case CELL_SYSUTIL_LANG_PORTUGUESE_BR:  return "Portuguese (BR)";
	case CELL_SYSUTIL_LANG_ENGLISH_GB:     return "English (GB)";
	case CELL_SYSUTIL_LANG_TURKISH:        return "Turkish";
	default:                               return "unknown";
	}
}

static void query_int(int id, const char *label)
{
	int value = -1;
	int rc = cellSysutilGetSystemParamInt(id, &value);
	if (rc == 0) {
		printf("  %-32s = %d\n", label, value);
	} else {
		printf("  %-32s = <error 0x%08x>\n", label, (unsigned)rc);
	}
}

int main(int argc, char **argv)
{
	(void)argc;
	(void)argv;

	printf("hello-ppu-sysparam: querying system parameters\n");

	/* Numeric params */
	int lang = -1;
	if (cellSysutilGetSystemParamInt(CELL_SYSUTIL_SYSTEMPARAM_ID_LANG, &lang) == 0) {
		printf("  language                         = %d (%s)\n", lang, lang_name(lang));
	}

	query_int(CELL_SYSUTIL_SYSTEMPARAM_ID_ENTER_BUTTON_ASSIGN,  "enter button assign");
	query_int(CELL_SYSUTIL_SYSTEMPARAM_ID_DATE_FORMAT,          "date format");
	query_int(CELL_SYSUTIL_SYSTEMPARAM_ID_TIME_FORMAT,          "time format");
	query_int(CELL_SYSUTIL_SYSTEMPARAM_ID_TIMEZONE,             "timezone (min from UTC)");
	query_int(CELL_SYSUTIL_SYSTEMPARAM_ID_SUMMERTIME,           "summertime");
	query_int(CELL_SYSUTIL_SYSTEMPARAM_ID_GAME_PARENTAL_LEVEL,  "parental level");
	query_int(CELL_SYSUTIL_SYSTEMPARAM_ID_PAD_RUMBLE,           "pad rumble");

	/* String params */
	char nickname[CELL_SYSUTIL_SYSTEMPARAM_NICKNAME_SIZE] = {0};
	int rc = cellSysutilGetSystemParamString(
		CELL_SYSUTIL_SYSTEMPARAM_ID_NICKNAME,
		nickname, sizeof(nickname));
	if (rc == 0) {
		printf("  nickname                         = \"%s\"\n", nickname);
	} else {
		printf("  nickname                         = <error 0x%08x>\n", (unsigned)rc);
	}

	char username[CELL_SYSUTIL_SYSTEMPARAM_CURRENT_USERNAME_SIZE] = {0};
	rc = cellSysutilGetSystemParamString(
		CELL_SYSUTIL_SYSTEMPARAM_ID_CURRENT_USERNAME,
		username, sizeof(username));
	if (rc == 0) {
		printf("  current username                 = \"%s\"\n", username);
	} else {
		printf("  current username                 = <error 0x%08x>\n", (unsigned)rc);
	}

	printf("result: PASS\n");
	return 0;
}
