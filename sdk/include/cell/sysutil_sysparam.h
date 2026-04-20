/*! \file cell/sysutil_sysparam.h
 \brief Sony-named forwarder for cellSysutilGetSystemParam{Int,String}.

  Thin static-inline forwarders over PSL1GHT's sysUtilGetSystemParamInt
  / String.  Verified: PSL1GHT's *_fnid symbols hold Sony's FNIDs —
    sysUtilGetSystemParamInt_fnid    = 0x40e895d3  (cellSysutilGetSystemParamInt)
    sysUtilGetSystemParamString_fnid = 0x938013a0  (cellSysutilGetSystemParamString)

  CELL_SYSUTIL_SYSTEMPARAM_ID_* constants are bit-identical to
  PSL1GHT's SYSUTIL_SYSTEMPARAM_ID_* set; aliased here so Sony-SDK
  source writes the identifiers Sony's headers document.

  Not covered — values that cellSysutilGetSystemParam{Int,String}
  returns (enum values for language, button-assign, date format, etc.)
  have dedicated CELL_SYSUTIL_* constants in Sony's header but are
  just documentation-facing integers.  A full translation would blow
  up this header; the important ones (LANG_*, ENTER_BUTTON_ASSIGN_*)
  are included here so user code isn't forced to recall magic numbers.
*/

#ifndef __PSL1GHT_CELL_SYSUTIL_SYSPARAM_H__
#define __PSL1GHT_CELL_SYSUTIL_SYSPARAM_H__

#include <sysutil/sysutil.h>
#include <cell/sysutil.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Param-ID constants.  Values match PSL1GHT's SYSUTIL_SYSTEMPARAM_ID_*
 * 0x0111..0x0157; just re-exported under Sony's naming. */
#define CELL_SYSUTIL_SYSTEMPARAM_ID_LANG                                SYSUTIL_SYSTEMPARAM_ID_LANG
#define CELL_SYSUTIL_SYSTEMPARAM_ID_ENTER_BUTTON_ASSIGN                 SYSUTIL_SYSTEMPARAM_ID_ENTER_BUTTON_ASSIGN
#define CELL_SYSUTIL_SYSTEMPARAM_ID_NICKNAME                            SYSUTIL_SYSTEMPARAM_ID_NICKNAME
#define CELL_SYSUTIL_SYSTEMPARAM_ID_DATE_FORMAT                         SYSUTIL_SYSTEMPARAM_ID_DATE_FORMAT
#define CELL_SYSUTIL_SYSTEMPARAM_ID_TIME_FORMAT                         SYSUTIL_SYSTEMPARAM_ID_TIME_FORMAT
#define CELL_SYSUTIL_SYSTEMPARAM_ID_TIMEZONE                            SYSUTIL_SYSTEMPARAM_ID_TIMEZONE
#define CELL_SYSUTIL_SYSTEMPARAM_ID_SUMMERTIME                          SYSUTIL_SYSTEMPARAM_ID_SUMMERTIME
#define CELL_SYSUTIL_SYSTEMPARAM_ID_GAME_PARENTAL_LEVEL                 SYSUTIL_SYSTEMPARAM_ID_GAME_PARENTAL_LEVEL
#define CELL_SYSUTIL_SYSTEMPARAM_ID_GAME_PARENTAL_LEVEL0_RESTRICT       SYSUTIL_SYSTEMPARAM_ID_GAME_PARENTAL_LEVEL0_RESTRICT
#define CELL_SYSUTIL_SYSTEMPARAM_ID_INTERNET_BROWSER_START_RESTRICT     SYSUTIL_SYSTEMPARAM_ID_INTERNET_BROWSER_START_RESTRICT
#define CELL_SYSUTIL_SYSTEMPARAM_ID_CURRENT_USERNAME                    0x0131
#define CELL_SYSUTIL_SYSTEMPARAM_ID_CURRENT_USER_HAS_NP_ACCOUNT         0x0141
#define CELL_SYSUTIL_SYSTEMPARAM_ID_CAMERA_PLFREQ                       0x0151
#define CELL_SYSUTIL_SYSTEMPARAM_ID_PAD_RUMBLE                          0x0152
#define CELL_SYSUTIL_SYSTEMPARAM_ID_KEYBOARD_TYPE                       0x0153
#define CELL_SYSUTIL_SYSTEMPARAM_ID_JAPANESE_KEYBOARD_ENTRY_METHOD      0x0154
#define CELL_SYSUTIL_SYSTEMPARAM_ID_CHINESE_KEYBOARD_ENTRY_METHOD       0x0155
#define CELL_SYSUTIL_SYSTEMPARAM_ID_PAD_AUTOOFF                         0x0156
#define CELL_SYSUTIL_SYSTEMPARAM_ID_MAGNETOMETER                        0x0157

#define CELL_SYSUTIL_SYSTEMPARAM_NICKNAME_SIZE          128
#define CELL_SYSUTIL_SYSTEMPARAM_CURRENT_USERNAME_SIZE  64

/* Language enum values — what LANG returns.  PSL1GHT defines these as
 * SYSUTIL_LANG_*; aliasing to Sony names for source compat. */
#define CELL_SYSUTIL_LANG_JAPANESE          SYSUTIL_LANG_JAPANESE
#define CELL_SYSUTIL_LANG_ENGLISH_US        SYSUTIL_LANG_ENGLISH_US
#define CELL_SYSUTIL_LANG_FRENCH            SYSUTIL_LANG_FRENCH
#define CELL_SYSUTIL_LANG_SPANISH           SYSUTIL_LANG_SPANISH
#define CELL_SYSUTIL_LANG_GERMAN            SYSUTIL_LANG_GERMAN
#define CELL_SYSUTIL_LANG_ITALIAN           SYSUTIL_LANG_ITALIAN
#define CELL_SYSUTIL_LANG_DUTCH             SYSUTIL_LANG_DUTCH
#define CELL_SYSUTIL_LANG_PORTUGUESE_PT     SYSUTIL_LANG_PORTUGUESE_PT
#define CELL_SYSUTIL_LANG_RUSSIAN           SYSUTIL_LANG_RUSSIAN
#define CELL_SYSUTIL_LANG_KOREAN            SYSUTIL_LANG_KOREAN
#define CELL_SYSUTIL_LANG_CHINESE_T         SYSUTIL_LANG_CHINESE_T
#define CELL_SYSUTIL_LANG_CHINESE_S         SYSUTIL_LANG_CHINESE_S
#define CELL_SYSUTIL_LANG_FINNISH           SYSUTIL_LANG_FINNISH
#define CELL_SYSUTIL_LANG_SWEDISH           SYSUTIL_LANG_SWEDISH
#define CELL_SYSUTIL_LANG_DANISH            SYSUTIL_LANG_DANISH
#define CELL_SYSUTIL_LANG_NORWEGIAN         SYSUTIL_LANG_NORWEGIAN
#define CELL_SYSUTIL_LANG_POLISH            SYSUTIL_LANG_POLISH
#define CELL_SYSUTIL_LANG_PORTUGUESE_BR     SYSUTIL_LANG_PORTUGUESE_BR
#define CELL_SYSUTIL_LANG_ENGLISH_GB        SYSUTIL_LANG_ENGLISH_GB
#define CELL_SYSUTIL_LANG_TURKISH           SYSUTIL_LANG_TURKISH

/* Enter-button-assign values. */
#define CELL_SYSUTIL_ENTER_BUTTON_ASSIGN_CIRCLE   0
#define CELL_SYSUTIL_ENTER_BUTTON_ASSIGN_CROSS    1

/* Date-format values. */
#define CELL_SYSUTIL_DATE_FMT_YYYYMMDD            0
#define CELL_SYSUTIL_DATE_FMT_DDMMYYYY            1
#define CELL_SYSUTIL_DATE_FMT_MMDDYYYY            2

/* Time-format values. */
#define CELL_SYSUTIL_TIME_FMT_CLOCK12             0
#define CELL_SYSUTIL_TIME_FMT_CLOCK24             1

/* Function forwarders. */
static inline int cellSysutilGetSystemParamInt(int id, int *value) {
	return (int)sysUtilGetSystemParamInt((s32)id, (s32 *)value);
}

static inline int cellSysutilGetSystemParamString(int id, char *buf, unsigned int bufsize) {
	return (int)sysUtilGetSystemParamString((s32)id, buf, (u32)bufsize);
}

#ifdef __cplusplus
}
#endif

#endif /* __PSL1GHT_CELL_SYSUTIL_SYSPARAM_H__ */
