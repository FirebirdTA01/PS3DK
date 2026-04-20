/*
 * PS3 Custom Toolchain — <cell/pad.h>
 *
 * Cell-SDK-style pad / controller input surface in our SDK's cell/
 * namespace.  At present the runtime lives in the PSL1GHT-derived
 * libio.a (ioPad* symbols from the PS3's sys_io SPRX); this header
 * presents the cell-SDK-spelled API on top.  As the SDK is weaned
 * off PSL1GHT, the backing symbols migrate into our own stubs / PRX
 * modules — but the interface stays stable.
 *
 * Exposed surface:
 *   cellPadInit / cellPadEnd / cellPadClearBuf
 *   cellPadGetInfo / cellPadGetInfo2 / cellPadGetCapabilityInfo
 *   cellPadGetData / cellPadGetDataExtra / cellPadGetRawData
 *   cellPadSetPortSetting / cellPadSetPressMode / cellPadSetSensorMode
 *   cellPadInfoPressMode / cellPadInfoSensorMode / cellPadSetActDirect
 *   cellPadPeriphGetInfo / cellPadPeriphGetData
 *   cellPadLddRegisterController / cellPadLddUnregisterController
 *   cellPadLddDataInsert / cellPadLddGetPortNo
 */

#ifndef PS3TC_CELL_PAD_H
#define PS3TC_CELL_PAD_H

#include <io/pad.h>
#include <ppu-types.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- limits --------------------------------------------------------- */
#define CELL_PAD_MAX_PORT_NUM             MAX_PORT_NUM
#define CELL_PAD_MAX_CODES                MAX_PAD_CODES
#define CELL_PAD_MAX_CAPABILITY_INFO      MAX_PAD_CAPABILITY_INFO
#define CELL_PAD_MAX_ACTUATOR             MAX_PAD_ACTUATOR

/* ---- port status bits (port_status[n]) ----------------------------- */
#define CELL_PAD_STATUS_DISCONNECTED      0
#define CELL_PAD_STATUS_CONNECTED         (1 << 0)
#define CELL_PAD_STATUS_ASSIGN_CHANGES    (1 << 1)

/* ---- press / sensor mode args -------------------------------------- */
#define CELL_PAD_PRESS_MODE_ON            PAD_PRESS_MODE_ON
#define CELL_PAD_PRESS_MODE_OFF           PAD_PRESS_MODE_OFF
#define CELL_PAD_SENSOR_MODE_ON           PAD_SENSOR_MODE_ON
#define CELL_PAD_SENSOR_MODE_OFF          PAD_SENSOR_MODE_OFF

/* Port-setting bitmask written via cellPadSetPortSetting. */
#define CELL_PAD_SETTING_PRESS_ON         PAD_SETTINGS_PRESS_ON
#define CELL_PAD_SETTING_PRESS_OFF        PAD_SETTINGS_PRESS_OFF
#define CELL_PAD_SETTING_SENSOR_ON        PAD_SETTINGS_SENSOR_ON
#define CELL_PAD_SETTING_SENSOR_OFF       PAD_SETTINGS_SENSOR_OFF

/* ---- return / error codes ------------------------------------------ */
#define CELL_PAD_OK                       PAD_OK
#define CELL_PAD_ERROR_FATAL              PAD_ERROR_FATAL
#define CELL_PAD_ERROR_INVALID_PARAMETER  PAD_ERROR_INVALID_PARAMETER
#define CELL_PAD_ERROR_ALREADY_INITIALIZED PAD_ERROR_ALREADY_INITIALIZED
#define CELL_PAD_ERROR_UNINITIALIZED      PAD_ERROR_NOT_INITIALIZED

/* ---- button[] offset indices --------------------------------------- */
#define CELL_PAD_BTN_OFFSET_DIGITAL1       PAD_BUTTON_OFFSET_DIGITAL1
#define CELL_PAD_BTN_OFFSET_DIGITAL2       PAD_BUTTON_OFFSET_DIGITAL2
#define CELL_PAD_BTN_OFFSET_ANALOG_RIGHT_X PAD_BUTTON_OFFSET_ANALOG_RIGHT_X
#define CELL_PAD_BTN_OFFSET_ANALOG_RIGHT_Y PAD_BUTTON_OFFSET_ANALOG_RIGHT_Y
#define CELL_PAD_BTN_OFFSET_ANALOG_LEFT_X  PAD_BUTTON_OFFSET_ANALOG_LEFT_X
#define CELL_PAD_BTN_OFFSET_ANALOG_LEFT_Y  PAD_BUTTON_OFFSET_ANALOG_LEFT_Y
#define CELL_PAD_BTN_OFFSET_PRESS_RIGHT    PAD_BUTTON_OFFSET_PRESS_RIGHT
#define CELL_PAD_BTN_OFFSET_PRESS_LEFT     PAD_BUTTON_OFFSET_PRESS_LEFT
#define CELL_PAD_BTN_OFFSET_PRESS_UP       PAD_BUTTON_OFFSET_PRESS_UP
#define CELL_PAD_BTN_OFFSET_PRESS_DOWN     PAD_BUTTON_OFFSET_PRESS_DOWN
#define CELL_PAD_BTN_OFFSET_PRESS_TRIANGLE PAD_BUTTON_OFFSET_PRESS_TRIANGLE
#define CELL_PAD_BTN_OFFSET_PRESS_CIRCLE   PAD_BUTTON_OFFSET_PRESS_CIRCLE
#define CELL_PAD_BTN_OFFSET_PRESS_CROSS    PAD_BUTTON_OFFSET_PRESS_CROSS
#define CELL_PAD_BTN_OFFSET_PRESS_SQUARE   PAD_BUTTON_OFFSET_PRESS_SQUARE
#define CELL_PAD_BTN_OFFSET_PRESS_L1       PAD_BUTTON_OFFSET_PRESS_L1
#define CELL_PAD_BTN_OFFSET_PRESS_R1       PAD_BUTTON_OFFSET_PRESS_R1
#define CELL_PAD_BTN_OFFSET_PRESS_L2       PAD_BUTTON_OFFSET_PRESS_L2
#define CELL_PAD_BTN_OFFSET_PRESS_R2       PAD_BUTTON_OFFSET_PRESS_R2
#define CELL_PAD_BTN_OFFSET_SENSOR_X       PAD_BUTTON_OFFSET_SENSOR_X
#define CELL_PAD_BTN_OFFSET_SENSOR_Y       PAD_BUTTON_OFFSET_SENSOR_Y
#define CELL_PAD_BTN_OFFSET_SENSOR_Z       PAD_BUTTON_OFFSET_SENSOR_Z
#define CELL_PAD_BTN_OFFSET_SENSOR_G       PAD_BUTTON_OFFSET_SENSOR_G

/* DIGITAL1 bitmask */
#define CELL_PAD_CTRL_LEFT                 PAD_CTRL_LEFT
#define CELL_PAD_CTRL_DOWN                 PAD_CTRL_DOWN
#define CELL_PAD_CTRL_RIGHT                PAD_CTRL_RIGHT
#define CELL_PAD_CTRL_UP                   PAD_CTRL_UP
#define CELL_PAD_CTRL_START                PAD_CTRL_START
#define CELL_PAD_CTRL_R3                   PAD_CTRL_R3
#define CELL_PAD_CTRL_L3                   PAD_CTRL_L3
#define CELL_PAD_CTRL_SELECT               PAD_CTRL_SELECT

/* DIGITAL2 bitmask */
#define CELL_PAD_CTRL_SQUARE               PAD_CTRL_SQUARE
#define CELL_PAD_CTRL_CROSS                PAD_CTRL_CROSS
#define CELL_PAD_CTRL_CIRCLE               PAD_CTRL_CIRCLE
#define CELL_PAD_CTRL_TRIANGLE             PAD_CTRL_TRIANGLE
#define CELL_PAD_CTRL_R1                   PAD_CTRL_R1
#define CELL_PAD_CTRL_L1                   PAD_CTRL_L1
#define CELL_PAD_CTRL_R2                   PAD_CTRL_R2
#define CELL_PAD_CTRL_L2                   PAD_CTRL_L2

/* ---- structs with cell-SDK field names ----------------------------- */
/*
 * Layout matches PSL1GHT's padInfo / padInfo2 / padData — the
 * underlying SPRX ABI is identical, only field spellings differ —
 * so the wrapper functions below cast pointer types between the two
 * structs at zero runtime cost.  The static_asserts lock the layout.
 */

typedef struct _CellPadInfo
{
    u32 max_connect;                             /* PSL1GHT: max      */
    u32 now_connect;                             /* PSL1GHT: connected*/
    u32 system_info;                             /* PSL1GHT: info     */
    u16 vendor_id [CELL_PAD_MAX_PORT_NUM + 120]; /* layout matches MAX_PADS=127 */
    u16 product_id[CELL_PAD_MAX_PORT_NUM + 120];
    u8  status    [CELL_PAD_MAX_PORT_NUM + 120];
} CellPadInfo;

typedef struct _CellPadInfo2
{
    u32 max_connect;                             /* PSL1GHT: max              */
    u32 now_connect;                             /* PSL1GHT: connected        */
    u32 system_info;                             /* PSL1GHT: info             */
    u32 port_status      [CELL_PAD_MAX_PORT_NUM];
    u32 port_setting     [CELL_PAD_MAX_PORT_NUM];
    u32 device_capability[CELL_PAD_MAX_PORT_NUM];
    u32 device_type      [CELL_PAD_MAX_PORT_NUM];
} CellPadInfo2;

/* CellPadData's wire layout is dictated by the SPRX — samples read
 * `len` then index `button[]` by the CELL_PAD_BTN_OFFSET_* constants
 * above.  PSL1GHT's padData declares the same length/array prefix
 * (plus a parallel bit-field view via an anonymous union) so we just
 * typedef through and let the same cast pattern apply at the API
 * surface. */
typedef padData CellPadData;

#if defined(__GNUC__)
_Static_assert(sizeof(CellPadInfo2) == sizeof(padInfo2),
               "CellPadInfo2 must be layout-compatible with padInfo2");
_Static_assert(sizeof(CellPadData)  == sizeof(padData),
               "CellPadData must be layout-compatible with padData");
#endif

/* ---- function wrappers --------------------------------------------- */
/*
 * We use inline thunks rather than `#define`s so argument-type
 * checking still catches mistakes at the cell-SDK-named boundary.  GCC
 * inlines these away at -O2, so the generated code is identical to
 * a direct call.
 */

static inline s32 cellPadInit(u32 max_connect)
{ return ioPadInit(max_connect); }

static inline s32 cellPadEnd(void)
{ return ioPadEnd(); }

static inline s32 cellPadClearBuf(u32 port)
{ return ioPadClearBuf(port); }

static inline s32 cellPadGetInfo(CellPadInfo *info)
{ return ioPadGetInfo((padInfo *)info); }

static inline s32 cellPadGetInfo2(CellPadInfo2 *info)
{ return ioPadGetInfo2((padInfo2 *)info); }

/* padCapabilityInfo (PSL1GHT) holds the capability bitmask + reserved
 * words; its layout matches what cell-SDK callers expect through
 * CellPadCapabilityInfo, so we expose the same type under both names. */
typedef padCapabilityInfo  CellPadCapabilityInfo;
typedef padActParam        CellPadActParam;

static inline s32 cellPadGetCapabilityInfo(u32 port, CellPadCapabilityInfo *caps)
{ return ioPadGetCapabilityInfo(port, caps); }

static inline s32 cellPadGetData(u32 port, CellPadData *data)
{ return ioPadGetData(port, (padData *)data); }

static inline s32 cellPadGetDataExtra(u32 port, u32 *device_type, CellPadData *data)
{ return ioPadGetDataExtra(port, device_type, (padData *)data); }

static inline s32 cellPadSetPortSetting(u32 port, u32 setting)
{ return ioPadSetPortSetting(port, setting); }

static inline s32 cellPadSetPressMode(u32 port, u32 mode)
{ return ioPadSetPressMode(port, mode); }

static inline s32 cellPadSetSensorMode(u32 port, u32 mode)
{ return ioPadSetSensorMode(port, mode); }

static inline s32 cellPadInfoPressMode(u32 port)
{ return ioPadInfoPressMode(port); }

static inline s32 cellPadInfoSensorMode(u32 port)
{ return ioPadInfoSensorMode(port); }

static inline s32 cellPadSetActDirect(u32 port, CellPadActParam *param)
{ return ioPadSetActDirect(port, param); }

#ifdef __cplusplus
}
#endif

#endif  /* PS3TC_CELL_PAD_H */
