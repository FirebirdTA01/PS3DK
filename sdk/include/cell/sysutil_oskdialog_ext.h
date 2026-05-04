/* cell/sysutil_oskdialog_ext.h — cellOskDialogExt* extended OSK API.
 *
 * Clean-room header.  These 17 entry points layer extended behaviour
 * on top of the base cellOskDialog surface: input-word confirm
 * filtering, hardware-keyboard event hooks, force-finish callbacks,
 * half-byte kana, clipboard, pointer display, and visual scaling /
 * colour / device-lock controls.
 *
 * Pointer fields crossing the SPRX boundary carry ATTRIBUTE_PRXPTR
 * for 4-byte effective-address storage on LP64 hosts.
 */
#ifndef __PS3DK_CELL_SYSUTIL_OSKDIALOG_EXT_H__
#define __PS3DK_CELL_SYSUTIL_OSKDIALOG_EXT_H__

#include <stdint.h>
#include <stdbool.h>

/* Pull in CellOskDialogPoint, CellOskDialogKeyMessage etc. from the
 * base oskdialog header. */
#include <cell/sysutil_oskdialog.h>

#ifdef __PPU__
#include <ppu-types.h>
#else
#ifndef ATTRIBUTE_PRXPTR
#define ATTRIBUTE_PRXPTR
#endif
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* ---- hook-type flags for keyboard event hook -------------------- */

#define CELL_OSKDIALOG_EVENT_HOOK_TYPE_FUNCTION_KEY   0x01
#define CELL_OSKDIALOG_EVENT_HOOK_TYPE_ASCII_KEY      0x02
#define CELL_OSKDIALOG_EVENT_HOOK_TYPE_ONLY_MODIFIER  0x04

/* ---- scale limits ----------------------------------------------- */

#define CELL_OSKDIALOG_SCALE_MAX  1.05f
#define CELL_OSKDIALOG_SCALE_MIN  0.80f

/* ---- filter callback return values ------------------------------ */

typedef enum {
    CELL_OSKDIALOG_NOT_CHANGE = 0,
    CELL_OSKDIALOG_CHANGE_WORD,
} CellOskDialogFilterCallbackReturnValue;

/* ---- key-event action values ------------------------------------ */

typedef enum {
    CELL_OSKDIALOG_CHANGE_NO_EVENT          = 0,
    CELL_OSKDIALOG_CHANGE_EVENT_CANCEL      = 1,
    CELL_OSKDIALOG_CHANGE_WORDS_INPUT       = 3,
    CELL_OSKDIALOG_CHANGE_WORDS_INSERT      = 4,
    CELL_OSKDIALOG_CHANGE_WORDS_REPLACE_ALL = 6,
} CellOskDialogActionValue;

/* ---- force-finish reason ---------------------------------------- */

typedef enum {
    CELL_OSKDIALOG_CLOSE_CONFIRM = 0,
    CELL_OSKDIALOG_CLOSE_CANCEL,
} CellOskDialogFinishReason;

/* ---- keyboard message struct ------------------------------------ */

typedef struct {
    uint32_t led;
    uint32_t mkey;
    uint16_t keycode;
} CellOskDialogKeyMessage;

/* ---- IME option dictionary info --------------------------------- */

typedef struct {
    int32_t     targetLanguage;
    const char *dictionaryPath ATTRIBUTE_PRXPTR;
} CellOskDialogImeDictionaryInfo;

/* ---- callback typedefs (passed by value — no PRXPTR) ------------ */

typedef int (*cellOskDialogConfirmWordFilterCallback)(
    uint16_t *pConfirmString, int32_t wordLength);

typedef bool (*cellOskDialogHardwareKeyboardEventHookCallback)(
    CellOskDialogKeyMessage *keyMessage, uint32_t *action, void *pActionInfo);

typedef bool (*cellOskDialogForceFinishCallback)(void);

/* ---- API entry points (17) -------------------------------------- */

int cellOskDialogExtRegisterConfirmWordFilterCallback(
    cellOskDialogConfirmWordFilterCallback pCallback);

int cellOskDialogExtRegisterKeyboardEventHookCallback(
    uint16_t hookEventMode,
    cellOskDialogHardwareKeyboardEventHookCallback pCallback);

int cellOskDialogExtRegisterKeyboardEventHookCallbackEx(
    uint16_t hookEventMode,
    cellOskDialogHardwareKeyboardEventHookCallback pCallback);

int cellOskDialogExtSetInitialScale(float initialScale);

int cellOskDialogExtUpdateInputText(void);

int cellOskDialogExtInputDeviceLock(void);

int cellOskDialogExtInputDeviceUnlock(void);

int cellOskDialogExtSendFinishMessage(CellOskDialogFinishReason finishReason);

int cellOskDialogExtRegisterForceFinishCallback(
    cellOskDialogForceFinishCallback pCallback);

int cellOskDialogExtSetBaseColor(float red, float green, float blue, float alpha);

int cellOskDialogExtEnableHalfByteKana(void);

int cellOskDialogExtDisableHalfByteKana(void);

int cellOskDialogExtAddJapaneseOptionDictionary(const char **filePath);

int cellOskDialogExtAddOptionDictionary(
    const CellOskDialogImeDictionaryInfo *dictionaryInfo);

int cellOskDialogExtEnableClipboard(void);

int cellOskDialogExtUpdatePointerDisplayPos(const CellOskDialogPoint pos);

int cellOskDialogExtSetPointerEnable(bool enable);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* __PS3DK_CELL_SYSUTIL_OSKDIALOG_EXT_H__ */
