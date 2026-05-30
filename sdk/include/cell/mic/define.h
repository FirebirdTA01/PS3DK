#ifndef __PS3DK_CELL_MIC_DEFINE_H__
#define __PS3DK_CELL_MIC_DEFINE_H__

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum CellMicSignalState {
    CELLMIC_SIGSTATE_LOCTALK = 0,
    CELLMIC_SIGSTATE_FARTALK = 1,
    CELLMIC_SIGSTATE_NSR     = 3,
    CELLMIC_SIGSTATE_AGC     = 4,
    CELLMIC_SIGSTATE_MICENG  = 5,
    CELLMIC_SIGSTATE_SPKENG  = 6
} CellMicSignalState;

typedef enum CellMicCommand {
    CELLMIC_INIT = 0,
    CELLMIC_END,
    CELLMIC_ATTACH,
    CELLMIC_DETACH,
    CELLMIC_SWITCH,
    CELLMIC_DATA,
    CELLMIC_OPEN,
    CELLMIC_CLOSE,
    CELLMIC_START,
    CELLMIC_STOP,
    CELLMIC_QUERY,
    CELLMIC_CONFIG,
    CELLMIC_CALLBACK,
    CELLMIC_RESET,
    CELLMIC_STATUS,
    CELLMIC_IPC,
    CELLMIC_CALLBACK2,
    CELLMIC_WEAK,
    CELLMIC_INIT2
} CellMicCommand;

typedef enum CellMicDeviceAttr {
    CELLMIC_DEVATTR_LED     = 9,
    CELLMIC_DEVATTR_GAIN    = 10,
    CELLMIC_DEVATTR_VOLUME  = 201,
    CELLMIC_DEVATTR_AGC     = 202,
    CELLMIC_DEVATTR_CHANVOL = 301,
    CELLMIC_DEVATTR_DSPTYPE = 302
} CellMicDeviceAttr;

typedef enum CellMicSignalAttr {
    CELLMIC_SIGATTR_BKNGAIN    = 0,
    CELLMIC_SIGATTR_REVERB     = 9,
    CELLMIC_SIGATTR_AGCLEVEL   = 26,
    CELLMIC_SIGATTR_VOLUME     = 301,
    CELLMIC_SIGATTR_PITCHSHIFT = 331
} CellMicSignalAttr;

typedef enum CellMicSignalType {
    CELLMIC_SIGTYPE_NULL = 0,
    CELLMIC_SIGTYPE_DSP  = 1,
    CELLMIC_SIGTYPE_AUX  = 2,
    CELLMIC_SIGTYPE_RAW  = 4
} CellMicSignalType;

typedef enum CellMicType {
    CELLMIC_TYPE_UNDEF     = -1,
    CELLMIC_TYPE_UNKNOWN   = 0,
    CELLMIC_TYPE_EYETOY1   = 1,
    CELLMIC_TYPE_EYETOY2   = 2,
    CELLMIC_TYPE_USBAUDIO  = 3,
    CELLMIC_TYPE_BLUETOOTH = 4,
    CELLMIC_TYPE_A2DP      = 5
} CellMicType;

enum {
    CELL_MAX_MICS = 8,
    MAX_MICS_PERMISSABLE = 4,
    CELL_MIC_STARTFLAG_LATENCY_4 = 0x00000001,
    CELL_MIC_STARTFLAG_LATENCY_2 = 0x00000002,
    CELL_MIC_STARTFLAG_LATENCY_1 = 0x00000003
};

#define CELLMIC_SIGTYPE_ALL \
    ((CellMicSignalType)(CELLMIC_SIGTYPE_DSP | CELLMIC_SIGTYPE_AUX | CELLMIC_SIGTYPE_RAW))

typedef struct CellMicInputFormatI {
    uint8_t channelNum;
    uint8_t subframeSize;
    uint8_t bitResolution;
    uint8_t dataType;
    uint32_t sampleRate;
} CellMicInputFormatI;

typedef struct CellMicInputStream {
    uint32_t uiBufferBottom;
    uint32_t uiBufferSize;
    uint32_t uiBuffer;
} CellMicInputStream;

typedef struct CellMicInputDefinition {
    uint32_t uiDevId;
    CellMicInputStream data;
    CellMicInputFormatI aux_format;
    CellMicInputFormatI raw_format;
    CellMicInputFormatI sig_format;
} CellMicInputDefinition;

typedef struct CellMicStatus {
    int32_t raw_samprate;
    int32_t dsp_samprate;
    int32_t dsp_volume;
    int32_t isStart;
    int32_t isOpen;
    int32_t local_voice;
    int32_t remote_voice;
    float mic_energy;
    float spk_energy;
} CellMicStatus;

#ifdef __cplusplus
}
#endif

#endif /* __PS3DK_CELL_MIC_DEFINE_H__ */
