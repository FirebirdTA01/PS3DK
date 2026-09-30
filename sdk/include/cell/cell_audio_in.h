/* cell/cell_audio_in.h - audio input devices (USB and Bluetooth microphones
 * and headsets) as the system utility sees them: device modes, what a device
 * reports about itself, and registration.  The functions are imported
 * through libsysutil_avconf_ext_stub.  Reached through
 * <sysutil/sysutil_sysparam.h>. */
#ifndef PS3TC_CELL_CELL_AUDIO_IN_H
#define PS3TC_CELL_CELL_AUDIO_IN_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CELL_AUDIO_IN_SUCCEEDED                    0
#define CELL_AUDIO_IN_ERROR_NOT_IMPLEMENTED        0x8002b260
#define CELL_AUDIO_IN_ERROR_ILLEGAL_CONFIGURATION  0x8002b261
#define CELL_AUDIO_IN_ERROR_ILLEGAL_PARAMETER      0x8002b262
#define CELL_AUDIO_IN_ERROR_PARAMETER_OUT_OF_RANGE 0x8002b263
#define CELL_AUDIO_IN_ERROR_DEVICE_NOT_FOUND       0x8002b264
#define CELL_AUDIO_IN_ERROR_UNSUPPORTED_AUDIO_IN   0x8002b265
#define CELL_AUDIO_IN_ERROR_UNSUPPORTED_SOUND_MODE 0x8002b266
#define CELL_AUDIO_IN_ERROR_CONDITION_BUSY         0x8002b267

typedef enum CellAudioInDeviceMode {
	CELL_AUDIO_IN_SINGLE_DEVICE_MODE  = 0,
	CELL_AUDIO_IN_MULTI_DEVICE_MODE   = 1,
	CELL_AUDIO_IN_MULTI_DEVICE_MODE_2 = 2,
	CELL_AUDIO_IN_MULTI_DEVICE_MODE_10 = 10
} CellAudioInDeviceMode;

typedef enum CellAudioInPortType {
	CELL_AUDIO_IN_PORT_USB       = 3,
	CELL_AUDIO_IN_PORT_BLUETOOTH = 4
} CellAudioInPortType;

typedef enum CellAudioInDeviceState {
	CELL_AUDIO_IN_DEVICE_STATE_UNAVAILABLE = 0,
	CELL_AUDIO_IN_DEVICE_STATE_AVAILABLE   = 1
} CellAudioInDeviceState;

typedef enum CellAudioInCodingType {
	CELL_AUDIO_IN_CODING_TYPE_LPCM = 0
} CellAudioInCodingType;

typedef enum CellAudioInChnum {
	CELL_AUDIO_IN_CHNUM_NONE = 0,
	CELL_AUDIO_IN_CHNUM_1    = 1,
	CELL_AUDIO_IN_CHNUM_2    = 2
} CellAudioInChnum;

/* sample-rate bits, combined in CellAudioInSoundMode.fs */
typedef enum CellAudioInFs {
	CELL_AUDIO_IN_FS_UNDEFINED = 0x00,
	CELL_AUDIO_IN_FS_8KHZ      = 0x01,
	CELL_AUDIO_IN_FS_12KHZ     = 0x02,
	CELL_AUDIO_IN_FS_16KHZ     = 0x04,
	CELL_AUDIO_IN_FS_24KHZ     = 0x08,
	CELL_AUDIO_IN_FS_32KHZ     = 0x10,
	CELL_AUDIO_IN_FS_48KHZ     = 0x20
} CellAudioInFs;

typedef struct CellAudioInSoundMode {
	uint8_t  type;        /* CellAudioInCodingType */
	uint8_t  channel;     /* CellAudioInChnum */
	uint16_t fs;          /* CellAudioInFs bits */
	uint8_t  reserved[4];
} CellAudioInSoundMode;

typedef struct CellAudioInDeviceInfo {
	uint8_t  portType;            /* CellAudioInPortType */
	uint8_t  availableModeCount;
	uint8_t  state;               /* CellAudioInDeviceState */
	uint8_t  deviceNumber;
	uint8_t  reserved[12];
	uint64_t deviceId;
	uint64_t type;
	char     name[64];
	CellAudioInSoundMode availableModes[16];
} CellAudioInDeviceInfo;

typedef struct CellAudioInDeviceConfiguration {
	uint8_t volume;
	uint8_t reserved[31];
} CellAudioInDeviceConfiguration;

typedef struct CellAudioInRegistrationOption {
	uint32_t reserved;
} CellAudioInRegistrationOption;

int cellAudioInSetDeviceMode(uint32_t deviceMode);
int cellAudioInRegisterDevice(uint64_t deviceType, const char *name,
                              CellAudioInRegistrationOption *option,
                              CellAudioInDeviceConfiguration *config);
int cellAudioInUnregisterDevice(uint32_t deviceNumber);
int cellAudioInGetDeviceInfo(uint32_t deviceNumber, uint32_t deviceIndex,
                             CellAudioInDeviceInfo *info);
int cellAudioInGetAvailableDeviceInfo(uint32_t count, CellAudioInDeviceInfo info[]);

#ifdef __cplusplus
}
#endif

#endif /* PS3TC_CELL_CELL_AUDIO_IN_H */
