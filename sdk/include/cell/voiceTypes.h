#ifndef __PS3DK_CELL_VOICE_TYPES_H__
#define __PS3DK_CELL_VOICE_TYPES_H__

#include <ppu-types.h>
#include <stdint.h>
#include <sys/memory.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CELL_VOICE_ERROR_LIBVOICE_NOT_INIT     0x80310801
#define CELL_VOICE_ERROR_LIBVOICE_INITIALIZED  0x80310802
#define CELL_VOICE_ERROR_GENERAL               0x80310803
#define CELL_VOICE_ERROR_PORT_INVALID          0x80310804
#define CELL_VOICE_ERROR_ARGUMENT_INVALID      0x80310805
#define CELL_VOICE_ERROR_CONTAINER_INVALID     0x80310806
#define CELL_VOICE_ERROR_TOPOLOGY              0x80310807
#define CELL_VOICE_ERROR_RESOURCE_INSUFFICIENT 0x80310808
#define CELL_VOICE_ERROR_NOT_IMPLEMENTED       0x80310809
#define CELL_VOICE_ERROR_ADDRESS_INVALID       0x8031080a
#define CELL_VOICE_ERROR_SERVICE_DETACHED      0x8031080b
#define CELL_VOICE_ERROR_SERVICE_ATTACHED      0x8031080c
#define CELL_VOICE_ERROR_SERVICE_NOT_FOUND     0x8031080d
#define CELL_VOICE_ERROR_SHAREDMEMORY          0x8031080e
#define CELL_VOICE_ERROR_EVENT_QUEUE           0x8031080f
#define CELL_VOICE_ERROR_SERVICE_HANDLE        0x80310810
#define CELL_VOICE_ERROR_EVENT_DISPATCH        0x80310811
#define CELL_VOICE_ERROR_DEVICE_NOT_PRESENT    0x80310812

typedef enum CellVoiceAppType {
    CELLVOICE_APPTYPE_GAME_1MB = 1u << 29
} CellVoiceAppType;

typedef enum CellVoiceBitRate {
    CELLVOICE_BITRATE_NULL  = 0xffffffffu,
    CELLVOICE_BITRATE_3850  = 3850,
    CELLVOICE_BITRATE_4650  = 4650,
    CELLVOICE_BITRATE_5700  = 5700,
    CELLVOICE_BITRATE_7300  = 7300,
    CELLVOICE_BITRATE_14400 = 14400,
    CELLVOICE_BITRATE_16000 = 16000,
    CELLVOICE_BITRATE_22533 = 22533
} CellVoiceBitRate;

typedef enum CellVoiceEventType {
    CELLVOICE_EVENT_DATA_ERROR         = 1u << 0,
    CELLVOICE_EVENT_PORT_ATTACHED      = 1u << 1,
    CELLVOICE_EVENT_PORT_DETACHED      = 1u << 2,
    CELLVOICE_EVENT_SERVICE_ATTACHED   = 1u << 3,
    CELLVOICE_EVENT_SERVICE_DETACHED   = 1u << 4,
    CELLVOICE_EVENT_PORT_WEAK_ATTACHED = 1u << 5,
    CELLVOICE_EVENT_PORT_WEAK_DETACHED = 1u << 6
} CellVoiceEventType;

typedef enum CellVoicePcmDataType {
    CELLVOICE_PCM_NULL                  = 0xffffffffu,
    CELLVOICE_PCM_FLOAT                 = 0,
    CELLVOICE_PCM_FLOAT_LITTLE_ENDIAN   = 1,
    CELLVOICE_PCM_SHORT                 = 2,
    CELLVOICE_PCM_SHORT_LITTLE_ENDIAN   = 3,
    CELLVOICE_PCM_INTEGER               = 4,
    CELLVOICE_PCM_INTEGER_LITTLE_ENDIAN = 5
} CellVoicePcmDataType;

typedef enum CellVoicePortAttr {
    CELLVOICE_ATTR_ENERGY_LEVEL       = 1000,
    CELLVOICE_ATTR_VAD                = 1001,
    CELLVOICE_ATTR_DTX                = 1002,
    CELLVOICE_ATTR_AUTO_RESAMPLE      = 1003,
    CELLVOICE_ATTR_LATENCY            = 1004,
    CELLVOICE_ATTR_SILENCE_THRESHOLD  = 1005
} CellVoicePortAttr;

typedef enum CellVoicePortState {
    CELLVOICE_PORTSTATE_NULL      = 0xffffffffu,
    CELLVOICE_PORTSTATE_IDLE      = 0,
    CELLVOICE_PORTSTATE_READY     = 1,
    CELLVOICE_PORTSTATE_BUFFERING = 2,
    CELLVOICE_PORTSTATE_RUNNING   = 3
} CellVoicePortState;

typedef enum CellVoicePortType {
    CELLVOICE_PORTTYPE_NULL          = 0xffffffffu,
    CELLVOICE_PORTTYPE_IN_MIC        = 0,
    CELLVOICE_PORTTYPE_IN_PCMAUDIO   = 1,
    CELLVOICE_PORTTYPE_IN_VOICE      = 2,
    CELLVOICE_PORTTYPE_OUT_PCMAUDIO  = 3,
    CELLVOICE_PORTTYPE_OUT_VOICE     = 4,
    CELLVOICE_PORTTYPE_OUT_SECONDARY = 5
} CellVoicePortType;

typedef enum CellVoiceSamplingRate {
    CELLVOICE_SAMPLINGRATE_NULL  = 0xffffffffu,
    CELLVOICE_SAMPLINGRATE_16000 = 16000
} CellVoiceSamplingRate;

typedef enum CellVoiceVersionCheck {
    CELLVOICE_VERSION_100 = 100
} CellVoiceVersionCheck;

enum {
    CELLVOICE_MAX_IN_VOICE_PORT           = 32,
    CELLVOICE_MAX_OUT_VOICE_PORT          = 4,
    CELLVOICE_GAME_1MB_MAX_IN_VOICE_PORT  = 8,
    CELLVOICE_GAME_1MB_MAX_OUT_VOICE_PORT = 2,
    CELLVOICE_MAX_PORT                    = 128,
    CELLVOICE_INVALID_PORT_ID             = 0xff
};

typedef struct __attribute__((aligned(64))) CellVoiceBasePortInfo {
    CellVoicePortType portType;
    CellVoicePortState state;
    uint16_t numEdge;
    uint16_t reserved0;
    uint32_t *pEdge ATTRIBUTE_PRXPTR;
    uint32_t numByte;
    uint32_t frameSize;
} CellVoiceBasePortInfo;

typedef struct __attribute__((aligned(32))) CellVoiceInitParam {
    CellVoiceEventType eventMask;
    CellVoiceVersionCheck version;
    int32_t appType;
    uint8_t reserved[20];
} CellVoiceInitParam;

typedef struct CellVoicePCMFormat {
    uint8_t numChannels;
    uint8_t sampleAlignment;
    uint16_t reserved0;
    CellVoicePcmDataType dataType;
    CellVoiceSamplingRate sampleRate;
} CellVoicePCMFormat;

typedef struct __attribute__((aligned(64))) CellVoicePortParam {
    CellVoicePortType portType;
    uint16_t threshold;
    uint16_t bMute;
    float volume;
    union {
        struct {
            CellVoiceBitRate bitrate;
        } voice;
        struct {
            uint32_t bufSize;
            CellVoicePCMFormat format;
        } pcmaudio;
        struct {
            uint32_t playerId;
        } device;
    };
} CellVoicePortParam;

typedef struct __attribute__((aligned(32))) CellVoiceStartParam {
    sys_memory_container_t container;
    uint8_t reserved[28];
} CellVoiceStartParam;

#ifdef __cplusplus
}
#endif

#endif /* __PS3DK_CELL_VOICE_TYPES_H__ */
