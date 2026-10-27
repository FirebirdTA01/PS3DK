/*
 * cell/gem/libgem.h - PlayStation Move (libgem) PPU interface.
 *
 * libgem tracks up to four Move motion controllers.  The controller's
 * buttons, trigger and inertial sensors (accelerometer, gyroscope,
 * magnetometer) arrive over Bluetooth; position comes from finding the
 * lit sphere in PlayStation Eye frames, which the library analyses on
 * SPUs through a caller-supplied SPURS instance.
 *
 * Typical frame loop:
 *
 *   cellGemGetMemorySize(n)          size the work area
 *   cellGemAttributeInit(...)        fill a CellGemAttribute
 *   cellGemInit(&attr)               once; needs SPURS
 *   cellGemPrepareCamera(...)        after the camera is open
 *   per camera frame:
 *     cellGemUpdateStart(frame, t)   hand the frame to the tracker
 *     cellGemUpdateFinish()          wait for the SPU work
 *     cellGemGetState(...)           read pose, buttons, flags
 *   cellGemEnd()
 *
 * Buttons and inertial data (cellGemGetInertialState, the pad fields of
 * CellGemState) work without a camera; position does not.
 *
 * The structures below are shared with the libgem PRX, so their layout
 * is the PRX's 32-bit layout in both data models: pointer members carry
 * ATTRIBUTE_PRXPTR and stay 4 bytes under -mlp64.  The vector members
 * keep CellGemState and CellGemInertialState 16-byte aligned; allocate
 * them accordingly.
 *
 * Legacy PSL1GHT code uses <io/move.h> (PSL1GHT's libgem.a), whose
 * functions call the same PRX entry points: gemXxx() is cellGemXxx(),
 * gemState is CellGemState, and so on.  Its constants are spelled
 * differently (MAX_MOVES, GEM_*, STATE_*) and its external-port write
 * buffer is 32 bytes where CELL_GEM_EXTERNAL_PORT_OUTPUT_SIZE is 40, so
 * port code does not translate one-to-one.  The two headers can be
 * included together; their types are distinct and do not interconvert.
 */
#ifndef __PS3DK_CELL_GEM_LIBGEM_H__
#define __PS3DK_CELL_GEM_LIBGEM_H__

#include <cell/error.h>
#include <cell/spurs.h>
#include <ppu-types.h>
#include <stdint.h>
#include <sys/types.h>
#include <vec_types.h>

/* Value written to the version field of both attribute structures. */
#define CELL_GEM_VERSION                2

/* Controllers the library can track at once. */
#define CELL_GEM_MAX_NUM                (4)

/* Radius of the controller's sphere, in millimetres. */
#define CELL_GEM_SPHERE_RADIUS_MM       22.5f
#define CELL_GEM_LATENCY_OFFSET         (-22000)

/* CellGemInfo.status[] values. */
#define CELL_GEM_STATUS_DISCONNECTED    (0)
#define CELL_GEM_STATUS_READY           (1)

/* CellGemPadData.digitalbuttons bits. */
#define CELL_GEM_CTRL_SELECT            (1 << 0)
#define CELL_GEM_CTRL_T                 (1 << 1)
#define CELL_GEM_CTRL_MOVE              (1 << 2)
#define CELL_GEM_CTRL_START             (1 << 3)
#define CELL_GEM_CTRL_TRIANGLE          (1 << 4)
#define CELL_GEM_CTRL_CIRCLE            (1 << 5)
#define CELL_GEM_CTRL_CROSS             (1 << 6)
#define CELL_GEM_CTRL_SQUARE            (1 << 7)

/* CellGemExtPortData.status bits for a device on the expansion port. */
#define CELL_GEM_EXT_CONNECTED          (1 << 0)
#define CELL_GEM_EXT_EXT0               (1 << 1)
#define CELL_GEM_EXT_EXT1               (1 << 2)

/* Expansion-port buffer sizes, in bytes. */
#define CELL_GEM_EXTERNAL_PORT_DEVICE_INFO_SIZE 38
#define CELL_GEM_EXTERNAL_PORT_OUTPUT_SIZE      40

/* cellGemPrepareCamera() exposure limits, in camera exposure units. */
#define CELL_GEM_MIN_CAMERA_EXPOSURE    40
#define CELL_GEM_MAX_CAMERA_EXPOSURE    511

/* cellGemGetState() flag: which moment the returned pose describes. */
#define CELL_GEM_STATE_FLAG_CURRENT_TIME        0
#define CELL_GEM_STATE_FLAG_LATEST_IMAGE_TIME   1
#define CELL_GEM_STATE_FLAG_TIMESTAMP           2

/* cellGemGetInertialState() flag: which sensor sample to return. */
#define CELL_GEM_INERTIAL_STATE_FLAG_LATEST     0
#define CELL_GEM_INERTIAL_STATE_FLAG_PREVIOUS   1
#define CELL_GEM_INERTIAL_STATE_FLAG_NEXT       2

/*
 * Positive (non-error) results of the state queries: the call worked,
 * but the data is limited for the reason given.
 */
#define CELL_GEM_NOT_CONNECTED                  1
#define CELL_GEM_SPHERE_NOT_CALIBRATED          2
#define CELL_GEM_SPHERE_CALIBRATING             3
#define CELL_GEM_COMPUTING_AVAILABLE_COLORS     4
#define CELL_GEM_HUE_NOT_SET                    5
#define CELL_GEM_NO_VIDEO                       6
#define CELL_GEM_TIME_OUT_OF_RANGE              7
#define CELL_GEM_NOT_CALIBRATED                 8
#define CELL_GEM_NO_EXTERNAL_PORT_DEVICE        9

/* Special hue requests for cellGemTrackHues(). */
#define CELL_GEM_DONT_TRACK_HUE         (2 << 24)
#define CELL_GEM_DONT_CARE_HUE          (4 << 24)
#define CELL_GEM_DONT_CHANGE_HUE        (8 << 24)

/* cellGemGetStatusFlags() / cellGemClearStatusFlags() bits. */
#define CELL_GEM_FLAG_CALIBRATION_OCCURRED                   (1 << 0)
#define CELL_GEM_FLAG_CALIBRATION_SUCCEEDED                  (1 << 1)
#define CELL_GEM_FLAG_CALIBRATION_FAILED_CANT_FIND_SPHERE    (1 << 2)
#define CELL_GEM_FLAG_CALIBRATION_FAILED_MOTION_DETECTED     (1 << 3)
#define CELL_GEM_FLAG_CALIBRATION_FAILED_BRIGHT_LIGHTING     (1 << 4)
#define CELL_GEM_FLAG_CALIBRATION_WARNING_MOTION_DETECTED    (1 << 5)
#define CELL_GEM_FLAG_CALIBRATION_WARNING_BRIGHT_LIGHTING    (1 << 6)
#define CELL_GEM_FLAG_LIGHTING_CHANGED                       (1 << 7)
#define CELL_GEM_FLAG_WRONG_FIELD_OF_VIEW_SETTING            (1 << 8)
#define CELL_GEM_FLAG_CAMERA_PITCH_ANGLE_CHANGED             (1 << 9)
#define CELL_GEM_FLAG_VARIABLE_MAGNETIC_FIELD                (1 << 10)
#define CELL_GEM_FLAG_WEAK_MAGNETIC_FIELD                    (1 << 11)
#define CELL_GEM_FLAG_VERY_COLORFUL_ENVIRONMENT              (1 << 12)
#define CELL_GEM_FLAG_CURRENT_HUE_CONFLICTS_WITH_ENVIRONMENT (1 << 13)
/* Mask for clearing every status flag. */
#define CELL_GEM_ALL_FLAGS              0xffffffffffffffffull

/* CellGemState.tracking_flags bits. */
#define CELL_GEM_TRACKING_FLAG_POSITION_TRACKED (1 << 0)
#define CELL_GEM_TRACKING_FLAG_VISIBLE          (1 << 1)

/* Error codes (facility 0x0121, range 0x18xx). */
#define CELL_GEM_ERROR_RESOURCE_ALLOCATION_FAILED CELL_ERROR_CAST(0x80121801)
#define CELL_GEM_ERROR_ALREADY_INITIALIZED        CELL_ERROR_CAST(0x80121802)
#define CELL_GEM_ERROR_UNINITIALIZED              CELL_ERROR_CAST(0x80121803)
#define CELL_GEM_ERROR_INVALID_PARAMETER          CELL_ERROR_CAST(0x80121804)
#define CELL_GEM_ERROR_INVALID_ALIGNMENT          CELL_ERROR_CAST(0x80121805)
#define CELL_GEM_ERROR_UPDATE_NOT_FINISHED        CELL_ERROR_CAST(0x80121806)
#define CELL_GEM_ERROR_UPDATE_NOT_STARTED         CELL_ERROR_CAST(0x80121807)
#define CELL_GEM_ERROR_CONVERT_NOT_FINISHED       CELL_ERROR_CAST(0x80121808)
#define CELL_GEM_ERROR_CONVERT_NOT_STARTED        CELL_ERROR_CAST(0x80121809)
#define CELL_GEM_ERROR_WRITE_NOT_FINISHED         CELL_ERROR_CAST(0x8012180A)
#define CELL_GEM_ERROR_NOT_A_HUE                  CELL_ERROR_CAST(0x8012180B)

/* cellGemGetInfo(): how many controllers are connected, and where. */
typedef struct CellGemInfo {
    uint32_t max_connect;               /* as passed to cellGemInit */
    uint32_t now_connect;               /* currently connected */
    uint32_t status[CELL_GEM_MAX_NUM];  /* CELL_GEM_STATUS_* per slot */
    uint32_t port[CELL_GEM_MAX_NUM];
} CellGemInfo;

/* Buttons and trigger. */
typedef struct CellGemPadData {
    uint16_t digitalbuttons;            /* CELL_GEM_CTRL_* bits */
    uint16_t analog_T;                  /* trigger */
} CellGemPadData;

/* Device plugged into the controller's expansion port, if any. */
typedef struct CellGemExtPortData {
    uint16_t status;                    /* CELL_GEM_EXT_* bits */
    uint16_t digital1;
    uint16_t digital2;
    uint16_t analog_right_x;
    uint16_t analog_right_y;
    uint16_t analog_left_x;
    uint16_t analog_left_y;
    unsigned char custom[5];
} CellGemExtPortData;

/*
 * Full controller state from cellGemGetState(): sphere position and
 * motion, orientation quaternion and angular motion, and the same
 * position and motion for the handle.
 */
typedef struct CellGemState {
    vec_float4 pos;
    vec_float4 vel;
    vec_float4 accel;
    vec_float4 quat;
    vec_float4 angvel;
    vec_float4 angaccel;
    vec_float4 handle_pos;
    vec_float4 handle_vel;
    vec_float4 handle_accel;
    CellGemPadData pad;
    CellGemExtPortData ext;
    system_time_t timestamp;
    float temperature;
    float camera_pitch_angle;
    uint32_t tracking_flags;            /* CELL_GEM_TRACKING_FLAG_* */
} CellGemState;

/* One raw sensor sample from cellGemGetInertialState(). */
typedef struct CellGemInertialState {
    vec_float4 accelerometer;
    vec_float4 gyro;
    vec_float4 accelerometer_bias;
    vec_float4 gyro_bias;
    CellGemPadData pad;
    CellGemExtPortData ext;
    system_time_t timestamp;
    int counter;
    float temperature;
} CellGemInertialState;

/* Where the sphere appeared in the most recent analysed camera frame. */
typedef struct CellGemImageState {
    system_time_t frame_timestamp;
    system_time_t timestamp;
    float u;                            /* sphere centre in the image */
    float v;
    float r;                            /* sphere radius in the image */
    float projectionx;
    float projectiony;
    float distance;
    unsigned char visible;
    unsigned char r_valid;
} CellGemImageState;

/* Camera settings the library chose, from cellGemGetCameraState(). */
typedef struct CellGemCameraState {
    int exposure;
    float exposure_time;
    float gain;
    float pitch_angle;
    float pitch_angle_estimate;
} CellGemCameraState;

/* cellGemInit() parameters; fill with cellGemAttributeInit(). */
typedef struct CellGemAttribute {
    uint32_t version;                   /* CELL_GEM_VERSION */
    uint32_t max_connect;               /* 1..CELL_GEM_MAX_NUM */
    void *memory_ptr ATTRIBUTE_PRXPTR;  /* cellGemGetMemorySize() bytes */
    CellSpurs *spurs ATTRIBUTE_PRXPTR;  /* instance that runs the tracker */
    uint8_t spu_priorities[8];          /* SPURS workload priority per SPU */
} CellGemAttribute;

/* Output of the optional camera-frame conversion (cellGemConvertVideo*). */
typedef enum {
    CELL_GEM_NO_VIDEO_OUTPUT = 1,
    CELL_GEM_RGBA_640x480,
    CELL_GEM_YUV_640x480,
    CELL_GEM_YUV422_640x480,
    CELL_GEM_YUV411_640x480,
    CELL_GEM_RGBA_320x240,
    CELL_GEM_BAYER_RESTORED,
    CELL_GEM_BAYER_RESTORED_RGGB,
    CELL_GEM_BAYER_RESTORED_RASTERIZED
} CellGemVideoConvertFormatEnum;

/* Processing steps for the conversion; combine with |. */
typedef enum {
    CELL_GEM_AUTO_WHITE_BALANCE = 0x1,
    CELL_GEM_GAMMA_BOOST = 0x2,
    CELL_GEM_COMBINE_PREVIOUS_INPUT_FRAME = 0x4,
    CELL_GEM_FILTER_OUTLIER_PIXELS = 0x8
} CellGemVideoConvertFlagEnum;

/* cellGemPrepareVideoConvert() parameters. */
typedef struct CellGemVideoConvertAttribute {
    int version;                        /* CELL_GEM_VERSION */
    int output_format;                  /* CellGemVideoConvertFormatEnum */
    int conversion_flags;               /* CellGemVideoConvertFlagEnum bits */
    float gain;
    float red_gain;
    float green_gain;
    float blue_gain;
    /* Previous-frame buffer for CELL_GEM_COMBINE_PREVIOUS_INPUT_FRAME. */
    void *buffer_memory ATTRIBUTE_PRXPTR;
    /* Destination, cellGemGetVideoConvertSize(output_format) bytes. */
    void *video_data_out ATTRIBUTE_PRXPTR;
    uint8_t alpha;                      /* alpha for RGBA output */
} CellGemVideoConvertAttribute;

#ifdef __cplusplus
extern "C" {
#endif

/* Work-area size cellGemInit needs for max_connect controllers. */
int32_t cellGemGetMemorySize(int max_connect);

/* Fill attr for cellGemInit().  spu_priorities must have 8 entries. */
static inline void cellGemAttributeInit(CellGemAttribute *attr,
                                        uint32_t max_connect,
                                        void *memory_ptr,
                                        CellSpurs *spurs,
                                        const uint8_t spu_priorities[8])
{
    unsigned int spu;

    attr->version = CELL_GEM_VERSION;
    attr->max_connect = max_connect;
    attr->memory_ptr = memory_ptr;
    attr->spurs = spurs;
    for (spu = 0; spu < sizeof attr->spu_priorities; spu++)
        attr->spu_priorities[spu] = spu_priorities[spu];
}

int32_t cellGemInit(const CellGemAttribute *attr);
int32_t cellGemEnd(void);

/* Let the library manage camera exposure, up to max_exposure. */
int32_t cellGemPrepareCamera(int max_exposure, float image_quality);

/*
 * Bytes a converted frame occupies in the given output format, or -1
 * for an unknown format.  The 640x480 YUV formats are planar: a full
 * luma plane plus two chroma planes subsampled 2:1 horizontally (422)
 * or 2:1 in both directions (411).
 */
static inline int32_t cellGemGetVideoConvertSize(int output_format)
{
    const int32_t vga = 640 * 480, qvga = 320 * 240;

    switch (output_format) {
    case CELL_GEM_NO_VIDEO_OUTPUT:           return 0;
    case CELL_GEM_RGBA_640x480:              return vga * 4;
    case CELL_GEM_YUV_640x480:               return vga * 3;
    case CELL_GEM_YUV422_640x480:            return vga + 2 * (vga / 2);
    case CELL_GEM_YUV411_640x480:            return vga + 2 * (vga / 4);
    case CELL_GEM_RGBA_320x240:              return qvga * 4;
    case CELL_GEM_BAYER_RESTORED:            return vga;
    case CELL_GEM_BAYER_RESTORED_RGGB:       return qvga * 4;
    case CELL_GEM_BAYER_RESTORED_RASTERIZED: return qvga * 4;
    default:                                 return -1;
    }
}

/* Fill attr for cellGemPrepareVideoConvert(). */
static inline void cellGemVideoConvertAttributeInit(
    CellGemVideoConvertAttribute *attr, int output_format,
    int conversion_flags, float gain, float red_gain, float green_gain,
    float blue_gain, void *buffer_memory, void *video_data_out,
    uint8_t alpha)
{
    attr->version = CELL_GEM_VERSION;
    attr->output_format = output_format;
    attr->conversion_flags = conversion_flags;
    attr->gain = gain;
    attr->red_gain = red_gain;
    attr->green_gain = green_gain;
    attr->blue_gain = blue_gain;
    attr->buffer_memory = buffer_memory;
    attr->video_data_out = video_data_out;
    attr->alpha = alpha;
}

/*
 * The usual setup: opaque 640x480 RGBA with white balance, gamma boost
 * and outlier filtering at unit gains.  Passing a frame_buffer (a
 * previous-frame buffer) also enables frame combining.
 */
static inline void cellGemVideoConvertAttributeInitRgba(
    CellGemVideoConvertAttribute *attr, void *rgba_out, void *frame_buffer)
{
    int flags = CELL_GEM_AUTO_WHITE_BALANCE | CELL_GEM_GAMMA_BOOST |
                CELL_GEM_FILTER_OUTLIER_PIXELS;

    if (frame_buffer != 0)
        flags |= CELL_GEM_COMBINE_PREVIOUS_INPUT_FRAME;
    cellGemVideoConvertAttributeInit(attr, CELL_GEM_RGBA_640x480, flags,
                                     1.0f, 1.0f, 1.0f, 1.0f,
                                     frame_buffer, rgba_out, 0xff);
}

/* Camera-frame conversion; Start queues SPU work, Finish waits for it. */
int32_t cellGemPrepareVideoConvert(const CellGemVideoConvertAttribute *attr);
int32_t cellGemConvertVideoStart(const void *video_frame);
int32_t cellGemConvertVideoFinish(void);

/* Tracking update for one camera frame; call Finish before the next Start. */
int32_t cellGemUpdateStart(const void *camera_frame, system_time_t timestamp);
int32_t cellGemUpdateFinish(void);

/* CELL_GEM_FLAG_* status bits per controller. */
int32_t cellGemGetStatusFlags(int gem_num, uint64_t *flags);
int32_t cellGemClearStatusFlags(int gem_num, uint64_t mask);

/* State queries; flag selects the moment (CELL_GEM_*STATE_FLAG_*). */
int32_t cellGemGetState(uint32_t gem_num, uint32_t flag,
                        system_time_t time_parameter, CellGemState *gem_state);
int32_t cellGemGetInertialState(uint32_t gem_num, uint32_t flag,
                                system_time_t timestamp,
                                CellGemInertialState *gem_inertial_state);
int32_t cellGemGetAccelerometerPositionInDevice(uint32_t gem_num,
                                                vec_float4 *pos);
int32_t cellGemGetImageState(uint32_t gem_num,
                             CellGemImageState *gem_image_state);
int32_t cellGemGetCameraState(CellGemCameraState *gem_camera_state);

/* Sphere colour, rumble and the expansion port. */
int32_t cellGemForceRGB(uint32_t gem_num, float r, float g, float b);
int32_t cellGemSetRumble(uint32_t gem_num, unsigned char rumble);
int32_t cellGemGetRumble(uint32_t gem_num, unsigned char *rumble);
int32_t cellGemGetRGB(uint32_t gem_num, float *r, float *g, float *b);
int32_t cellGemReadExternalPortDeviceInfo(
    uint32_t gem_num, unsigned int *ext_id,
    unsigned char ext_info[CELL_GEM_EXTERNAL_PORT_DEVICE_INFO_SIZE]);
int32_t cellGemWriteExternalPort(
    uint32_t gem_num, unsigned char data[CELL_GEM_EXTERNAL_PORT_OUTPUT_SIZE]);

/* Hue selection and sphere colour tracking. */
int32_t cellGemGetTrackerHue(uint32_t gem_num, unsigned int *hue);
int32_t cellGemHSVtoRGB(float h, float s, float v,
                        float *r, float *g, float *b);
int32_t cellGemIsTrackableHue(unsigned int hue);
int32_t cellGemGetAllTrackableHues(unsigned char *hues);
int32_t cellGemGetHuePixels(const void *camera_frame, unsigned int hue,
                            unsigned char *pixels);
int32_t cellGemTrackHues(const unsigned int *req_hues, unsigned int *res_hues);
int32_t cellGemGetEnvironmentLightingColor(float *r, float *g, float *b);

/* Connection, calibration and filtering. */
int32_t cellGemGetInfo(CellGemInfo *info);
int32_t cellGemReset(uint32_t gem_num);
int32_t cellGemCalibrate(uint32_t gem_num);
int32_t cellGemInvalidateCalibration(int32_t gem_num);
int32_t cellGemFilterState(uint32_t gem_num, uint32_t flag);
int32_t cellGemEnableMagnetometer(uint32_t gem_num, int flag);
int32_t cellGemEnableCameraPitchAngleCorrection(int flag);
int32_t cellGemSetYaw(uint32_t gem_num, vec_float4 z_direction);

#ifdef __cplusplus
}
#endif

#endif /* __PS3DK_CELL_GEM_LIBGEM_H__ */
