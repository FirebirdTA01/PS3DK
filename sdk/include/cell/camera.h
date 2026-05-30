#ifndef __PS3DK_CELL_CAMERA_H__
#define __PS3DK_CELL_CAMERA_H__

#include <cell/error.h>
#include <ppu-types.h>
#include <stdint.h>
#include <sys/event.h>
#include <sys/memory.h>
#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CELL_CAMERA_ERROR_ALREADY_INIT       0x80140801
#define CELL_CAMERA_ERROR_NOT_INIT           0x80140803
#define CELL_CAMERA_ERROR_PARAM              0x80140804
#define CELL_CAMERA_ERROR_ALREADY_OPEN       0x80140805
#define CELL_CAMERA_ERROR_NOT_OPEN           0x80140806
#define CELL_CAMERA_ERROR_DEVICE_NOT_FOUND   0x80140807
#define CELL_CAMERA_ERROR_DEVICE_DEACTIVATED 0x80140808
#define CELL_CAMERA_ERROR_NOT_STARTED        0x80140809
#define CELL_CAMERA_ERROR_FORMAT_UNKNOWN     0x8014080a
#define CELL_CAMERA_ERROR_RESOLUTION_UNKNOWN 0x8014080b
#define CELL_CAMERA_ERROR_BAD_FRAMERATE      0x8014080c
#define CELL_CAMERA_ERROR_TIMEOUT            0x8014080d
#define CELL_CAMERA_ERROR_BUSY               0x8014080e
#define CELL_CAMERA_ERROR_FATAL              0x8014080f
#define CELL_CAMERA_ERROR_MUTEX              0x80140810

#define CELL_CAMERA_MAX_CAMERAS 1

typedef enum CellCameraType {
    CELL_CAMERA_TYPE_UNKNOWN = 0,
    CELL_CAMERA_EYETOY = 1,
    CELL_CAMERA_EYETOY2 = 2,
    CELL_CAMERA_USBVIDEOCLASS = 3
} CellCameraType;

typedef enum CellCameraFormat {
    CELL_CAMERA_FORMAT_UNKNOWN = 0,
    CELL_CAMERA_JPG = 1,
    CELL_CAMERA_RAW8 = 2,
    CELL_CAMERA_YUV422 = 3,
    CELL_CAMERA_RAW10 = 4,
    CELL_CAMERA_RGBA = 5,
    CELL_CAMERA_YUV420 = 6,
    CELL_CAMERA_V_Y1_U_Y0 = 7,
    CELL_CAMERA_Y0_U_Y1_V = CELL_CAMERA_YUV422
} CellCameraFormat;

typedef enum CellCameraResolution {
    CELL_CAMERA_RESOLUTION_UNKNOWN = 0,
    CELL_CAMERA_VGA = 1,
    CELL_CAMERA_QVGA = 2,
    CELL_CAMERA_WGA = 3,
    CELL_CAMERA_SPECIFIED_WIDTH_HEIGHT = 4
} CellCameraResolution;

typedef enum CellCameraAttribute {
    CELL_CAMERA_GAIN = 0,
    CELL_CAMERA_REDBLUEGAIN = 1,
    CELL_CAMERA_SATURATION = 2,
    CELL_CAMERA_EXPOSURE = 3,
    CELL_CAMERA_BRIGHTNESS = 4,
    CELL_CAMERA_AEC = 5,
    CELL_CAMERA_AGC = 6,
    CELL_CAMERA_AWB = 7,
    CELL_CAMERA_ABC = 8,
    CELL_CAMERA_LED = 9,
    CELL_CAMERA_AUDIOGAIN = 10,
    CELL_CAMERA_QS = 11,
    CELL_CAMERA_NONZEROCOEFFS = 12,
    CELL_CAMERA_YUVFLAG = 13,
    CELL_CAMERA_JPEGFLAG = 14,
    CELL_CAMERA_BACKLIGHTCOMP = 15,
    CELL_CAMERA_MIRRORFLAG = 16,
    CELL_CAMERA_MEASUREDQS = 17,
    CELL_CAMERA_422FLAG = 18,
    CELL_CAMERA_USBLOAD = 19,
    CELL_CAMERA_GAMMA = 20,
    CELL_CAMERA_GREENGAIN = 21,
    CELL_CAMERA_AGCLIMIT = 22,
    CELL_CAMERA_DENOISE = 23,
    CELL_CAMERA_FRAMERATEADJUST = 24,
    CELL_CAMERA_PIXELOUTLIERFILTER = 25,
    CELL_CAMERA_AGCLOW = 26,
    CELL_CAMERA_AGCHIGH = 27,
    CELL_CAMERA_DEVICELOCATION = 28,
    CELL_CAMERA_FORMATCAP = 100,
    CELL_CAMERA_FORMATINDEX = 101,
    CELL_CAMERA_NUMFRAME = 102,
    CELL_CAMERA_FRAMEINDEX = 103,
    CELL_CAMERA_FRAMESIZE = 104,
    CELL_CAMERA_INTERVALTYPE = 105,
    CELL_CAMERA_INTERVALINDEX = 106,
    CELL_CAMERA_INTERVALVALUE = 107,
    CELL_CAMERA_COLORMATCHING = 108,
    CELL_CAMERA_PLFREQ = 109,
    CELL_CAMERA_DEVICEID = 110,
    CELL_CAMERA_DEVICECAP = 111,
    CELL_CAMERA_DEVICESPEED = 112,
    CELL_CAMERA_UVCREQCODE = 113,
    CELL_CAMERA_UVCREQDATA = 114,
    CELL_CAMERA_DEVICEID2 = 115,
    CELL_CAMERA_READMODE = 300,
    CELL_CAMERA_GAMEPID = 301,
    CELL_CAMERA_PBUFFER = 302,
    CELL_CAMERA_READFINISH = 303,
    CELL_CAMERA_ATTRIBUTE_UNKNOWN = 500
} CellCameraAttribute;

#define CELL_CAMERA_EFLAG_FRAME_UPDATE 0x00000001
#define CELL_CAMERA_EFLAG_OPEN         0x00000002
#define CELL_CAMERA_EFLAG_CLOSE        0x00000004
#define CELL_CAMERA_EFLAG_START        0x00000008
#define CELL_CAMERA_EFLAG_STOP         0x00000010
#define CELL_CAMERA_EFLAG_RESET        0x00000020

#define CELL_CAMERA_CM_CP_UNSPECIFIED 0
#define CELL_CAMERA_CM_CP_BT709_sRGB  1
#define CELL_CAMERA_CM_CP_BT470_2M    2
#define CELL_CAMERA_CM_CP_BT470_2BG   3
#define CELL_CAMERA_CM_CP_SMPTE170M   4
#define CELL_CAMERA_CM_CP_SMPTE240M   5

#define CELL_CAMERA_CM_TC_UNSPECIFIED 0
#define CELL_CAMERA_CM_TC_BT709       1
#define CELL_CAMERA_CM_TC_BT470_2M    2
#define CELL_CAMERA_CM_TC_BT470_2BG   3
#define CELL_CAMERA_CM_TC_SMPTE170M   4
#define CELL_CAMERA_CM_TC_SMPTE240M   5
#define CELL_CAMERA_CM_TC_LINEAR      6
#define CELL_CAMERA_CM_TC_sRGB        7

#define CELL_CAMERA_CM_MC_UNSPECIFIED 0
#define CELL_CAMERA_CM_MC_BT709       1
#define CELL_CAMERA_CM_MC_FCC         2
#define CELL_CAMERA_CM_MC_BT470_2BG   3
#define CELL_CAMERA_CM_MC_SMPTE170M   4
#define CELL_CAMERA_CM_MC_SMPTE240M   5

#define CELL_CAMERA_PLFREQ_DISABLED 0
#define CELL_CAMERA_PLFREQ_50Hz     1
#define CELL_CAMERA_PLFREQ_60Hz     2

#define CELL_CAMERA_CTC_SCANNING_MODE             (1u << 0)
#define CELL_CAMERA_CTC_AUTO_EXPOSURE_MODE        (1u << 1)
#define CELL_CAMERA_CTC_AUTO_EXPOSURE_PRIORITY    (1u << 2)
#define CELL_CAMERA_CTC_EXPOSURE_TIME_ABS         (1u << 3)
#define CELL_CAMERA_CTC_EXPOSURE_TIME_REL         (1u << 4)
#define CELL_CAMERA_CTC_FOCUS_ABS                 (1u << 5)
#define CELL_CAMERA_CTC_FOCUS_REL                 (1u << 6)
#define CELL_CAMERA_CTC_IRIS_ABS                  (1u << 7)
#define CELL_CAMERA_CTC_IRIS_REL                  (1u << 8)
#define CELL_CAMERA_CTC_ZOOM_ABS                  (1u << 9)
#define CELL_CAMERA_CTC_ZOOM_REL                  (1u << 10)
#define CELL_CAMERA_CTC_PANTILT_ABS               (1u << 11)
#define CELL_CAMERA_CTC_PANTILT_REL               (1u << 12)
#define CELL_CAMERA_CTC_ROLL_ABS                  (1u << 13)
#define CELL_CAMERA_CTC_ROLL_REL                  (1u << 14)
#define CELL_CAMERA_CTC_RESERVED_15               (1u << 15)
#define CELL_CAMERA_CTC_RESERVED_16               (1u << 16)
#define CELL_CAMERA_CTC_FOCUS_AUTO                (1u << 17)
#define CELL_CAMERA_CTC_PRIVACY                   (1u << 18)

#define CELL_CAMERA_PUC_BRIGHTNESS                     (1u << 0)
#define CELL_CAMERA_PUC_CONTRAST                       (1u << 1)
#define CELL_CAMERA_PUC_HUE                            (1u << 2)
#define CELL_CAMERA_PUC_SATURATION                     (1u << 3)
#define CELL_CAMERA_PUC_SHARPNESS                      (1u << 4)
#define CELL_CAMERA_PUC_GAMMA                          (1u << 5)
#define CELL_CAMERA_PUC_WHITE_BALANCE_TEMPERATURE      (1u << 6)
#define CELL_CAMERA_PUC_WHITE_BALANCE_COMPONENT        (1u << 7)
#define CELL_CAMERA_PUC_BACKLIGHT_COMPENSATION         (1u << 8)
#define CELL_CAMERA_PUC_GAIN                           (1u << 9)
#define CELL_CAMERA_PUC_POWER_LINE_FREQUENCY           (1u << 10)
#define CELL_CAMERA_PUC_HUE_AUTO                       (1u << 11)
#define CELL_CAMERA_PUC_WHITE_BALANCE_TEMPERATURE_AUTO (1u << 12)
#define CELL_CAMERA_PUC_WHITE_BALANCE_COMPONENT_AUTO   (1u << 13)
#define CELL_CAMERA_PUC_DIGITAL_MULTIPLIER             (1u << 14)
#define CELL_CAMERA_PUC_DIGITAL_MULTIPLIER_LIMIT       (1u << 15)
#define CELL_CAMERA_PUC_ANALOG_VIDEO_STANDARD          (1u << 16)
#define CELL_CAMERA_PUC_ANALOG_VIDEO_LOCK_STATUS       (1u << 17)

#define CELL_CAMERA_CS_SHIFT  0
#define CELL_CAMERA_CS_BITS   0x000000ff
#define CELL_CAMERA_CAP_SHIFT 8
#define CELL_CAMERA_CAP_BITS  0x0000ff00
#define CELL_CAMERA_NUM_SHIFT 16
#define CELL_CAMERA_NUM_BITS  0x000f0000
#define CELL_CAMERA_NUM_1     0x00010000
#define CELL_CAMERA_NUM_2     0x00020000
#define CELL_CAMERA_NUM_3     0x00030000
#define CELL_CAMERA_NUM_4     0x00040000
#define CELL_CAMERA_LEN_SHIFT 20
#define CELL_CAMERA_LEN_BITS  0x00f00000
#define CELL_CAMERA_LEN_1     0x00100000
#define CELL_CAMERA_LEN_2     0x00200000
#define CELL_CAMERA_LEN_4     0x00400000
#define CELL_CAMERA_ID_SHIFT  24
#define CELL_CAMERA_ID_BITS   0x0f000000
#define CELL_CAMERA_ID_CT     0x01000000
#define CELL_CAMERA_ID_SU     0x02000000
#define CELL_CAMERA_ID_PU     0x04000000

#define CELL_CAMERA_UVC_SCANNING_MODE                  0x01110001
#define CELL_CAMERA_UVC_AUTO_EXPOSURE_MODE             0x01110102
#define CELL_CAMERA_UVC_AUTO_EXPOSURE_PRIORITY         0x01110203
#define CELL_CAMERA_UVC_EXPOSURE_TIME_ABS              0x01410304
#define CELL_CAMERA_UVC_EXPOSURE_TIME_REL              0x01110405
#define CELL_CAMERA_UVC_FOCUS_ABS                      0x01210506
#define CELL_CAMERA_UVC_FOCUS_REL                      0x01120607
#define CELL_CAMERA_UVC_FOCUS_AUTO                     0x01111108
#define CELL_CAMERA_UVC_IRIS_ABS                       0x01210709
#define CELL_CAMERA_UVC_IRIS_REL                       0x0111080a
#define CELL_CAMERA_UVC_ZOOM_ABS                       0x0121090b
#define CELL_CAMERA_UVC_ZOOM_REL                       0x01130a0c
#define CELL_CAMERA_UVC_PANTILT_ABS                    0x01420b0d
#define CELL_CAMERA_UVC_PANTILT_REL                    0x01140c0e
#define CELL_CAMERA_UVC_ROLL_ABS                       0x01210d0f
#define CELL_CAMERA_UVC_ROLL_REL                       0x01120e10
#define CELL_CAMERA_UVC_PRIVACY                        0x01111211
#define CELL_CAMERA_UVC_INPUT_SELECT                   0x02110101
#define CELL_CAMERA_UVC_BACKLIGHT_COMPENSATION         0x04210801
#define CELL_CAMERA_UVC_BRIGHTNESS                     0x04210002
#define CELL_CAMERA_UVC_CONTRAST                       0x04210103
#define CELL_CAMERA_UVC_GAIN                           0x04210904
#define CELL_CAMERA_UVC_POWER_LINE_FREQUENCY           0x04110a05
#define CELL_CAMERA_UVC_HUE                            0x04210206
#define CELL_CAMERA_UVC_HUE_AUTO                       0x04110b10
#define CELL_CAMERA_UVC_SATURATION                     0x04210307
#define CELL_CAMERA_UVC_SHARPNESS                      0x04210408
#define CELL_CAMERA_UVC_GAMMA                          0x04210509
#define CELL_CAMERA_UVC_WHITE_BALANCE_TEMPERATURE      0x0421060a
#define CELL_CAMERA_UVC_WHITE_BALANCE_TEMPERATURE_AUTO 0x04110c0b
#define CELL_CAMERA_UVC_WHITE_BALANCE_COMPONENT        0x0422070c
#define CELL_CAMERA_UVC_WHITE_BALANCE_COMPONENT_AUTO   0x04110d0d
#define CELL_CAMERA_UVC_DIGITAL_MULTIPLIER             0x04210e0e
#define CELL_CAMERA_UVC_DIGITAL_MULTIPLIER_LIMIT       0x04210f0f
#define CELL_CAMERA_UVC_ANALOG_VIDEO_STANDARD          0x04111011
#define CELL_CAMERA_UVC_ANALOG_VIDEO_LOCK_STATUS       0x04111112

#define CELL_CAMERA_RC_CUR  0x81
#define CELL_CAMERA_RC_MIN  0x82
#define CELL_CAMERA_RC_MAX  0x83
#define CELL_CAMERA_RC_RES  0x84
#define CELL_CAMERA_RC_LEN  0x85
#define CELL_CAMERA_RC_INFO 0x86
#define CELL_CAMERA_RC_DEF  0x87

#define CELL_CAMERA_INFO_VER_100 0x0100
#define CELL_CAMERA_INFO_VER_101 0x0101
#define CELL_CAMERA_INFO_VER_200 0x0200
#define CELL_CAMERA_INFO_VER     CELL_CAMERA_INFO_VER_200

#define CELL_CAMERA_READ_FUNCCALL 0
#define CELL_CAMERA_READ_DIRECT   1

#define CELL_CAMERA_READ_VER_100 0x0100
#define CELL_CAMERA_READ_VER     CELL_CAMERA_READ_VER_100

#define CELL_CAMERA_ATTACH       1
#define CELL_CAMERA_DETACH       0
#define CELL_CAMERA_FRAME_UPDATE 2
#define CELL_CAMERA_OPEN         3
#define CELL_CAMERA_CLOSE        4
#define CELL_CAMERA_START        5
#define CELL_CAMERA_STOP         6
#define CELL_CAMERA_RESET        7

typedef struct CellCameraInfo {
    CellCameraFormat format;
    CellCameraResolution resolution;
    int framerate;
    uint8_t *buffer ATTRIBUTE_PRXPTR;
    int bytesize;
    int width;
    int height;
    int dev_num;
    int guid;
} CellCameraInfo;

typedef struct CellCameraInfoEx {
    CellCameraFormat format;
    CellCameraResolution resolution;
    int framerate;
    uint8_t *buffer ATTRIBUTE_PRXPTR;
    int bytesize;
    int width;
    int height;
    int dev_num;
    int guid;
    int info_ver;
    sys_memory_container_t container;
    int read_mode;
    uint32_t pbuf[2];
} CellCameraInfoEx;

typedef struct CellCameraReadEx {
    int version;
    uint32_t frame;
    uint32_t bytesread;
    system_time_t timestamp;
    uint8_t *pbuf ATTRIBUTE_PRXPTR;
} CellCameraReadEx;

int cellCameraInit(void);
int cellCameraEnd(void);
int cellCameraGetDeviceGUID(int dev_num, uint32_t *ptr_guid);
int cellCameraGetType(int dev_num, CellCameraType *type);
int cellCameraIsAttached(int dev_num);
int cellCameraIsAvailable(int dev_num);
int cellCameraIsOpen(int dev_num);
int cellCameraIsStarted(int dev_num);
int cellCameraGetBufferSize(int devnum, CellCameraInfoEx *info);
int cellCameraOpenEx(int devnum, CellCameraInfoEx *info);
int cellCameraClose(int dev_num);
int cellCameraStart(int dev_num);
int cellCameraStop(int dev_num);
int cellCameraReset(int dev_num);
int cellCameraRead(int dev_num, unsigned int *frame, unsigned int *bytesread);
int cellCameraReadEx(int dev_num, CellCameraReadEx *pRead);
int cellCameraGetBufferInfoEx(int dev_num, CellCameraInfoEx *info);
int cellCameraSetAttribute(int dev_num, CellCameraAttribute attribute, uint32_t arg1, uint32_t arg2);
int cellCameraGetAttribute(int dev_num, CellCameraAttribute attribute, uint32_t *arg1, uint32_t *arg2);
int cellCameraReadComplete(int devnum, uint32_t bufnum, uint32_t arg2);
int cellCameraPrepExtensionUnit(int devnum, uint8_t *guidExtensionCode);
int cellCameraCtrlExtensionUnit(int devnum, uint8_t bRequest, uint16_t wValue, uint16_t wLength, uint8_t *pData);
int cellCameraSetExtensionUnit(int devnum, uint16_t wValue, uint16_t wLength, uint8_t *pData);
int cellCameraGetExtensionUnit(int devnum, uint16_t wValue, uint16_t wLength, uint8_t *pData);
int cellCameraSetNotifyEventQueue(sys_ipc_key_t key);
int cellCameraRemoveNotifyEventQueue(sys_ipc_key_t key);
int cellCameraSetNotifyEventQueue2(sys_ipc_key_t key, uint64_t source, uint64_t flag);
int cellCameraRemoveNotifyEventQueue2(sys_ipc_key_t key);

#ifdef __cplusplus
}
#endif

#endif /* __PS3DK_CELL_CAMERA_H__ */
