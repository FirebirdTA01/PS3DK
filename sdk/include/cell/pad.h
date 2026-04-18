/*
 * PS3 Custom Toolchain — <cell/pad.h>
 *
 * Part of our SDK's cell/ namespace.  Provides the Sony-style pad /
 * controller input surface.  At present the runtime implementation
 * lives in the PSL1GHT-derived libio.a (ioPad* symbols from the PS3's
 * sys_io SPRX); this header presents the Sony-spelled API on top so
 * user code written against Sony samples compiles unchanged.  As the
 * SDK is weaned off PSL1GHT, the backing symbols move into our own
 * stubs / PRX modules — but the header stays stable.
 *
 * Covered:
 *   cellPadInit / cellPadEnd
 *   cellPadGetData / cellPadGetDataExtra / cellPadGetRawData
 *   cellPadGetInfo / cellPadGetInfo2 / cellPadGetCapabilityInfo
 *   cellPadClearBuf
 *   cellPadSetPortSetting / cellPadSetPressMode / cellPadSetSensorMode
 *   cellPadInfoPressMode / cellPadInfoSensorMode
 *   cellPadSetActDirect
 *   cellPadPeriphGetInfo / cellPadPeriphGetData
 *   cellPadLddRegisterController / cellPadLddUnregisterController
 *   cellPadLddDataInsert / cellPadLddGetPortNo
 *
 * Constants (CELL_PAD_*, max counts, error codes) alias onto the
 * long-standing PAD_* spellings the io/pad.h declarations already
 * ship; semantics and bit layouts are identical.
 */

#ifndef PS3TC_CELL_PAD_H
#define PS3TC_CELL_PAD_H

#include <io/pad.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- struct aliases ------------------------------------------------- */
typedef padInfo   CellPadInfo;
typedef padInfo2  CellPadInfo2;
typedef padData   CellPadData;

/* ---- limit / mode aliases ------------------------------------------ */
#define CELL_PAD_MAX_PORT_NUM             MAX_PORT_NUM
#define CELL_PAD_MAX_CODES                MAX_PAD_CODES
#define CELL_PAD_MAX_CAPABILITY_INFO      MAX_PAD_CAPABILITY_INFO
#define CELL_PAD_MAX_ACTUATOR             MAX_PAD_ACTUATOR

#define CELL_PAD_PRESS_MODE_ON            PAD_PRESS_MODE_ON
#define CELL_PAD_PRESS_MODE_OFF           PAD_PRESS_MODE_OFF
#define CELL_PAD_SENSOR_MODE_ON           PAD_SENSOR_MODE_ON
#define CELL_PAD_SENSOR_MODE_OFF          PAD_SENSOR_MODE_OFF

/* Pressure / sensor bitmasks written via cellPadSetPortSetting. */
#define CELL_PAD_SETTING_PRESS_ON         PAD_SETTINGS_PRESS_ON
#define CELL_PAD_SETTING_PRESS_OFF        PAD_SETTINGS_PRESS_OFF
#define CELL_PAD_SETTING_SENSOR_ON        PAD_SETTINGS_SENSOR_ON
#define CELL_PAD_SETTING_SENSOR_OFF       PAD_SETTINGS_SENSOR_OFF

/* Error / status codes — long-standing Sony values, identical in libio. */
#define CELL_PAD_OK                       PAD_OK
#define CELL_PAD_ERROR_FATAL              PAD_ERROR_FATAL
#define CELL_PAD_ERROR_INVALID_PARAMETER  PAD_ERROR_INVALID_PARAMETER
#define CELL_PAD_ERROR_ALREADY_INITIALIZED PAD_ERROR_ALREADY_INITIALIZED
#define CELL_PAD_ERROR_UNINITIALIZED      PAD_ERROR_NOT_INITIALIZED

/* ---- button offset + bit aliases ----------------------------------- */
/* Indices into padData.button[].  Sony spells them CELL_PAD_BTN_*; the
 * PSL1GHT-named PAD_BUTTON_* constants resolve to the same slots. */
#define CELL_PAD_BTN_OFFSET_DIGITAL1      PAD_BUTTON_OFFSET_DIGITAL1
#define CELL_PAD_BTN_OFFSET_DIGITAL2      PAD_BUTTON_OFFSET_DIGITAL2
#define CELL_PAD_BTN_OFFSET_ANALOG_RIGHT_X PAD_BUTTON_OFFSET_ANALOG_RIGHT_X
#define CELL_PAD_BTN_OFFSET_ANALOG_RIGHT_Y PAD_BUTTON_OFFSET_ANALOG_RIGHT_Y
#define CELL_PAD_BTN_OFFSET_ANALOG_LEFT_X  PAD_BUTTON_OFFSET_ANALOG_LEFT_X
#define CELL_PAD_BTN_OFFSET_ANALOG_LEFT_Y  PAD_BUTTON_OFFSET_ANALOG_LEFT_Y
#define CELL_PAD_BTN_OFFSET_PRESS_RIGHT   PAD_BUTTON_OFFSET_PRESS_RIGHT
#define CELL_PAD_BTN_OFFSET_PRESS_LEFT    PAD_BUTTON_OFFSET_PRESS_LEFT
#define CELL_PAD_BTN_OFFSET_PRESS_UP      PAD_BUTTON_OFFSET_PRESS_UP
#define CELL_PAD_BTN_OFFSET_PRESS_DOWN    PAD_BUTTON_OFFSET_PRESS_DOWN
#define CELL_PAD_BTN_OFFSET_PRESS_TRIANGLE PAD_BUTTON_OFFSET_PRESS_TRIANGLE
#define CELL_PAD_BTN_OFFSET_PRESS_CIRCLE  PAD_BUTTON_OFFSET_PRESS_CIRCLE
#define CELL_PAD_BTN_OFFSET_PRESS_CROSS   PAD_BUTTON_OFFSET_PRESS_CROSS
#define CELL_PAD_BTN_OFFSET_PRESS_SQUARE  PAD_BUTTON_OFFSET_PRESS_SQUARE
#define CELL_PAD_BTN_OFFSET_PRESS_L1      PAD_BUTTON_OFFSET_PRESS_L1
#define CELL_PAD_BTN_OFFSET_PRESS_R1      PAD_BUTTON_OFFSET_PRESS_R1
#define CELL_PAD_BTN_OFFSET_PRESS_L2      PAD_BUTTON_OFFSET_PRESS_L2
#define CELL_PAD_BTN_OFFSET_PRESS_R2      PAD_BUTTON_OFFSET_PRESS_R2
#define CELL_PAD_BTN_OFFSET_SENSOR_X      PAD_BUTTON_OFFSET_SENSOR_X
#define CELL_PAD_BTN_OFFSET_SENSOR_Y      PAD_BUTTON_OFFSET_SENSOR_Y
#define CELL_PAD_BTN_OFFSET_SENSOR_Z      PAD_BUTTON_OFFSET_SENSOR_Z
#define CELL_PAD_BTN_OFFSET_SENSOR_G      PAD_BUTTON_OFFSET_SENSOR_G

/* DIGITAL1 button bits */
#define CELL_PAD_CTRL_LEFT                PAD_CTRL_LEFT
#define CELL_PAD_CTRL_DOWN                PAD_CTRL_DOWN
#define CELL_PAD_CTRL_RIGHT               PAD_CTRL_RIGHT
#define CELL_PAD_CTRL_UP                  PAD_CTRL_UP
#define CELL_PAD_CTRL_START               PAD_CTRL_START
#define CELL_PAD_CTRL_R3                  PAD_CTRL_R3
#define CELL_PAD_CTRL_L3                  PAD_CTRL_L3
#define CELL_PAD_CTRL_SELECT              PAD_CTRL_SELECT

/* DIGITAL2 button bits */
#define CELL_PAD_CTRL_SQUARE              PAD_CTRL_SQUARE
#define CELL_PAD_CTRL_CROSS               PAD_CTRL_CROSS
#define CELL_PAD_CTRL_CIRCLE              PAD_CTRL_CIRCLE
#define CELL_PAD_CTRL_TRIANGLE            PAD_CTRL_TRIANGLE
#define CELL_PAD_CTRL_R1                  PAD_CTRL_R1
#define CELL_PAD_CTRL_L1                  PAD_CTRL_L1
#define CELL_PAD_CTRL_R2                  PAD_CTRL_R2
#define CELL_PAD_CTRL_L2                  PAD_CTRL_L2

/* ---- function aliases ---------------------------------------------- */
/*
 * We redirect at the preprocessor level so user source written against
 * `cellPad*` symbols resolves to the same underlying stub the PSL1GHT
 * libio exports.  Both names refer to the same SPRX entry at runtime;
 * the #define just saves us from shipping a parallel libpad_stub.a
 * that duplicates the descriptors.
 *
 * When we cut PSL1GHT's libio loose, this block flips to real
 * declarations of cellPadX functions that our own libpad_stub.a
 * exports — caller code stays the same.
 */
#define cellPadInit                    ioPadInit
#define cellPadEnd                     ioPadEnd
#define cellPadClearBuf                ioPadClearBuf
#define cellPadGetData                 ioPadGetData
#define cellPadGetDataExtra            ioPadGetDataExtra
#define cellPadGetRawData              ioPadGetRawData
#define cellPadGetInfo                 ioPadGetInfo
#define cellPadGetInfo2                ioPadGetInfo2
#define cellPadGetCapabilityInfo       ioPadGetCapabilityInfo
#define cellPadSetPortSetting          ioPadSetPortSetting
#define cellPadSetPressMode            ioPadSetPressMode
#define cellPadSetSensorMode           ioPadSetSensorMode
#define cellPadInfoPressMode           ioPadInfoPressMode
#define cellPadInfoSensorMode          ioPadInfoSensorMode
#define cellPadSetActDirect            ioPadSetActDirect
#define cellPadPeriphGetData           ioPadPeriphGetData
#define cellPadPeriphGetInfo           ioPadPeriphGetInfo
#define cellPadLddRegisterController   ioPadLddRegisterController
#define cellPadLddUnregisterController ioPadLddUnregisterController
#define cellPadLddDataInsert           ioPadLddDataInsert
#define cellPadLddGetPortNo            ioPadLddGetPortNo

#ifdef __cplusplus
}
#endif

#endif  /* PS3TC_CELL_PAD_H */
