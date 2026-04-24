/*! \file cell/cell_audio_out.h
 \brief cellAudioOut configuration / state surface (PS3 audio output,
        SPDIF / HDMI / analog / USB / Bluetooth / Network).

  Clean-room header matching the reference-SDK cellAudioOut surface
  (the one the reference SDK declares inside `sysutil_sysparam.h`
  alongside cellVideoOut).  Split into its own header here to keep
  sysutil_sysparam.h focused on system-parameter queries.

  Linkage: FNIDs land in libsysutil_audio_out_stub.a (produced by
  scripts/build-cell-stub-archives.sh from
  tools/nidgen/nids/extracted/libsysutil_audio_out_stub.yaml).  The
  module name "cellSysutil" appears in .rodata.sceResident so the
  runtime PRX loader binds libsysutil.sprx at launch.

  Workflow (typical game boot sequence):
    cellAudioOutGetNumberOfDevice(CELL_AUDIO_OUT_PRIMARY)
    cellAudioOutGetDeviceInfo(primary, 0, &info)         // query supported modes
    cellAudioOutConfigure(primary, &config, NULL, 0)     // commit channel/encoder
    cellAudioOutGetState(primary, 0, &state)             // observe state
*/

#ifndef __PS3DK_CELL_CELL_AUDIO_OUT_H__
#define __PS3DK_CELL_CELL_AUDIO_OUT_H__

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ *
 * Error codes
 * ------------------------------------------------------------------ */
#define CELL_AUDIO_OUT_SUCCEEDED                        0
#define CELL_AUDIO_OUT_ERROR_NOT_IMPLEMENTED            0x8002b240
#define CELL_AUDIO_OUT_ERROR_ILLEGAL_CONFIGURATION      0x8002b241
#define CELL_AUDIO_OUT_ERROR_ILLEGAL_PARAMETER          0x8002b242
#define CELL_AUDIO_OUT_ERROR_PARAMETER_OUT_OF_RANGE     0x8002b243
#define CELL_AUDIO_OUT_ERROR_DEVICE_NOT_FOUND           0x8002b244
#define CELL_AUDIO_OUT_ERROR_UNSUPPORTED_AUDIO_OUT      0x8002b245
#define CELL_AUDIO_OUT_ERROR_UNSUPPORTED_SOUND_MODE     0x8002b246
#define CELL_AUDIO_OUT_ERROR_CONDITION_BUSY             0x8002b247

/* cellAudioIn error codes (same SPRX; full cellAudioIn surface isn't
 * ported yet but sample code that handles BUSY uniformly for audio-in
 * needs these symbols resolvable). */
#define CELL_AUDIO_IN_SUCCEEDED                         0
#define CELL_AUDIO_IN_ERROR_NOT_IMPLEMENTED             0x8002b260
#define CELL_AUDIO_IN_ERROR_ILLEGAL_CONFIGURATION       0x8002b261
#define CELL_AUDIO_IN_ERROR_ILLEGAL_PARAMETER           0x8002b262
#define CELL_AUDIO_IN_ERROR_PARAMETER_OUT_OF_RANGE      0x8002b263
#define CELL_AUDIO_IN_ERROR_DEVICE_NOT_FOUND            0x8002b264
#define CELL_AUDIO_IN_ERROR_UNSUPPORTED_AUDIO_IN        0x8002b265
#define CELL_AUDIO_IN_ERROR_UNSUPPORTED_SOUND_MODE      0x8002b266
#define CELL_AUDIO_IN_ERROR_CONDITION_BUSY              0x8002b267

/* ------------------------------------------------------------------ *
 * Device selection
 * ------------------------------------------------------------------ */
typedef enum CellAudioOut {
    CELL_AUDIO_OUT_PRIMARY   = 0,
    CELL_AUDIO_OUT_SECONDARY = 1
} CellAudioOut;

typedef enum CellAudioOutPortType {
    CELL_AUDIO_OUT_PORT_HDMI      = 0,
    CELL_AUDIO_OUT_PORT_SPDIF     = 1,
    CELL_AUDIO_OUT_PORT_ANALOG    = 2,
    CELL_AUDIO_OUT_PORT_USB       = 3,
    CELL_AUDIO_OUT_PORT_BLUETOOTH = 4,
    CELL_AUDIO_OUT_PORT_NETWORK   = 5
} CellAudioOutPortType;

typedef enum CellAudioOutputState {
    CELL_AUDIO_OUT_OUTPUT_STATE_ENABLED   = 0,
    CELL_AUDIO_OUT_OUTPUT_STATE_DISABLED  = 1,
    CELL_AUDIO_OUT_OUTPUT_STATE_PREPARING = 2
} CellAudioOutOutputState;

typedef enum CellAudioOutDeviceState {
    CELL_AUDIO_OUT_DEVICE_STATE_UNAVAILABLE = 0,
    CELL_AUDIO_OUT_DEVICE_STATE_AVAILABLE   = 1
} CellAudioOutDeviceState;

/* ------------------------------------------------------------------ *
 * Coding / sample-rate / channel / bit-depth enumerations
 * ------------------------------------------------------------------ */
typedef enum CellAudioOutCodingType {
    CELL_AUDIO_OUT_CODING_TYPE_LPCM               = 0,
    CELL_AUDIO_OUT_CODING_TYPE_AC3                = 1,
    CELL_AUDIO_OUT_CODING_TYPE_MPEG1              = 2,
    CELL_AUDIO_OUT_CODING_TYPE_MP3                = 3,
    CELL_AUDIO_OUT_CODING_TYPE_MPEG2              = 4,
    CELL_AUDIO_OUT_CODING_TYPE_AAC                = 5,
    CELL_AUDIO_OUT_CODING_TYPE_DTS                = 6,
    CELL_AUDIO_OUT_CODING_TYPE_ATRAC              = 7,
    CELL_AUDIO_OUT_CODING_TYPE_DOLBY_DIGITAL_PLUS = 9,
    CELL_AUDIO_OUT_CODING_TYPE_BITSTREAM          = 0xff
} CellAudioOutCodingType;

typedef enum CellAudioOutFs {
    CELL_AUDIO_OUT_FS_32KHZ  = 0x01,
    CELL_AUDIO_OUT_FS_44KHZ  = 0x02,
    CELL_AUDIO_OUT_FS_48KHZ  = 0x04,
    CELL_AUDIO_OUT_FS_88KHZ  = 0x08,
    CELL_AUDIO_OUT_FS_96KHZ  = 0x10,
    CELL_AUDIO_OUT_FS_176KHZ = 0x20,
    CELL_AUDIO_OUT_FS_192KHZ = 0x40
} CellAudioOutFs;

typedef enum CellAudioOutChnum {
    CELL_AUDIO_OUT_CHNUM_2 = 2,
    CELL_AUDIO_OUT_CHNUM_4 = 4,
    CELL_AUDIO_OUT_CHNUM_6 = 6,
    CELL_AUDIO_OUT_CHNUM_8 = 8
} CellAudioOutChnum;

typedef enum CellAudioOutSbitOrBitrate {
    CELL_AUDIO_OUT_SBIT_NONE  = 0x00,
    CELL_AUDIO_OUT_SBIT_16BIT = 0x01,
    CELL_AUDIO_OUT_SBIT_20BIT = 0x02,
    CELL_AUDIO_OUT_SBIT_24BIT = 0x04
} CellAudioOutSbitOrBitrate;

typedef enum CellAudioOutSpeakerLayout {
    CELL_AUDIO_OUT_SPEAKER_LAYOUT_DEFAULT      = 0x00000000,
    CELL_AUDIO_OUT_SPEAKER_LAYOUT_2CH          = 0x00000001,
    CELL_AUDIO_OUT_SPEAKER_LAYOUT_6CH_LREClr   = 0x00010000,
    CELL_AUDIO_OUT_SPEAKER_LAYOUT_8CH_LREClrxy = 0x40000000
} CellAudioOutSpeakerLayout;

typedef enum CellAudioOutDownMixer {
    CELL_AUDIO_OUT_DOWNMIXER_NONE   = 0,
    CELL_AUDIO_OUT_DOWNMIXER_TYPE_A = 1,
    CELL_AUDIO_OUT_DOWNMIXER_TYPE_B = 2
} CellAudioOutDownMixer;

typedef enum CellAudioOutCopyControl {
    CELL_AUDIO_OUT_COPY_CONTROL_COPY_FREE  = 0,
    CELL_AUDIO_OUT_COPY_CONTROL_COPY_ONCE  = 1,
    CELL_AUDIO_OUT_COPY_CONTROL_COPY_NEVER = 2
} CellAudioOutCopyControl;

/* ------------------------------------------------------------------ *
 * Sound-mode / state / configuration structures
 * ------------------------------------------------------------------ */
typedef struct CellAudioOutSoundMode {
    uint8_t  type;       /* CellAudioOutCodingType */
    uint8_t  channel;    /* CellAudioOutChnum */
    uint8_t  fs;         /* CellAudioOutFs bitmask of supported rates */
    uint8_t  reserved;
    uint32_t layout;     /* CellAudioOutSpeakerLayout */
} CellAudioOutSoundMode;

typedef struct CellAudioOutState {
    uint8_t               state;      /* CellAudioOutOutputState */
    uint8_t               encoder;    /* CellAudioOutCodingType in use */
    uint8_t               reserved[6];
    uint32_t              downMixer;
    CellAudioOutSoundMode soundMode;
} CellAudioOutState;

typedef struct CellAudioOutDeviceInfo {
    uint8_t               portType;           /* CellAudioOutPortType */
    uint8_t               availableModeCount;
    uint8_t               state;              /* CellAudioOutDeviceState */
    uint8_t               reserved[3];
    uint16_t              latency;
    CellAudioOutSoundMode availableModes[16];
} CellAudioOutDeviceInfo;

typedef struct CellAudioOutConfiguration {
    uint8_t  channel;     /* CellAudioOutChnum */
    uint8_t  encoder;     /* CellAudioOutCodingType */
    uint8_t  reserved[10];
    uint32_t downMixer;
} CellAudioOutConfiguration;

typedef struct CellAudioOutOption {
    uint32_t reserved;
} CellAudioOutOption;

/* ------------------------------------------------------------------ *
 * Event / callback
 * ------------------------------------------------------------------ */
typedef enum CellAudioOutEvent {
    CELL_AUDIO_OUT_EVENT_DEVICE_CHANGED       = 0,
    CELL_AUDIO_OUT_EVENT_OUTPUT_DISABLED      = 1,
    CELL_AUDIO_OUT_EVENT_DEVICE_AUTHENTICATED = 2,
    CELL_AUDIO_OUT_EVENT_OUTPUT_ENABLED       = 3
} CellAudioOutEvent;

typedef int (*CellAudioOutCallback)(uint32_t slot,
                                    uint32_t audioOut,
                                    uint32_t deviceIndex,
                                    uint32_t event,
                                    CellAudioOutDeviceInfo *info,
                                    void *userData);

typedef uint32_t CellAudioOutLatency;

/* ------------------------------------------------------------------ *
 * Function prototypes (libsysutil SPRX exports)
 * ------------------------------------------------------------------ */
extern int cellAudioOutRegisterCallback(uint32_t slot,
                                        CellAudioOutCallback function,
                                        void *userData);
extern int cellAudioOutUnregisterCallback(uint32_t slot);

extern int cellAudioOutGetNumberOfDevice(uint32_t audioOut);
extern int cellAudioOutGetDeviceInfo(uint32_t audioOut,
                                     uint32_t deviceIndex,
                                     CellAudioOutDeviceInfo *info);
extern int cellAudioOutGetState(uint32_t audioOut,
                                uint32_t deviceIndex,
                                CellAudioOutState *state);

extern int cellAudioOutConfigure(uint32_t audioOut,
                                 CellAudioOutConfiguration *config,
                                 CellAudioOutOption *option,
                                 uint32_t waitForEvent);
extern int cellAudioOutGetConfiguration(uint32_t audioOut,
                                        CellAudioOutConfiguration *config,
                                        CellAudioOutOption *option);

extern int cellAudioOutGetSoundAvailability(uint32_t audioOut,
                                            uint32_t type,
                                            uint32_t fs,
                                            uint32_t option);
extern int cellAudioOutGetSoundAvailability2(uint32_t audioOut,
                                             uint32_t type,
                                             uint32_t fs,
                                             uint32_t channel,
                                             uint32_t option);

extern int cellAudioOutSetCopyControl(uint32_t audioOut, uint32_t control);

#ifdef __cplusplus
}
#endif

#endif /* __PS3DK_CELL_CELL_AUDIO_OUT_H__ */
