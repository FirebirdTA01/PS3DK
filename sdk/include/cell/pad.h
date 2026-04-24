/*! \file cell/pad.h
 \brief cellPad API - PS3 controller input surface, 21 exports.

  Includes the full 475.001 reference set (14 exports) plus 7 legacy
  pre-475 exports the sys_io SPRX still supports — kept so PSL1GHT
  homebrew that calls the older names keeps working.

  Declarations are linked against libio.a (built from
  tools/nidgen/nids/extracted/libio_stub.yaml by
  scripts/build-cell-stub-archives.sh; installed to $PS3DK/ppu/lib/
  where it shadows PSL1GHT's libio.a at link time).  Each cellPad*
  FNID lands in .rodata.sceFNID; the module name "sys_io" appears in
  .rodata.sceResident so the PRX loader binds the sys_io SPRX at
  runtime.

  Workflow:
    cellPadInit(max_connect)              // once at startup
    cellPadGetInfo2(&info)                // discover connected ports
    // per-frame: cellPadGetData(port, &data) then read data.button[]
    cellPadSetActDirect(port, &actParam)  // rumble
    cellPadEnd()                          // at shutdown

  Button offsets: index data.button[] with CELL_PAD_BTN_OFFSET_*.
  DIGITAL1/DIGITAL2 are bitmasks (CELL_PAD_CTRL_* values).  Analog
  sticks read 0x00..0xFF; sensor axes read 0x000..0x3FF.
*/

#ifndef __PS3DK_CELL_PAD_H__
#define __PS3DK_CELL_PAD_H__

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ *
 * Limits
 * ------------------------------------------------------------------ */

#define CELL_MAX_PADS                   127
#define CELL_PAD_MAX_CODES              64
#define CELL_PAD_MAX_PORT_NUM           7
#define CELL_PAD_MAX_CAPABILITY_INFO    32
#define CELL_PAD_ACTUATOR_MAX           2

/* ------------------------------------------------------------------ *
 * Port status / info / capability / device-type bits
 * ------------------------------------------------------------------ */

#define CELL_PAD_INFO_INTERCEPTED       (1 << 0)

#define CELL_PAD_STATUS_DISCONNECTED    0
#define CELL_PAD_STATUS_CONNECTED       (1 << 0)
#define CELL_PAD_STATUS_ASSIGN_CHANGES  (1 << 1)

#define CELL_PAD_SETTING_PRESS_ON       (1 << 1)
#define CELL_PAD_SETTING_SENSOR_ON      (1 << 2)
#define CELL_PAD_SETTING_PRESS_OFF      0
#define CELL_PAD_SETTING_SENSOR_OFF     0

#define CELL_PAD_CAPABILITY_PS3_CONFORMITY   (1 << 0)
#define CELL_PAD_CAPABILITY_PRESS_MODE       (1 << 1)
#define CELL_PAD_CAPABILITY_SENSOR_MODE      (1 << 2)
#define CELL_PAD_CAPABILITY_HP_ANALOG_STICK  (1 << 3)
#define CELL_PAD_CAPABILITY_ACTUATOR         (1 << 4)

#define CELL_PAD_DEV_TYPE_STANDARD    0
#define CELL_PAD_DEV_TYPE_BD_REMOCON  4
#define CELL_PAD_DEV_TYPE_LDD         5

/* Press / sensor mode legacy args (cellPadSetPressMode / SetSensorMode). */
#define CELL_PAD_PRESS_MODE_ON   1
#define CELL_PAD_PRESS_MODE_OFF  0
#define CELL_PAD_SENSOR_MODE_ON  1
#define CELL_PAD_SENSOR_MODE_OFF 0

/* ------------------------------------------------------------------ *
 * Button data offsets (index into CellPadData.button[])
 * ------------------------------------------------------------------ */

#define CELL_PAD_BTN_OFFSET_DIGITAL1         2
#define CELL_PAD_BTN_OFFSET_DIGITAL2         3
#define CELL_PAD_BTN_OFFSET_ANALOG_RIGHT_X   4
#define CELL_PAD_BTN_OFFSET_ANALOG_RIGHT_Y   5
#define CELL_PAD_BTN_OFFSET_ANALOG_LEFT_X    6
#define CELL_PAD_BTN_OFFSET_ANALOG_LEFT_Y    7
#define CELL_PAD_BTN_OFFSET_PRESS_RIGHT      8
#define CELL_PAD_BTN_OFFSET_PRESS_LEFT       9
#define CELL_PAD_BTN_OFFSET_PRESS_UP        10
#define CELL_PAD_BTN_OFFSET_PRESS_DOWN      11
#define CELL_PAD_BTN_OFFSET_PRESS_TRIANGLE  12
#define CELL_PAD_BTN_OFFSET_PRESS_CIRCLE    13
#define CELL_PAD_BTN_OFFSET_PRESS_CROSS     14
#define CELL_PAD_BTN_OFFSET_PRESS_SQUARE    15
#define CELL_PAD_BTN_OFFSET_PRESS_L1        16
#define CELL_PAD_BTN_OFFSET_PRESS_R1        17
#define CELL_PAD_BTN_OFFSET_PRESS_L2        18
#define CELL_PAD_BTN_OFFSET_PRESS_R2        19
#define CELL_PAD_BTN_OFFSET_SENSOR_X        20
#define CELL_PAD_BTN_OFFSET_SENSOR_Y        21
#define CELL_PAD_BTN_OFFSET_SENSOR_Z        22
#define CELL_PAD_BTN_OFFSET_SENSOR_G        23

/* DIGITAL1 bitmask */
#define CELL_PAD_CTRL_LEFT      (1 << 7)
#define CELL_PAD_CTRL_DOWN      (1 << 6)
#define CELL_PAD_CTRL_RIGHT     (1 << 5)
#define CELL_PAD_CTRL_UP        (1 << 4)
#define CELL_PAD_CTRL_START     (1 << 3)
#define CELL_PAD_CTRL_R3        (1 << 2)
#define CELL_PAD_CTRL_L3        (1 << 1)
#define CELL_PAD_CTRL_SELECT    (1 << 0)

/* DIGITAL2 bitmask */
#define CELL_PAD_CTRL_SQUARE    (1 << 7)
#define CELL_PAD_CTRL_CROSS     (1 << 6)
#define CELL_PAD_CTRL_CIRCLE    (1 << 5)
#define CELL_PAD_CTRL_TRIANGLE  (1 << 4)
#define CELL_PAD_CTRL_R1        (1 << 3)
#define CELL_PAD_CTRL_L1        (1 << 2)
#define CELL_PAD_CTRL_R2        (1 << 1)
#define CELL_PAD_CTRL_L2        (1 << 0)

#define CELL_PAD_CTRL_LDD_PS    (1 << 0)

/* ------------------------------------------------------------------ *
 * Error codes
 * ------------------------------------------------------------------ */

#define CELL_PAD_OK                             0
#define CELL_PAD_ERROR_FATAL                    0x80121101
#define CELL_PAD_ERROR_INVALID_PARAMETER        0x80121102
#define CELL_PAD_ERROR_ALREADY_INITIALIZED      0x80121103
#define CELL_PAD_ERROR_UNINITIALIZED            0x80121104
#define CELL_PAD_ERROR_RESOURCE_ALLOCATION_FAILED 0x80121105
#define CELL_PAD_ERROR_DATA_READ_FAILED         0x80121106
#define CELL_PAD_ERROR_NO_DEVICE                0x80121107
#define CELL_PAD_ERROR_UNSUPPORTED_GAMEPAD      0x80121108
#define CELL_PAD_ERROR_TOO_MANY_DEVICES         0x80121109
#define CELL_PAD_ERROR_EBUSY                    0x8012110a

/* ------------------------------------------------------------------ *
 * Structs
 * ------------------------------------------------------------------ */

/* Legacy flat-vector info block (cellPadGetInfo).  Superseded by
 * CellPadInfo2 in 475; kept because sys_io SPRX still exports the NID
 * and PSL1GHT homebrew uses it. */
typedef struct CellPadInfo {
    uint32_t max_connect;
    uint32_t now_connect;
    uint32_t system_info;
    uint16_t vendor_id [CELL_MAX_PADS];
    uint16_t product_id[CELL_MAX_PADS];
    uint8_t  status    [CELL_MAX_PADS];
} CellPadInfo;

typedef struct CellPadInfo2 {
    uint32_t max_connect;
    uint32_t now_connect;
    uint32_t system_info;
    uint32_t port_status      [CELL_PAD_MAX_PORT_NUM];
    uint32_t port_setting     [CELL_PAD_MAX_PORT_NUM];
    uint32_t device_capability[CELL_PAD_MAX_PORT_NUM];
    uint32_t device_type      [CELL_PAD_MAX_PORT_NUM];
} CellPadInfo2;

typedef struct CellPadCapabilityInfo {
    uint32_t info[CELL_PAD_MAX_CAPABILITY_INFO];
} CellPadCapabilityInfo;

typedef struct CellPadData {
    int32_t  len;
    uint16_t button[CELL_PAD_MAX_CODES];
} CellPadData;

typedef struct CellPadActParam {
    uint8_t motor[CELL_PAD_ACTUATOR_MAX];
    uint8_t reserved[6];
} CellPadActParam;

typedef struct CellPadPeriphInfo {
    uint32_t max_connect;
    uint32_t now_connect;
    uint32_t system_info;
    uint32_t port_status      [CELL_PAD_MAX_PORT_NUM];
    uint32_t port_setting     [CELL_PAD_MAX_PORT_NUM];
    uint32_t device_capability[CELL_PAD_MAX_PORT_NUM];
    uint32_t device_type      [CELL_PAD_MAX_PORT_NUM];
    uint32_t pclass_type      [CELL_PAD_MAX_PORT_NUM];
    uint32_t pclass_profile   [CELL_PAD_MAX_PORT_NUM];
} CellPadPeriphInfo;

typedef struct CellPadPeriphData {
    uint32_t pclass_type;
    uint32_t pclass_profile;
    int32_t  len;
    uint16_t button[CELL_PAD_MAX_CODES];
} CellPadPeriphData;

/* ------------------------------------------------------------------ *
 * Library lifetime
 * ------------------------------------------------------------------ */

int32_t cellPadInit(uint32_t max_connect);
int32_t cellPadEnd(void);

/* ------------------------------------------------------------------ *
 * Per-port state and data
 * ------------------------------------------------------------------ */

int32_t cellPadClearBuf(uint32_t port_no);
int32_t cellPadGetData(uint32_t port_no, CellPadData *data);
int32_t cellPadGetDataExtra(uint32_t port_no, uint32_t *device_type, CellPadData *data);
int32_t cellPadGetInfo2(CellPadInfo2 *info);
int32_t cellPadSetPortSetting(uint32_t port_no, uint32_t port_setting);
int32_t cellPadSetActDirect(uint32_t port_no, CellPadActParam *param);

/* Pre-475 legacy entry points (sys_io SPRX still exports them). */
int32_t cellPadGetInfo(CellPadInfo *info);
int32_t cellPadGetCapabilityInfo(uint32_t port_no, CellPadCapabilityInfo *info);
int32_t cellPadGetRawData(uint32_t port_no, CellPadData *data);
int32_t cellPadSetPressMode(uint32_t port_no, uint32_t mode);
int32_t cellPadInfoPressMode(uint32_t port_no);
int32_t cellPadSetSensorMode(uint32_t port_no, uint32_t mode);
int32_t cellPadInfoSensorMode(uint32_t port_no);

/* ------------------------------------------------------------------ *
 * LDD (custom / virtual) controller API
 * ------------------------------------------------------------------ */

int32_t cellPadLddRegisterController(void);
int32_t cellPadLddUnregisterController(int32_t handle);
int32_t cellPadLddDataInsert(int32_t handle, CellPadData *data);
int32_t cellPadLddGetPortNo(int32_t handle);

/* ------------------------------------------------------------------ *
 * Peripheral-class device API (guitar, drum, DJ, dancemat, navigation)
 * ------------------------------------------------------------------ */

int32_t cellPadPeriphGetInfo(CellPadPeriphInfo *info);
int32_t cellPadPeriphGetData(uint32_t port_no, CellPadPeriphData *data);

#ifdef __cplusplus
}
#endif

#endif /* __PS3DK_CELL_PAD_H__ */
