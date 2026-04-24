/*! \file cell/sysutil_sysparam.h
 \brief Cell-SDK forwarder for cellSysutilGetSystemParam{Int,String}.

  Thin static-inline forwarders over PSL1GHT's sysUtilGetSystemParamInt
  / String.  Verified: PSL1GHT's *_fnid symbols hold the reference
  FNIDs —
    sysUtilGetSystemParamInt_fnid    = 0x40e895d3  (cellSysutilGetSystemParamInt)
    sysUtilGetSystemParamString_fnid = 0x938013a0  (cellSysutilGetSystemParamString)

  CELL_SYSUTIL_SYSTEMPARAM_ID_* constants are bit-identical to
  PSL1GHT's SYSUTIL_SYSTEMPARAM_ID_* set; aliased here so reference-SDK
  source writes the identifiers the reference headers document.

  Not covered — values that cellSysutilGetSystemParam{Int,String}
  returns (enum values for language, button-assign, date format, etc.)
  have dedicated CELL_SYSUTIL_* constants in the reference header but
  are just documentation-facing integers.  A full translation would
  blow up this header; the important ones (LANG_*, ENTER_BUTTON_ASSIGN_*)
  are included here so user code isn't forced to recall magic numbers.
*/

#ifndef __PSL1GHT_CELL_SYSUTIL_SYSPARAM_H__
#define __PSL1GHT_CELL_SYSUTIL_SYSPARAM_H__

#include <cell/sysutil.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Param-ID constants - direct numeric values so this header doesn't
 * need <sysutil/sysutil.h>.  Values are the stable cell-SDK IDs the
 * SPRX recognizes (0x0111..0x0157 for the enumerated PSN/XMB
 * settings; 0x0131..0x0157 for per-user and peripheral prefs). */
#define CELL_SYSUTIL_SYSTEMPARAM_ID_LANG                                0x0111
#define CELL_SYSUTIL_SYSTEMPARAM_ID_ENTER_BUTTON_ASSIGN                 0x0112
#define CELL_SYSUTIL_SYSTEMPARAM_ID_NICKNAME                            0x0113
#define CELL_SYSUTIL_SYSTEMPARAM_ID_DATE_FORMAT                         0x0114
#define CELL_SYSUTIL_SYSTEMPARAM_ID_TIME_FORMAT                         0x0115
#define CELL_SYSUTIL_SYSTEMPARAM_ID_TIMEZONE                            0x0116
#define CELL_SYSUTIL_SYSTEMPARAM_ID_SUMMERTIME                          0x0117
#define CELL_SYSUTIL_SYSTEMPARAM_ID_GAME_PARENTAL_LEVEL                 0x0121
#define CELL_SYSUTIL_SYSTEMPARAM_ID_GAME_PARENTAL_LEVEL0_RESTRICT       0x0123
#define CELL_SYSUTIL_SYSTEMPARAM_ID_INTERNET_BROWSER_START_RESTRICT     0x0125
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

/* Language enum values — what LANG returns.  Direct numeric values
 * so this header doesn't need PSL1GHT's SYSUTIL_LANG_* symbols in
 * scope. */
#define CELL_SYSUTIL_LANG_JAPANESE          0
#define CELL_SYSUTIL_LANG_ENGLISH_US        1
#define CELL_SYSUTIL_LANG_FRENCH            2
#define CELL_SYSUTIL_LANG_SPANISH           3
#define CELL_SYSUTIL_LANG_GERMAN            4
#define CELL_SYSUTIL_LANG_ITALIAN           5
#define CELL_SYSUTIL_LANG_DUTCH             6
#define CELL_SYSUTIL_LANG_PORTUGUESE_PT     7
#define CELL_SYSUTIL_LANG_RUSSIAN           8
#define CELL_SYSUTIL_LANG_KOREAN            9
#define CELL_SYSUTIL_LANG_CHINESE_T         10
#define CELL_SYSUTIL_LANG_CHINESE_S         11
#define CELL_SYSUTIL_LANG_FINNISH           12
#define CELL_SYSUTIL_LANG_SWEDISH           13
#define CELL_SYSUTIL_LANG_DANISH            14
#define CELL_SYSUTIL_LANG_NORWEGIAN         15
#define CELL_SYSUTIL_LANG_POLISH            16
#define CELL_SYSUTIL_LANG_PORTUGUESE_BR     17
#define CELL_SYSUTIL_LANG_ENGLISH_GB        18
#define CELL_SYSUTIL_LANG_TURKISH           19

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

/* PSL1GHT-side entry points declared locally so this header doesn't
 * need <sysutil/sysutil.h> (which pollutes the global namespace with
 * the `sysutilCallback` typedef and clashes with reference-SDK
 * sample code). */
extern int sysUtilGetSystemParamInt(int id, int *value);
extern int sysUtilGetSystemParamString(int id, char *buf, unsigned int bufsize);

/* Function forwarders. */
static inline int cellSysutilGetSystemParamInt(int id, int *value) {
	return sysUtilGetSystemParamInt(id, value);
}

static inline int cellSysutilGetSystemParamString(int id, char *buf, unsigned int bufsize) {
	return sysUtilGetSystemParamString(id, buf, bufsize);
}

#ifdef __cplusplus
}
#endif

#endif /* __PSL1GHT_CELL_SYSUTIL_SYSPARAM_H__ */
