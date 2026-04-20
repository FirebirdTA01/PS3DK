/*! \file cell/sysutil_gamecontent.h
 \brief Sony-SDK-source-compat cellGame* API — Phase 6 forwarder.

  Sony's sysutil_gamecontent.h + sysutil_game_common.h surface over
  PSL1GHT's libsysutil.a (which embeds Sony FNIDs under renamed
  sysGame* wrappers).  Source written against Sony's reference headers
  compiles unchanged against this header.

  Verified FNIDs imported by user code:
    cellGameBootCheck                     0xf52639ea
    cellGameDataCheck                     0xdb9819f3
    cellGamePatchCheck                    0xf8115d69
    cellGameCreateGameData                0x42a2e133
    cellGameGetParamInt                   0xb7a45caf
    cellGameGetParamString                0x3a5d726a
    cellGameGetSizeKB                     0xef9d42d5
    cellGameSetParamString                0xdaa5cd20
    cellGameGetDiscContentInfoUpdatePath  0x2a8e6b92
    cellGameContentPermit                 0x70acec67
    cellGameContentErrorDialog            0xb0a1f8c6
    cellGameThemeInstall                  0xd24e3928
    cellGameThemeInstallFromBuffer        0x87406734
    cellGameGetLocalWebContentPath        0xa80bf223
    cellGameDeleteGameData                0xb367c6e3
    cellGameRegisterDiscChangeCallback    0xef9d42d5
    cellGameUnregisterDiscChangeCallback  0x3e22cb4b
*/

#ifndef __PSL1GHT_CELL_SYSUTIL_GAMECONTENT_H__
#define __PSL1GHT_CELL_SYSUTIL_GAMECONTENT_H__

#include <sysutil/game.h>
#include <ppu-types.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================
 * sysutil_game_common.h surface — return/error codes, sizes,
 * game-type enum.
 * ============================================================ */

#define CELL_GAME_RET_OK                0
#define CELL_GAME_RET_CANCEL            1
#define CELL_GAME_RET_NONE              2

#define CELL_GAME_ERROR_NOTFOUND        0x8002cb04
#define CELL_GAME_ERROR_BROKEN          0x8002cb05
#define CELL_GAME_ERROR_INTERNAL        0x8002cb06
#define CELL_GAME_ERROR_PARAM           0x8002cb07
#define CELL_GAME_ERROR_NOAPP           0x8002cb08
#define CELL_GAME_ERROR_ACCESS_ERROR    0x8002cb09
#define CELL_GAME_ERROR_NOSPACE         0x8002cb20
#define CELL_GAME_ERROR_NOTSUPPORTED    0x8002cb21
#define CELL_GAME_ERROR_FAILURE         0x8002cb22
#define CELL_GAME_ERROR_BUSY            0x8002cb23
#define CELL_GAME_ERROR_IN_SHUTDOWN     0x8002cb24
#define CELL_GAME_ERROR_INVALID_ID      0x8002cb25
#define CELL_GAME_ERROR_EXIST           0x8002cb26
#define CELL_GAME_ERROR_NOTPATCH        0x8002cb27
#define CELL_GAME_ERROR_INVALID_THEME_FILE  0x8002cb28
#define CELL_GAME_ERROR_BOOTPATH        0x8002cb50

enum {
	CELL_GAME_PATH_MAX              = 128,
	CELL_GAME_DIRNAME_SIZE          = 32,
	CELL_GAME_TITLE_SIZE            = 128,
	CELL_GAME_TITLEID_SIZE          = 10,
	CELL_GAME_HDDGAMEPATH_SIZE      = 128,
	CELL_GAME_THEMEFILENAME_SIZE    = 48,
};

enum {
	CELL_GAME_GAMETYPE_SYS      = 0,
	CELL_GAME_GAMETYPE_DISC     = 1,
	CELL_GAME_GAMETYPE_HDD      = 2,
	CELL_GAME_GAMETYPE_GAMEDATA = 3,
	CELL_GAME_GAMETYPE_HOME     = 4,
};

/* ============================================================
 * sysutil_gamecontent.h — attribute bits, theme options, result
 * codes, PARAM.SFO IDs, error-dialog types, resolution / sound /
 * disc-type enums.
 * ============================================================ */

#define CELL_GAME_ATTRIBUTE_PATCH                (1 << 0)
#define CELL_GAME_ATTRIBUTE_APP_HOME             (1 << 1)
#define CELL_GAME_ATTRIBUTE_DEBUG                (1 << 2)
#define CELL_GAME_ATTRIBUTE_XMBBUY               (1 << 3)
#define CELL_GAME_ATTRIBUTE_COMMERCE2_BROWSER    (1 << 4)
#define CELL_GAME_ATTRIBUTE_INVITE_MESSAGE       (1 << 5)
#define CELL_GAME_ATTRIBUTE_CUSTOM_DATA_MESSAGE  (1 << 6)
#define CELL_GAME_ATTRIBUTE_WEB_BROWSER          (1 << 8)

#define CELL_GAME_THEME_OPTION_NONE              0
#define CELL_GAME_THEME_OPTION_APPLY             (1 << 0)

#define CELL_GAME_CBRESULT_OK                    0
#define CELL_GAME_CBRESULT_OK_CANCEL             1

enum {
	CELL_GAME_SYSP_LANGUAGE_NUM         = 20,
	CELL_GAME_SYSP_TITLE_SIZE           = 128,
	CELL_GAME_SYSP_TITLEID_SIZE         = 10,
	CELL_GAME_SYSP_VERSION_SIZE         = 6,
	CELL_GAME_SYSP_PS3_SYSTEM_VER_SIZE  = 8,
	CELL_GAME_SYSP_APP_VER_SIZE         = 6,
};

enum {
	CELL_GAME_PARAMID_TITLE             = 0,
	CELL_GAME_PARAMID_TITLE_DEFAULT     = 1,
	CELL_GAME_PARAMID_TITLE_JAPANESE    = 2,
	CELL_GAME_PARAMID_TITLE_ENGLISH     = 3,
	CELL_GAME_PARAMID_TITLE_FRENCH      = 4,
	CELL_GAME_PARAMID_TITLE_SPANISH     = 5,
	CELL_GAME_PARAMID_TITLE_GERMAN      = 6,
	CELL_GAME_PARAMID_TITLE_ITALIAN     = 7,
	CELL_GAME_PARAMID_TITLE_DUTCH       = 8,
	CELL_GAME_PARAMID_TITLE_PORTUGUESE  = 9,
	CELL_GAME_PARAMID_TITLE_RUSSIAN     = 10,
	CELL_GAME_PARAMID_TITLE_KOREAN      = 11,
	CELL_GAME_PARAMID_TITLE_CHINESE_T   = 12,
	CELL_GAME_PARAMID_TITLE_CHINESE_S   = 13,
	CELL_GAME_PARAMID_TITLE_FINNISH     = 14,
	CELL_GAME_PARAMID_TITLE_SWEDISH     = 15,
	CELL_GAME_PARAMID_TITLE_DANISH      = 16,
	CELL_GAME_PARAMID_TITLE_NORWEGIAN   = 17,
	CELL_GAME_PARAMID_TITLE_POLISH      = 18,
	CELL_GAME_PARAMID_TITLE_PORTUGUESE_BRAZIL  = 19,
	CELL_GAME_PARAMID_TITLE_ENGLISH_UK  = 20,
	CELL_GAME_PARAMID_TITLE_TURKISH     = 21,

	CELL_GAME_PARAMID_TITLE_ID          = 100,
	CELL_GAME_PARAMID_VERSION           = 101,
	CELL_GAME_PARAMID_PARENTAL_LEVEL    = 102,
	CELL_GAME_PARAMID_RESOLUTION        = 103,
	CELL_GAME_PARAMID_SOUND_FORMAT      = 104,
	CELL_GAME_PARAMID_PS3_SYSTEM_VER    = 105,
	CELL_GAME_PARAMID_APP_VER           = 106,
};

enum {
	CELL_GAME_ERRDIALOG_BROKEN_GAMEDATA       = 0,
	CELL_GAME_ERRDIALOG_BROKEN_HDDGAME        = 1,
	CELL_GAME_ERRDIALOG_NOSPACE               = 2,
	CELL_GAME_ERRDIALOG_BROKEN_EXIT_GAMEDATA  = 100,
	CELL_GAME_ERRDIALOG_BROKEN_EXIT_HDDGAME   = 101,
	CELL_GAME_ERRDIALOG_NOSPACE_EXIT          = 102,
};

enum {
	CELL_GAME_RESOLUTION_1080   = 0x08,
	CELL_GAME_RESOLUTION_720    = 0x04,
	CELL_GAME_RESOLUTION_576SQ  = 0x20,
	CELL_GAME_RESOLUTION_576    = 0x02,
	CELL_GAME_RESOLUTION_480SQ  = 0x10,
	CELL_GAME_RESOLUTION_480    = 0x01,
};

enum {
	CELL_GAME_SOUNDFORMAT_71LPCM    = 0x10,
	CELL_GAME_SOUNDFORMAT_51LPCM    = 0x04,
	CELL_GAME_SOUNDFORMAT_51DDENC   = 0x102,
	CELL_GAME_SOUNDFORMAT_51DTSENC  = 0x202,
	CELL_GAME_SOUNDFORMAT_2LPCM     = 0x01,
};

enum {
	CELL_GAME_DISCTYPE_OTHER        = 0,
	CELL_GAME_DISCTYPE_PS3          = 1,
	CELL_GAME_DISCTYPE_PS2          = 2,
};

#define CELL_GAME_SIZEKB_NOTCALC            (-1)
#define CELL_GAME_THEMEINSTALL_BUFSIZE_MIN  4096

/* ============================================================
 * Sony struct types.  Layouts byte-identical to PSL1GHT's
 * sysGame* per the _Static_asserts below.
 * ============================================================ */

typedef struct CellGameContentSize {
	int hddFreeSizeKB;
	int sizeKB;
	int sysSizeKB;
} CellGameContentSize;

typedef struct CellGameSetInitParams {
	char title[CELL_GAME_SYSP_TITLE_SIZE];
	char titleId[CELL_GAME_SYSP_TITLEID_SIZE];
	char reserved0[2];
	char version[CELL_GAME_SYSP_VERSION_SIZE];
	char reserved1[66];
} CellGameSetInitParams;

_Static_assert(sizeof(CellGameContentSize)    == sizeof(sysGameContentSize),     "GameContentSize drift");
_Static_assert(sizeof(CellGameSetInitParams)  == sizeof(sysGameSetInitParams),   "GameSetInitParams drift");

/* ============================================================
 * Callback typedefs.
 * ============================================================ */

typedef void (*CellGameDiscEjectCallback)(void);
typedef void (*CellGameDiscInsertCallback)(unsigned int discType, char *titleId);
typedef int  (*CellGameThemeInstallCallback)(unsigned int fileOffset,
                                             unsigned int readSize, void *buf);

/* ============================================================
 * Function forwarders.
 * ============================================================ */

static inline int cellGameDataCheck(unsigned int type, const char *dirName,
                                    CellGameContentSize *size) {
	return (int)sysGameDataCheck((u32)type, dirName, (sysGameContentSize *)size);
}

static inline int cellGameBootCheck(unsigned int *type, unsigned int *attributes,
                                    CellGameContentSize *size, char *dirName) {
	return (int)sysGameBootCheck((u32 *)type, (u32 *)attributes,
	                             (sysGameContentSize *)size, dirName);
}

static inline int cellGamePatchCheck(CellGameContentSize *size, void *reserved) {
	return (int)sysGamePatchCheck((sysGameContentSize *)size, reserved);
}

static inline int cellGameCreateGameData(CellGameSetInitParams *init,
                                         char *tmp_contentInfoPath,
                                         char *tmp_usrdirPath) {
	return (int)sysGameCreateGameData((sysGameSetInitParams *)init,
	                                  tmp_contentInfoPath, tmp_usrdirPath);
}

static inline int cellGameGetParamInt(int id, int *value) {
	return (int)sysGameGetParamInt((s32)id, (s32 *)value);
}

static inline int cellGameGetParamString(int id, char *buf, unsigned int bufsize) {
	return (int)sysGameGetParamString((s32)id, buf, (u32)bufsize);
}

static inline int cellGameGetSizeKB(int *sizeKB) {
	return (int)sysGameGetSizeKB((s32 *)sizeKB);
}

static inline int cellGameSetParamString(int id, const char *buf) {
	return (int)sysGameSetParamString((s32)id, buf);
}

static inline int cellGameGetDiscContentInfoUpdatePath(char *updatePath) {
	return (int)sysGameGetDiscContentInfoUpdatePath(updatePath);
}

static inline int cellGameContentPermit(char *contentInfoPath, char *usrdirPath) {
	return (int)sysGameContentPermit(contentInfoPath, usrdirPath);
}

static inline int cellGameContentErrorDialog(int type, int errNeedSizeKB,
                                             const char *dirName) {
	return (int)sysGameContentErrorDialog((s32)type, (s32)errNeedSizeKB, dirName);
}

static inline int cellGameThemeInstall(const char *usrdirPath, const char *fileName,
                                       unsigned int option) {
	return (int)sysGameThemeInstall(usrdirPath, fileName, (u32)option);
}

static inline int cellGameThemeInstallFromBuffer(unsigned int fileSize,
                                                 unsigned int bufSize,
                                                 void *buf,
                                                 CellGameThemeInstallCallback cb,
                                                 unsigned int option) {
	return (int)sysGameThemeInstallFromBuffer((u32)fileSize, (u32)bufSize, buf,
	                                          (sysGameThemeInstallCallback)cb,
	                                          (u32)option);
}

static inline int cellGameGetLocalWebContentPath(char *contentPath) {
	return (int)sysGameGetLocalWebContentPath(contentPath);
}

static inline int cellGameDeleteGameData(const char *dirName) {
	return (int)sysGameDeleteGameData(dirName);
}

static inline int cellGameRegisterDiscChangeCallback(CellGameDiscEjectCallback funcEject,
                                                     CellGameDiscInsertCallback funcInsert) {
	return (int)sysGameRegisterDiscChangeCallback((sysGameDiscEjectCallback)funcEject,
	                                              (sysGameDiscInsertCallback)funcInsert);
}

static inline int cellGameUnregisterDiscChangeCallback(void) {
	return (int)sysGameUnregisterDiscChangeCallback();
}

#ifdef __cplusplus
}
#endif

#endif /* __PSL1GHT_CELL_SYSUTIL_GAMECONTENT_H__ */
