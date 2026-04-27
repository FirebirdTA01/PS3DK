/*! \file cell/sysutil_oskdialog.h
 \brief On-Screen Keyboard dialog API (cellOskDialog*) — late-SDK
        layout, separate-window, key-layout, device-mask extensions.

  These entry points resolve to libsysutil_stub.a (cellSysutil PRX);
  RPCS3 HLEs the panel.
*/

#ifndef PS3TC_CELL_SYSUTIL_OSKDIALOG_H
#define PS3TC_CELL_SYSUTIL_OSKDIALOG_H

#include <stdint.h>
#include <sys/memory.h>

#ifdef __cplusplus
extern "C" {
#endif

/* `sys_memory_container_t` is the reference-SDK name; PSL1GHT spells
 * it `sys_mem_container_t` in <sys/memory.h>.  Local typedef so this
 * header compiles against either name. */
#ifndef PS3TC_SYS_MEMORY_CONTAINER_T_DEFINED
#define PS3TC_SYS_MEMORY_CONTAINER_T_DEFINED
typedef sys_mem_container_t sys_memory_container_t;
#endif

/* ---- callback / event status codes -------------------------------- */
#define CELL_SYSUTIL_OSKDIALOG_LOADED                0x0502
#define CELL_SYSUTIL_OSKDIALOG_FINISHED              0x0503
#define CELL_SYSUTIL_OSKDIALOG_UNLOADED              0x0504
#define CELL_SYSUTIL_OSKDIALOG_INPUT_ENTERED         0x0505
#define CELL_SYSUTIL_OSKDIALOG_INPUT_CANCELED        0x0506
#define CELL_SYSUTIL_OSKDIALOG_INPUT_DEVICE_CHANGED  0x0507
#define CELL_SYSUTIL_OSKDIALOG_DISPLAY_CHANGED       0x0508

/* ---- error codes -------------------------------------------------- */
#define CELL_OSKDIALOG_ERROR_IME_ALREADY_IN_USE  0x8002b501
#define CELL_OSKDIALOG_ERROR_GET_SIZE_ERROR      0x8002b502
#define CELL_OSKDIALOG_ERROR_UNKNOWN             0x8002b503
#define CELL_OSKDIALOG_ERROR_PARAM               0x8002b504

/* ---- panel mode flags --------------------------------------------- */
#define CELL_OSKDIALOG_PANELMODE_DEFAULT              0x00000000
#define CELL_OSKDIALOG_PANELMODE_DEFAULT_NO_JAPANESE  0x00000200
#define CELL_OSKDIALOG_PANELMODE_GERMAN               0x00000001
#define CELL_OSKDIALOG_PANELMODE_ENGLISH              0x00000002
#define CELL_OSKDIALOG_PANELMODE_SPANISH              0x00000004
#define CELL_OSKDIALOG_PANELMODE_FRENCH               0x00000008
#define CELL_OSKDIALOG_PANELMODE_ITALIAN              0x00000010
#define CELL_OSKDIALOG_PANELMODE_DUTCH                0x00000020
#define CELL_OSKDIALOG_PANELMODE_PORTUGUESE           0x00000040
#define CELL_OSKDIALOG_PANELMODE_RUSSIAN              0x00000080
#define CELL_OSKDIALOG_PANELMODE_JAPANESE             0x00000100
#define CELL_OSKDIALOG_PANELMODE_KOREAN               0x00001000
#define CELL_OSKDIALOG_PANELMODE_DANISH               0x00020000
#define CELL_OSKDIALOG_PANELMODE_SWEDISH              0x00040000
#define CELL_OSKDIALOG_PANELMODE_NORWEGIAN            0x00080000
#define CELL_OSKDIALOG_PANELMODE_FINNISH              0x00100000
#define CELL_OSKDIALOG_PANELMODE_POLISH               0x00000400
#define CELL_OSKDIALOG_PANELMODE_PORTUGUESE_BRAZIL    0x00010000
#define CELL_OSKDIALOG_PANELMODE_TURKEY               0x00002000
#define CELL_OSKDIALOG_PANELMODE_CHINA_TRADITIONAL    0x00004000
#define CELL_OSKDIALOG_PANELMODE_TRADITIONAL_CHINESE  0x00004000
#define CELL_OSKDIALOG_PANELMODE_SIMPLIFIED_CHINESE   0x00008000
#define CELL_OSKDIALOG_PANELMODE_JAPANESE_HIRAGANA    0x00200000
#define CELL_OSKDIALOG_PANELMODE_JAPANESE_KATAKANA    0x00400000
#define CELL_OSKDIALOG_PANELMODE_ALPHABET_FULL_WIDTH  0x00800000
#define CELL_OSKDIALOG_PANELMODE_ALPHABET             0x01000000
#define CELL_OSKDIALOG_PANELMODE_LATIN                0x02000000
#define CELL_OSKDIALOG_PANELMODE_NUMERAL_FULL_WIDTH   0x04000000
#define CELL_OSKDIALOG_PANELMODE_NUMERAL              0x08000000
#define CELL_OSKDIALOG_PANELMODE_URL                  0x10000000
#define CELL_OSKDIALOG_PANELMODE_PASSWORD             0x20000000

/* ---- layout-mode (alignment) flags -------------------------------- */
#define CELL_OSKDIALOG_LAYOUTMODE_X_ALIGN_LEFT    0x00000200
#define CELL_OSKDIALOG_LAYOUTMODE_X_ALIGN_CENTER  0x00000400
#define CELL_OSKDIALOG_LAYOUTMODE_X_ALIGN_RIGHT   0x00000800
#define CELL_OSKDIALOG_LAYOUTMODE_Y_ALIGN_TOP     0x00001000
#define CELL_OSKDIALOG_LAYOUTMODE_Y_ALIGN_CENTER  0x00002000
#define CELL_OSKDIALOG_LAYOUTMODE_Y_ALIGN_BOTTOM  0x00004000

/* ---- prohibit / panel-enable / device flags ----------------------- */
#define CELL_OSKDIALOG_NO_SPACE              0x00000001
#define CELL_OSKDIALOG_NO_RETURN             0x00000002
#define CELL_OSKDIALOG_NO_INPUT_ANALOG       0x00000008
#define CELL_OSKDIALOG_NO_STARTUP_EFFECT     0x00001000
#define CELL_OSKDIALOG_10KEY_PANEL           0x00000001
#define CELL_OSKDIALOG_FULLKEY_PANEL         0x00000002
#define CELL_OSKDIALOG_STRING_SIZE           512
#define CELL_OSKDIALOG_DEVICE_MASK_PAD       0x000000ff

/* ---- enums -------------------------------------------------------- */
typedef enum {
    CELL_OSKDIALOG_LOCAL_STATUS_NONE                       = 0,
    CELL_OSKDIALOG_LOCAL_STATUS_INITIALIZING               = 1,
    CELL_OSKDIALOG_LOCAL_STATUS_INITIALIZED                = 2,
    CELL_OSKDIALOG_LOCAL_STATUS_OPENED                     = 3,
    CELL_OSKDIALOG_LOCAL_STATUS_CLOSING                    = 4,
    CELL_OSKDIALOG_LOCAL_STATUS_CLOSED                     = 5,
    CELL_OSKDIALOG_LOCAL_STATUS_CONTINUE                   = 6,
    CELL_OSKDIALOG_LOCAL_STATUS_CHANGED_TO_KEYBOARD_INPUT  = 7,
    CELL_OSKDIALOG_LOCAL_STATUS_CHANGED_TO_PAD_INPUT       = 8,
    CELL_OSKDIALOG_LOCAL_STATUS_HIDE_OSK                   = 9,
    CELL_OSKDIALOG_LOCAL_STATUS_SHOW_OSK                   = 10
} CellOskDialogLocalStatus;

typedef enum {
    CELL_OSKDIALOG_INITIAL_PANEL_LAYOUT_SYSTEM   = 0,
    CELL_OSKDIALOG_INITIAL_PANEL_LAYOUT_10KEY    = 1,
    CELL_OSKDIALOG_INITIAL_PANEL_LAYOUT_FULLKEY  = 2
} CellOskDialogInitialKeyLayout;

typedef enum {
    CELL_OSKDIALOG_INPUT_FIELD_RESULT_OK             = 0,
    CELL_OSKDIALOG_INPUT_FIELD_RESULT_CANCELED       = 1,
    CELL_OSKDIALOG_INPUT_FIELD_RESULT_ABORT          = 2,
    CELL_OSKDIALOG_INPUT_FIELD_RESULT_NO_INPUT_TEXT  = 3
} CellOskDialogInputFieldResult;

typedef enum {
    CELL_OSKDIALOG_INPUT_DEVICE_PAD       = 0,
    CELL_OSKDIALOG_INPUT_DEVICE_KEYBOARD  = 1
} CellOskDialogInputDevice;

typedef enum {
    CELL_OSKDIALOG_CONTINUOUS_MODE_NONE         = 0,
    CELL_OSKDIALOG_CONTINUOUS_MODE_REMAIN_OPEN  = 1,
    CELL_OSKDIALOG_CONTINUOUS_MODE_HIDE         = 2,
    CELL_OSKDIALOG_CONTINUOUS_MODE_SHOW         = 3
} CellOskDialogContinuousMode;

typedef enum {
    CELL_OSKDIALOG_DISPLAY_STATUS_HIDE  = 0,
    CELL_OSKDIALOG_DISPLAY_STATUS_SHOW  = 1
} CellOskDialogDisplayStatus;

typedef enum {
    CELL_OSKDIALOG_TYPE_SINGLELINE_OSK = 0,
    CELL_OSKDIALOG_TYPE_MULTILINE_OSK  = 1,
    CELL_OSKDIALOG_TYPE_FULL_KEYBOARD_SINGLELINE_OSK,
    CELL_OSKDIALOG_TYPE_FULL_KEYBOARD_MULTILINE_OSK,
    CELL_OSKDIALOG_TYPE_SEPARATE_SINGLELINE_FIELD_WINDOW,
    CELL_OSKDIALOG_TYPE_SEPARATE_MULTILINE_FIELD_WINDOW,
    CELL_OSKDIALOG_TYPE_SEPARATE_INPUT_PANEL_WINDOW,
    CELL_OSKDIALOG_TYPE_SEPARATE_FULL_KEYBOARD_INPUT_PANEL_WINDOW,
    CELL_OSKDIALOG_TYPE_SEPARATE_CANDIDATE_WINDOW
} CellOskDialogType;

/* ---- structs ------------------------------------------------------ */
typedef struct {
    uint16_t *message;
    uint16_t *init_text;
    int       limit_length;
} CellOskDialogInputFieldInfo;

typedef struct {
    CellOskDialogInputFieldResult  result;
    int                            numCharsResultString;
    uint16_t                      *pResultString;
} CellOskDialogCallbackReturnParam;

typedef struct {
    float x;
    float y;
} CellOskDialogPoint;

typedef struct {
    int                 layoutMode;
    CellOskDialogPoint  position;
} CellOskDialogLayoutInfo;

typedef struct {
    unsigned int        allowOskPanelFlg;
    unsigned int        firstViewPanel;
    CellOskDialogPoint  controlPoint;
    int                 prohibitFlgs;
} CellOskDialogParam;

typedef struct {
    CellOskDialogContinuousMode  continuousMode;
    int                          deviceMask;
    int                          inputFieldWindowWidth;
    float                        inputFieldBackgroundTrans;
    CellOskDialogLayoutInfo     *inputFieldLayoutInfo;
    CellOskDialogLayoutInfo     *inputPanelLayoutInfo;
    void                        *reserved;
} CellOskDialogSeparateWindowOption;

/* ---- entry points ------------------------------------------------- */
extern int cellOskDialogLoadAsync(sys_memory_container_t mem_container,
                                  const CellOskDialogParam *dialogParam,
                                  const CellOskDialogInputFieldInfo *inputFieldInfo);
extern int cellOskDialogUnloadAsync(CellOskDialogCallbackReturnParam *outputInfo);
extern int cellOskDialogAbort(void);
extern int cellOskDialogGetSize(uint16_t *width, uint16_t *height,
                                CellOskDialogType dialogType);
extern int cellOskDialogSetLayoutMode(int32_t layoutMode);
extern int cellOskDialogAddSupportLanguage(uint32_t supportLanguage);
extern int cellOskDialogSetKeyLayoutOption(uint32_t option);
extern int cellOskDialogDisableDimmer(void);
extern int cellOskDialogSetInitialKeyLayout(CellOskDialogInitialKeyLayout initialKeyLayout);
extern int cellOskDialogSetInitialInputDevice(CellOskDialogInputDevice inputDevice);
extern int cellOskDialogSetSeparateWindowOption(CellOskDialogSeparateWindowOption *windowOption);
extern int cellOskDialogSetDeviceMask(uint32_t deviceMask);
extern int cellOskDialogGetInputText(CellOskDialogCallbackReturnParam *outputInfo);

#ifdef __cplusplus
}
#endif

#endif  /* PS3TC_CELL_SYSUTIL_OSKDIALOG_H */
