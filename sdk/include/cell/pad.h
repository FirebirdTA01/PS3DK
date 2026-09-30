/*! \file cell/pad.h
 \brief cellPad API - PS3 controller input surface, 21 exports.

  Includes the full reference set (14 exports) plus 7 legacy
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
#include <cell/error.h>     /* CELL_OK + CELL_PAD_ERROR_* */

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

/* ------------------------------------------------------------------ *
 * More reference constants: peripheral-class types, profile bits and
 * button offsets (guitar, drum, DJ deck, dancemat), BD remote codes.
 * ------------------------------------------------------------------ */
#define CELL_PAD_BTN_OFFSET_BD_LEN                   24
#define CELL_PAD_BTN_OFFSET_BD_CODE                  25
#define CELL_PAD_BTN_CODE_BD_OPKEY_1                 0x00
#define CELL_PAD_BTN_CODE_BD_OPKEY_2                 1
#define CELL_PAD_BTN_CODE_BD_OPKEY_3                 2
#define CELL_PAD_BTN_CODE_BD_OPKEY_4                 3
#define CELL_PAD_BTN_CODE_BD_OPKEY_5                 4
#define CELL_PAD_BTN_CODE_BD_OPKEY_6                 5
#define CELL_PAD_BTN_CODE_BD_OPKEY_7                 6
#define CELL_PAD_BTN_CODE_BD_OPKEY_8                 7
#define CELL_PAD_BTN_CODE_BD_OPKEY_9                 8
#define CELL_PAD_BTN_CODE_BD_OPKEY_0                 9
#define CELL_PAD_BTN_CODE_BD_ENTER                   0x0b
#define CELL_PAD_BTN_CODE_BD_MULTI_DIGIT             0x0c
#define CELL_PAD_BTN_CODE_BD_RETURN                  0x0e
#define CELL_PAD_BTN_CODE_BD_CLEAR                   0x0f
#define CELL_PAD_BTN_CODE_BD_TOPMENU                 0x1a
#define CELL_PAD_BTN_CODE_BD_TIME                    0x28
#define CELL_PAD_BTN_CODE_BD_PREV                    0x30
#define CELL_PAD_BTN_CODE_BD_NEXT                    49
#define CELL_PAD_BTN_CODE_BD_PLAY                    50
#define CELL_PAD_BTN_CODE_BD_SCAN_REV                51
#define CELL_PAD_BTN_CODE_BD_SCAN_FWD                52
#define CELL_PAD_BTN_CODE_BD_STOP                    0x38
#define CELL_PAD_BTN_CODE_BD_PAUSE                   57
#define CELL_PAD_BTN_CODE_BD_POPUP_MENU              0x40
#define CELL_PAD_BTN_CODE_BD_SELECT                  0x50
#define CELL_PAD_BTN_CODE_BD_L3                      81
#define CELL_PAD_BTN_CODE_BD_R3                      82
#define CELL_PAD_BTN_CODE_BD_START                   83
#define CELL_PAD_BTN_CODE_BD_UP                      0x54
#define CELL_PAD_BTN_CODE_BD_RIGHT                   85
#define CELL_PAD_BTN_CODE_BD_DOWN                    86
#define CELL_PAD_BTN_CODE_BD_LEFT                    87
#define CELL_PAD_BTN_CODE_BD_L2                      88
#define CELL_PAD_BTN_CODE_BD_R2                      89
#define CELL_PAD_BTN_CODE_BD_L1                      90
#define CELL_PAD_BTN_CODE_BD_R1                      91
#define CELL_PAD_BTN_CODE_BD_TRIANGLE                92
#define CELL_PAD_BTN_CODE_BD_CIRCLE                  93
#define CELL_PAD_BTN_CODE_BD_CROSS                   94
#define CELL_PAD_BTN_CODE_BD_SQUARE                  95
#define CELL_PAD_BTN_CODE_BD_SLOW_REV                0x60
#define CELL_PAD_BTN_CODE_BD_SLOW_FWD                0x61
#define CELL_PAD_BTN_CODE_BD_SUBTITLE                0x63
#define CELL_PAD_BTN_CODE_BD_AUDIO                   0x64
#define CELL_PAD_BTN_CODE_BD_ANGLE                   0x65
#define CELL_PAD_BTN_CODE_BD_DISPLAY                 0x70
#define CELL_PAD_BTN_CODE_BD_FLASH_FWD               0x75
#define CELL_PAD_BTN_CODE_BD_FLASH_REV               0x76
#define CELL_PAD_BTN_CODE_BD_BLUE                    0x80
#define CELL_PAD_BTN_CODE_BD_RED                     129
#define CELL_PAD_BTN_CODE_BD_GREEN                   130
#define CELL_PAD_BTN_CODE_BD_YELLOW                  131
#define CELL_PAD_BTN_CODE_BD_RELEASE                 0xff
#define CELL_PAD_BTN_CODE_BD_NUMBER_11               0x011e
#define CELL_PAD_BTN_CODE_BD_NUMBER_12               0x011f
#define CELL_PAD_BTN_CODE_BD_NUMBER_PERIOD           0x012a
#define CELL_PAD_BTN_CODE_BD_PROGRAM_UP              0x0130
#define CELL_PAD_BTN_CODE_BD_PROGRAM_DOWN            0x0131
#define CELL_PAD_BTN_CODE_BD_PREV_CHANNEL            0x0132
#define CELL_PAD_BTN_CODE_BD_PROGRAM_GUIDE           0x0153
#define CELL_PAD_BTN_CODE_BD_SACN_FWD                CELL_PAD_BTN_CODE_BD_SCAN_FWD
#define CELL_PAD_PCLASS_TYPE_STANDARD                0
#define CELL_PAD_PCLASS_TYPE_GUITAR                  1
#define CELL_PAD_PCLASS_TYPE_DRUM                    2
#define CELL_PAD_PCLASS_TYPE_DJ                      3
#define CELL_PAD_PCLASS_TYPE_DANCEMAT                4
#define CELL_PAD_PCLASS_TYPE_NAVIGATION              5
#define CELL_PAD_PCLASS_PROFILE_GUITAR_FRET_1        (1<<0)
#define CELL_PAD_PCLASS_PROFILE_GUITAR_FRET_2        (1<<1)
#define CELL_PAD_PCLASS_PROFILE_GUITAR_FRET_3        (1<<2)
#define CELL_PAD_PCLASS_PROFILE_GUITAR_FRET_4        (1<<3)
#define CELL_PAD_PCLASS_PROFILE_GUITAR_FRET_5        (1<<4)
#define CELL_PAD_PCLASS_PROFILE_GUITAR_STRUM_UP      (1<<5)
#define CELL_PAD_PCLASS_PROFILE_GUITAR_STRUM_DOWN    (1<<6)
#define CELL_PAD_PCLASS_PROFILE_GUITAR_WHAMMYBAR     (1<<7)
#define CELL_PAD_PCLASS_PROFILE_GUITAR_FRET_H1       (1<<8)
#define CELL_PAD_PCLASS_PROFILE_GUITAR_FRET_H2       (1<<9)
#define CELL_PAD_PCLASS_PROFILE_GUITAR_FRET_H3       (1<<10)
#define CELL_PAD_PCLASS_PROFILE_GUITAR_FRET_H4       (1<<11)
#define CELL_PAD_PCLASS_PROFILE_GUITAR_FRET_H5       (1<<12)
#define CELL_PAD_PCLASS_PROFILE_GUITAR_5WAY_EFFECT   (1<<13)
#define CELL_PAD_PCLASS_PROFILE_GUITAR_TILT_SENS     (1<<14)
#define CELL_PAD_PCLASS_BTN_OFFSET_GUITAR_FRET_1     24
#define CELL_PAD_PCLASS_BTN_OFFSET_GUITAR_FRET_2     25
#define CELL_PAD_PCLASS_BTN_OFFSET_GUITAR_FRET_3     26
#define CELL_PAD_PCLASS_BTN_OFFSET_GUITAR_FRET_4     27
#define CELL_PAD_PCLASS_BTN_OFFSET_GUITAR_FRET_5     28
#define CELL_PAD_PCLASS_BTN_OFFSET_GUITAR_STRUM_UP   29
#define CELL_PAD_PCLASS_BTN_OFFSET_GUITAR_STRUM_DOWN 30
#define CELL_PAD_PCLASS_BTN_OFFSET_GUITAR_WHAMMYBAR  31
#define CELL_PAD_PCLASS_BTN_OFFSET_GUITAR_FRET_H1    32
#define CELL_PAD_PCLASS_BTN_OFFSET_GUITAR_FRET_H2    33
#define CELL_PAD_PCLASS_BTN_OFFSET_GUITAR_FRET_H3    34
#define CELL_PAD_PCLASS_BTN_OFFSET_GUITAR_FRET_H4    35
#define CELL_PAD_PCLASS_BTN_OFFSET_GUITAR_FRET_H5    36
#define CELL_PAD_PCLASS_BTN_OFFSET_GUITAR_5WAY_EFFECT 37
#define CELL_PAD_PCLASS_BTN_OFFSET_GUITAR_TILT_SENS  38
#define CELL_PAD_PCLASS_PROFILE_DRUM_SNARE           (1<<0)
#define CELL_PAD_PCLASS_PROFILE_DRUM_TOM             (1<<1)
#define CELL_PAD_PCLASS_PROFILE_DRUM_TOM2            (1<<2)
#define CELL_PAD_PCLASS_PROFILE_DRUM_TOM_FLOOR       (1<<3)
#define CELL_PAD_PCLASS_PROFILE_DRUM_KICK            (1<<4)
#define CELL_PAD_PCLASS_PROFILE_DRUM_CYM_HiHAT       (1<<5)
#define CELL_PAD_PCLASS_PROFILE_DRUM_CYM_CRASH       (1<<6)
#define CELL_PAD_PCLASS_PROFILE_DRUM_CYM_RIDE        (1<<7)
#define CELL_PAD_PCLASS_PROFILE_DRUM_KICK2           (1<<8)
#define CELL_PAD_PCLASS_BTN_OFFSET_DRUM_SNARE        24
#define CELL_PAD_PCLASS_BTN_OFFSET_DRUM_TOM          25
#define CELL_PAD_PCLASS_BTN_OFFSET_DRUM_TOM2         26
#define CELL_PAD_PCLASS_BTN_OFFSET_DRUM_TOM_FLOOR    27
#define CELL_PAD_PCLASS_BTN_OFFSET_DRUM_KICK         28
#define CELL_PAD_PCLASS_BTN_OFFSET_DRUM_CYM_HiHAT    29
#define CELL_PAD_PCLASS_BTN_OFFSET_DRUM_CYM_CRASH    30
#define CELL_PAD_PCLASS_BTN_OFFSET_DRUM_CYM_RIDE     31
#define CELL_PAD_PCLASS_BTN_OFFSET_DRUM_KICK2        32
#define CELL_PAD_PCLASS_PROFILE_DJ_MIXER_ATTACK      (1<<0)
#define CELL_PAD_PCLASS_PROFILE_DJ_MIXER_CROSSFADER  (1<<1)
#define CELL_PAD_PCLASS_PROFILE_DJ_MIXER_DSP_DIAL    (1<<2)
#define CELL_PAD_PCLASS_PROFILE_DJ_DECK1_STREAM1     (1<<3)
#define CELL_PAD_PCLASS_PROFILE_DJ_DECK1_STREAM2     (1<<4)
#define CELL_PAD_PCLASS_PROFILE_DJ_DECK1_STREAM3     (1<<5)
#define CELL_PAD_PCLASS_PROFILE_DJ_DECK1_PLATTER     (1<<6)
#define CELL_PAD_PCLASS_PROFILE_DJ_DECK2_STREAM1     (1<<7)
#define CELL_PAD_PCLASS_PROFILE_DJ_DECK2_STREAM2     (1<<8)
#define CELL_PAD_PCLASS_PROFILE_DJ_DECK2_STREAM3     (1<<9)
#define CELL_PAD_PCLASS_PROFILE_DJ_DECK2_PLATTER     (1<<10)
#define CELL_PAD_PCLASS_BTN_OFFSET_DJ_MIXER_ATTACK   24
#define CELL_PAD_PCLASS_BTN_OFFSET_DJ_MIXER_CROSSFADER 25
#define CELL_PAD_PCLASS_BTN_OFFSET_DJ_MIXER_DSP_DIAL 26
#define CELL_PAD_PCLASS_BTN_OFFSET_DJ_DECK1_STREAM1  27
#define CELL_PAD_PCLASS_BTN_OFFSET_DJ_DECK1_STREAM2  28
#define CELL_PAD_PCLASS_BTN_OFFSET_DJ_DECK1_STREAM3  29
#define CELL_PAD_PCLASS_BTN_OFFSET_DJ_DECK1_PLATTER  30
#define CELL_PAD_PCLASS_BTN_OFFSET_DJ_DECK2_STREAM1  31
#define CELL_PAD_PCLASS_BTN_OFFSET_DJ_DECK2_STREAM2  32
#define CELL_PAD_PCLASS_BTN_OFFSET_DJ_DECK2_STREAM3  33
#define CELL_PAD_PCLASS_BTN_OFFSET_DJ_DECK2_PLATTER  34
#define CELL_PAD_PCLASS_PROFILE_DANCEMAT_CIRCLE      (1<<0)
#define CELL_PAD_PCLASS_PROFILE_DANCEMAT_CROSS       (1<<1)
#define CELL_PAD_PCLASS_PROFILE_DANCEMAT_TRIANGLE    (1<<2)
#define CELL_PAD_PCLASS_PROFILE_DANCEMAT_SQUARE      (1<<3)
#define CELL_PAD_PCLASS_PROFILE_DANCEMAT_RIGHT       (1<<4)
#define CELL_PAD_PCLASS_PROFILE_DANCEMAT_LEFT        (1<<5)
#define CELL_PAD_PCLASS_PROFILE_DANCEMAT_UP          (1<<6)
#define CELL_PAD_PCLASS_PROFILE_DANCEMAT_DOWN        (1<<7)
#define CELL_PAD_PCLASS_BTN_OFFSET_DANCEMAT_CIRCLE   24
#define CELL_PAD_PCLASS_BTN_OFFSET_DANCEMAT_CROSS    25
#define CELL_PAD_PCLASS_BTN_OFFSET_DANCEMAT_TRIANGLE 26
#define CELL_PAD_PCLASS_BTN_OFFSET_DANCEMAT_SQUARE   27
#define CELL_PAD_PCLASS_BTN_OFFSET_DANCEMAT_RIGHT    28
#define CELL_PAD_PCLASS_BTN_OFFSET_DANCEMAT_LEFT     29
#define CELL_PAD_PCLASS_BTN_OFFSET_DANCEMAT_UP       30
#define CELL_PAD_PCLASS_BTN_OFFSET_DANCEMAT_DOWN     31

#ifdef __cplusplus
}
#endif

#endif /* __PS3DK_CELL_PAD_H__ */
