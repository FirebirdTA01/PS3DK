/*
 * <cell/gem.h> surface test, driven by gem-header-surface-test.sh.
 *
 * Layout: the libgem structures are shared with the PRX, so every size,
 * alignment and member offset below must hold in BOTH data models (the
 * pointer members stay 4-byte PRX pointers under -mlp64).
 * Constants: every CELL_GEM_* macro and enumerator, by value.
 * Prototypes: each of the 37 PRX imports is assigned to a pointer of the
 * exact expected type, so a wrong parameter list fails to compile, and the
 * table keeps one reference per import for the link step.
 * Inline helpers: gem_check_helpers() compares each helper's result with
 * the expected value.  At -O2 every comparison folds away; a mismatch
 * leaves a call to the undefined gem_helper_mismatch(), which the link
 * step reports.
 */
#include <cell/gem.h>
#include <stddef.h>

#ifdef __cplusplus
#define GEM_ASSERT(e) static_assert(e, #e)
#else
#define GEM_ASSERT(e) _Static_assert(e, #e)
#endif

GEM_ASSERT(sizeof(CellGemInfo) == 40);
GEM_ASSERT(__alignof__(CellGemInfo) == 4);
GEM_ASSERT(offsetof(CellGemInfo, max_connect) == 0);
GEM_ASSERT(offsetof(CellGemInfo, now_connect) == 4);
GEM_ASSERT(offsetof(CellGemInfo, status) == 8);
GEM_ASSERT(offsetof(CellGemInfo, port) == 24);
GEM_ASSERT(sizeof(CellGemPadData) == 4);
GEM_ASSERT(__alignof__(CellGemPadData) == 2);
GEM_ASSERT(offsetof(CellGemPadData, digitalbuttons) == 0);
GEM_ASSERT(offsetof(CellGemPadData, analog_T) == 2);
GEM_ASSERT(sizeof(CellGemExtPortData) == 20);
GEM_ASSERT(__alignof__(CellGemExtPortData) == 2);
GEM_ASSERT(offsetof(CellGemExtPortData, status) == 0);
GEM_ASSERT(offsetof(CellGemExtPortData, digital1) == 2);
GEM_ASSERT(offsetof(CellGemExtPortData, digital2) == 4);
GEM_ASSERT(offsetof(CellGemExtPortData, analog_right_x) == 6);
GEM_ASSERT(offsetof(CellGemExtPortData, analog_right_y) == 8);
GEM_ASSERT(offsetof(CellGemExtPortData, analog_left_x) == 10);
GEM_ASSERT(offsetof(CellGemExtPortData, analog_left_y) == 12);
GEM_ASSERT(offsetof(CellGemExtPortData, custom) == 14);
GEM_ASSERT(sizeof(CellGemState) == 192);
GEM_ASSERT(__alignof__(CellGemState) == 16);
GEM_ASSERT(offsetof(CellGemState, pos) == 0);
GEM_ASSERT(offsetof(CellGemState, vel) == 16);
GEM_ASSERT(offsetof(CellGemState, accel) == 32);
GEM_ASSERT(offsetof(CellGemState, quat) == 48);
GEM_ASSERT(offsetof(CellGemState, angvel) == 64);
GEM_ASSERT(offsetof(CellGemState, angaccel) == 80);
GEM_ASSERT(offsetof(CellGemState, handle_pos) == 96);
GEM_ASSERT(offsetof(CellGemState, handle_vel) == 112);
GEM_ASSERT(offsetof(CellGemState, handle_accel) == 128);
GEM_ASSERT(offsetof(CellGemState, pad) == 144);
GEM_ASSERT(offsetof(CellGemState, ext) == 148);
GEM_ASSERT(offsetof(CellGemState, timestamp) == 168);
GEM_ASSERT(offsetof(CellGemState, temperature) == 176);
GEM_ASSERT(offsetof(CellGemState, camera_pitch_angle) == 180);
GEM_ASSERT(offsetof(CellGemState, tracking_flags) == 184);
GEM_ASSERT(sizeof(CellGemInertialState) == 112);
GEM_ASSERT(__alignof__(CellGemInertialState) == 16);
GEM_ASSERT(offsetof(CellGemInertialState, accelerometer) == 0);
GEM_ASSERT(offsetof(CellGemInertialState, gyro) == 16);
GEM_ASSERT(offsetof(CellGemInertialState, accelerometer_bias) == 32);
GEM_ASSERT(offsetof(CellGemInertialState, gyro_bias) == 48);
GEM_ASSERT(offsetof(CellGemInertialState, pad) == 64);
GEM_ASSERT(offsetof(CellGemInertialState, ext) == 68);
GEM_ASSERT(offsetof(CellGemInertialState, timestamp) == 88);
GEM_ASSERT(offsetof(CellGemInertialState, counter) == 96);
GEM_ASSERT(offsetof(CellGemInertialState, temperature) == 100);
GEM_ASSERT(sizeof(CellGemImageState) == 48);
GEM_ASSERT(__alignof__(CellGemImageState) == 8);
GEM_ASSERT(offsetof(CellGemImageState, frame_timestamp) == 0);
GEM_ASSERT(offsetof(CellGemImageState, timestamp) == 8);
GEM_ASSERT(offsetof(CellGemImageState, u) == 16);
GEM_ASSERT(offsetof(CellGemImageState, v) == 20);
GEM_ASSERT(offsetof(CellGemImageState, r) == 24);
GEM_ASSERT(offsetof(CellGemImageState, projectionx) == 28);
GEM_ASSERT(offsetof(CellGemImageState, projectiony) == 32);
GEM_ASSERT(offsetof(CellGemImageState, distance) == 36);
GEM_ASSERT(offsetof(CellGemImageState, visible) == 40);
GEM_ASSERT(offsetof(CellGemImageState, r_valid) == 41);
GEM_ASSERT(sizeof(CellGemCameraState) == 20);
GEM_ASSERT(__alignof__(CellGemCameraState) == 4);
GEM_ASSERT(offsetof(CellGemCameraState, exposure) == 0);
GEM_ASSERT(offsetof(CellGemCameraState, exposure_time) == 4);
GEM_ASSERT(offsetof(CellGemCameraState, gain) == 8);
GEM_ASSERT(offsetof(CellGemCameraState, pitch_angle) == 12);
GEM_ASSERT(offsetof(CellGemCameraState, pitch_angle_estimate) == 16);
GEM_ASSERT(sizeof(CellGemAttribute) == 24);
GEM_ASSERT(__alignof__(CellGemAttribute) == 4);
GEM_ASSERT(offsetof(CellGemAttribute, version) == 0);
GEM_ASSERT(offsetof(CellGemAttribute, max_connect) == 4);
GEM_ASSERT(offsetof(CellGemAttribute, memory_ptr) == 8);
GEM_ASSERT(offsetof(CellGemAttribute, spurs) == 12);
GEM_ASSERT(offsetof(CellGemAttribute, spu_priorities) == 16);
GEM_ASSERT(sizeof(CellGemVideoConvertAttribute) == 40);
GEM_ASSERT(__alignof__(CellGemVideoConvertAttribute) == 4);
GEM_ASSERT(offsetof(CellGemVideoConvertAttribute, version) == 0);
GEM_ASSERT(offsetof(CellGemVideoConvertAttribute, output_format) == 4);
GEM_ASSERT(offsetof(CellGemVideoConvertAttribute, conversion_flags) == 8);
GEM_ASSERT(offsetof(CellGemVideoConvertAttribute, gain) == 12);
GEM_ASSERT(offsetof(CellGemVideoConvertAttribute, red_gain) == 16);
GEM_ASSERT(offsetof(CellGemVideoConvertAttribute, green_gain) == 20);
GEM_ASSERT(offsetof(CellGemVideoConvertAttribute, blue_gain) == 24);
GEM_ASSERT(offsetof(CellGemVideoConvertAttribute, buffer_memory) == 28);
GEM_ASSERT(offsetof(CellGemVideoConvertAttribute, video_data_out) == 32);
GEM_ASSERT(offsetof(CellGemVideoConvertAttribute, alpha) == 36);
GEM_ASSERT(sizeof(CellGemVideoConvertFormatEnum) == 4);
GEM_ASSERT(sizeof(CellGemVideoConvertFlagEnum) == 4);
GEM_ASSERT(sizeof(((CellGemInfo *)0)->status) == 16);
GEM_ASSERT(sizeof(((CellGemExtPortData *)0)->custom) == 5);
GEM_ASSERT(sizeof(((CellGemAttribute *)0)->spu_priorities) == 8);

GEM_ASSERT(CELL_GEM_VERSION == 2);
GEM_ASSERT(CELL_GEM_MAX_NUM == 4);
GEM_ASSERT(CELL_GEM_LATENCY_OFFSET == -22000);
GEM_ASSERT(CELL_GEM_STATUS_DISCONNECTED == 0);
GEM_ASSERT(CELL_GEM_STATUS_READY == 1);
GEM_ASSERT(CELL_GEM_CTRL_SELECT == 0x01);
GEM_ASSERT(CELL_GEM_CTRL_T == 0x02);
GEM_ASSERT(CELL_GEM_CTRL_MOVE == 0x04);
GEM_ASSERT(CELL_GEM_CTRL_START == 0x08);
GEM_ASSERT(CELL_GEM_CTRL_TRIANGLE == 0x10);
GEM_ASSERT(CELL_GEM_CTRL_CIRCLE == 0x20);
GEM_ASSERT(CELL_GEM_CTRL_CROSS == 0x40);
GEM_ASSERT(CELL_GEM_CTRL_SQUARE == 0x80);
GEM_ASSERT(CELL_GEM_EXT_CONNECTED == 1);
GEM_ASSERT(CELL_GEM_EXT_EXT0 == 2);
GEM_ASSERT(CELL_GEM_EXT_EXT1 == 4);
GEM_ASSERT(CELL_GEM_EXTERNAL_PORT_DEVICE_INFO_SIZE == 38);
GEM_ASSERT(CELL_GEM_EXTERNAL_PORT_OUTPUT_SIZE == 40);
GEM_ASSERT(CELL_GEM_MIN_CAMERA_EXPOSURE == 40);
GEM_ASSERT(CELL_GEM_MAX_CAMERA_EXPOSURE == 511);
GEM_ASSERT(CELL_GEM_STATE_FLAG_CURRENT_TIME == 0);
GEM_ASSERT(CELL_GEM_STATE_FLAG_LATEST_IMAGE_TIME == 1);
GEM_ASSERT(CELL_GEM_STATE_FLAG_TIMESTAMP == 2);
GEM_ASSERT(CELL_GEM_INERTIAL_STATE_FLAG_LATEST == 0);
GEM_ASSERT(CELL_GEM_INERTIAL_STATE_FLAG_PREVIOUS == 1);
GEM_ASSERT(CELL_GEM_INERTIAL_STATE_FLAG_NEXT == 2);
GEM_ASSERT(CELL_GEM_NOT_CONNECTED == 1);
GEM_ASSERT(CELL_GEM_SPHERE_NOT_CALIBRATED == 2);
GEM_ASSERT(CELL_GEM_SPHERE_CALIBRATING == 3);
GEM_ASSERT(CELL_GEM_COMPUTING_AVAILABLE_COLORS == 4);
GEM_ASSERT(CELL_GEM_HUE_NOT_SET == 5);
GEM_ASSERT(CELL_GEM_NO_VIDEO == 6);
GEM_ASSERT(CELL_GEM_TIME_OUT_OF_RANGE == 7);
GEM_ASSERT(CELL_GEM_NOT_CALIBRATED == 8);
GEM_ASSERT(CELL_GEM_NO_EXTERNAL_PORT_DEVICE == 9);
GEM_ASSERT(CELL_GEM_DONT_TRACK_HUE == 0x02000000);
GEM_ASSERT(CELL_GEM_DONT_CARE_HUE == 0x04000000);
GEM_ASSERT(CELL_GEM_DONT_CHANGE_HUE == 0x08000000);
GEM_ASSERT(CELL_GEM_FLAG_CALIBRATION_OCCURRED == 0x1);
GEM_ASSERT(CELL_GEM_FLAG_CALIBRATION_SUCCEEDED == 0x2);
GEM_ASSERT(CELL_GEM_FLAG_CALIBRATION_FAILED_CANT_FIND_SPHERE == 0x4);
GEM_ASSERT(CELL_GEM_FLAG_CALIBRATION_FAILED_MOTION_DETECTED == 0x8);
GEM_ASSERT(CELL_GEM_FLAG_CALIBRATION_FAILED_BRIGHT_LIGHTING == 0x10);
GEM_ASSERT(CELL_GEM_FLAG_CALIBRATION_WARNING_MOTION_DETECTED == 0x20);
GEM_ASSERT(CELL_GEM_FLAG_CALIBRATION_WARNING_BRIGHT_LIGHTING == 0x40);
GEM_ASSERT(CELL_GEM_FLAG_LIGHTING_CHANGED == 0x80);
GEM_ASSERT(CELL_GEM_FLAG_WRONG_FIELD_OF_VIEW_SETTING == 0x100);
GEM_ASSERT(CELL_GEM_FLAG_CAMERA_PITCH_ANGLE_CHANGED == 0x200);
GEM_ASSERT(CELL_GEM_FLAG_VARIABLE_MAGNETIC_FIELD == 0x400);
GEM_ASSERT(CELL_GEM_FLAG_WEAK_MAGNETIC_FIELD == 0x800);
GEM_ASSERT(CELL_GEM_FLAG_VERY_COLORFUL_ENVIRONMENT == 0x1000);
GEM_ASSERT(CELL_GEM_FLAG_CURRENT_HUE_CONFLICTS_WITH_ENVIRONMENT == 0x2000);
GEM_ASSERT(CELL_GEM_ALL_FLAGS == 0xffffffffffffffffull);
GEM_ASSERT(CELL_GEM_TRACKING_FLAG_POSITION_TRACKED == 1);
GEM_ASSERT(CELL_GEM_TRACKING_FLAG_VISIBLE == 2);
GEM_ASSERT(CELL_GEM_ERROR_RESOURCE_ALLOCATION_FAILED == (int)0x80121801);
GEM_ASSERT(CELL_GEM_ERROR_ALREADY_INITIALIZED == (int)0x80121802);
GEM_ASSERT(CELL_GEM_ERROR_UNINITIALIZED == (int)0x80121803);
GEM_ASSERT(CELL_GEM_ERROR_INVALID_PARAMETER == (int)0x80121804);
GEM_ASSERT(CELL_GEM_ERROR_INVALID_ALIGNMENT == (int)0x80121805);
GEM_ASSERT(CELL_GEM_ERROR_UPDATE_NOT_FINISHED == (int)0x80121806);
GEM_ASSERT(CELL_GEM_ERROR_UPDATE_NOT_STARTED == (int)0x80121807);
GEM_ASSERT(CELL_GEM_ERROR_CONVERT_NOT_FINISHED == (int)0x80121808);
GEM_ASSERT(CELL_GEM_ERROR_CONVERT_NOT_STARTED == (int)0x80121809);
GEM_ASSERT(CELL_GEM_ERROR_WRITE_NOT_FINISHED == (int)0x8012180A);
GEM_ASSERT(CELL_GEM_ERROR_NOT_A_HUE == (int)0x8012180B);
GEM_ASSERT(CELL_GEM_NO_VIDEO_OUTPUT == 1);
GEM_ASSERT(CELL_GEM_RGBA_640x480 == 2);
GEM_ASSERT(CELL_GEM_YUV_640x480 == 3);
GEM_ASSERT(CELL_GEM_YUV422_640x480 == 4);
GEM_ASSERT(CELL_GEM_YUV411_640x480 == 5);
GEM_ASSERT(CELL_GEM_RGBA_320x240 == 6);
GEM_ASSERT(CELL_GEM_BAYER_RESTORED == 7);
GEM_ASSERT(CELL_GEM_BAYER_RESTORED_RGGB == 8);
GEM_ASSERT(CELL_GEM_BAYER_RESTORED_RASTERIZED == 9);
GEM_ASSERT(CELL_GEM_AUTO_WHITE_BALANCE == 1);
GEM_ASSERT(CELL_GEM_GAMMA_BOOST == 2);
GEM_ASSERT(CELL_GEM_COMBINE_PREVIOUS_INPUT_FRAME == 4);
GEM_ASSERT(CELL_GEM_FILTER_OUTLIER_PIXELS == 8);
GEM_ASSERT(sizeof(CELL_GEM_ALL_FLAGS) == 8);
GEM_ASSERT(CELL_GEM_SPHERE_RADIUS_MM == 22.5f);

/* One typed pointer per import; volatile so -O2 keeps every reference. */
int32_t (*const volatile gem_ref_GetMemorySize)(int) = cellGemGetMemorySize;
int32_t (*const volatile gem_ref_Init)(const CellGemAttribute *) = cellGemInit;
int32_t (*const volatile gem_ref_End)(void) = cellGemEnd;
int32_t (*const volatile gem_ref_PrepareCamera)(int, float) = cellGemPrepareCamera;
int32_t (*const volatile gem_ref_PrepareVideoConvert)(const CellGemVideoConvertAttribute *) = cellGemPrepareVideoConvert;
int32_t (*const volatile gem_ref_ConvertVideoStart)(const void *) = cellGemConvertVideoStart;
int32_t (*const volatile gem_ref_ConvertVideoFinish)(void) = cellGemConvertVideoFinish;
int32_t (*const volatile gem_ref_UpdateStart)(const void *, system_time_t) = cellGemUpdateStart;
int32_t (*const volatile gem_ref_UpdateFinish)(void) = cellGemUpdateFinish;
int32_t (*const volatile gem_ref_GetStatusFlags)(int, uint64_t *) = cellGemGetStatusFlags;
int32_t (*const volatile gem_ref_ClearStatusFlags)(int, uint64_t) = cellGemClearStatusFlags;
int32_t (*const volatile gem_ref_GetState)(uint32_t, uint32_t, system_time_t, CellGemState *) = cellGemGetState;
int32_t (*const volatile gem_ref_GetInertialState)(uint32_t, uint32_t, system_time_t, CellGemInertialState *) = cellGemGetInertialState;
int32_t (*const volatile gem_ref_GetAccelerometerPositionInDevice)(uint32_t, vec_float4 *) = cellGemGetAccelerometerPositionInDevice;
int32_t (*const volatile gem_ref_GetImageState)(uint32_t, CellGemImageState *) = cellGemGetImageState;
int32_t (*const volatile gem_ref_GetCameraState)(CellGemCameraState *) = cellGemGetCameraState;
int32_t (*const volatile gem_ref_ForceRGB)(uint32_t, float, float, float) = cellGemForceRGB;
int32_t (*const volatile gem_ref_SetRumble)(uint32_t, unsigned char) = cellGemSetRumble;
int32_t (*const volatile gem_ref_ReadExternalPortDeviceInfo)(uint32_t, unsigned int *, unsigned char *) = cellGemReadExternalPortDeviceInfo;
int32_t (*const volatile gem_ref_WriteExternalPort)(uint32_t, unsigned char *) = cellGemWriteExternalPort;
int32_t (*const volatile gem_ref_GetRumble)(uint32_t, unsigned char *) = cellGemGetRumble;
int32_t (*const volatile gem_ref_GetRGB)(uint32_t, float *, float *, float *) = cellGemGetRGB;
int32_t (*const volatile gem_ref_GetTrackerHue)(uint32_t, unsigned int *) = cellGemGetTrackerHue;
int32_t (*const volatile gem_ref_HSVtoRGB)(float, float, float, float *, float *, float *) = cellGemHSVtoRGB;
int32_t (*const volatile gem_ref_IsTrackableHue)(unsigned int) = cellGemIsTrackableHue;
int32_t (*const volatile gem_ref_GetAllTrackableHues)(unsigned char *) = cellGemGetAllTrackableHues;
int32_t (*const volatile gem_ref_GetHuePixels)(const void *, unsigned int, unsigned char *) = cellGemGetHuePixels;
int32_t (*const volatile gem_ref_TrackHues)(const unsigned int *, unsigned int *) = cellGemTrackHues;
int32_t (*const volatile gem_ref_GetEnvironmentLightingColor)(float *, float *, float *) = cellGemGetEnvironmentLightingColor;
int32_t (*const volatile gem_ref_GetInfo)(CellGemInfo *) = cellGemGetInfo;
int32_t (*const volatile gem_ref_Reset)(uint32_t) = cellGemReset;
int32_t (*const volatile gem_ref_Calibrate)(uint32_t) = cellGemCalibrate;
int32_t (*const volatile gem_ref_InvalidateCalibration)(int32_t) = cellGemInvalidateCalibration;
int32_t (*const volatile gem_ref_FilterState)(uint32_t, uint32_t) = cellGemFilterState;
int32_t (*const volatile gem_ref_EnableMagnetometer)(uint32_t, int) = cellGemEnableMagnetometer;
int32_t (*const volatile gem_ref_EnableCameraPitchAngleCorrection)(int) = cellGemEnableCameraPitchAngleCorrection;
int32_t (*const volatile gem_ref_SetYaw)(uint32_t, vec_float4) = cellGemSetYaw;

#ifdef __cplusplus
extern "C"
#endif
void gem_helper_mismatch(void);

#define GEM_EXPECT(e) do { if (!(e)) gem_helper_mismatch(); } while (0)

void gem_check_helpers(void);
void gem_check_helpers(void)
{
    static CellSpurs spurs;
    static char work[64], out[16], prev[16];
    static const uint8_t prio[8] = { 1, 2, 3, 4, 5, 6, 7, 8 };
    CellGemAttribute a;
    CellGemVideoConvertAttribute v;

    cellGemAttributeInit(&a, 3, work, &spurs, prio);
    GEM_EXPECT(a.version == CELL_GEM_VERSION);
    GEM_EXPECT(a.max_connect == 3);
    GEM_EXPECT(a.memory_ptr == (void *)work);
    GEM_EXPECT(a.spurs == &spurs);
    GEM_EXPECT(a.spu_priorities[0] == 1 && a.spu_priorities[1] == 2);
    GEM_EXPECT(a.spu_priorities[2] == 3 && a.spu_priorities[3] == 4);
    GEM_EXPECT(a.spu_priorities[4] == 5 && a.spu_priorities[5] == 6);
    GEM_EXPECT(a.spu_priorities[6] == 7 && a.spu_priorities[7] == 8);

    GEM_EXPECT(cellGemGetVideoConvertSize(CELL_GEM_NO_VIDEO_OUTPUT) == 0);
    GEM_EXPECT(cellGemGetVideoConvertSize(CELL_GEM_RGBA_640x480) == 1228800);
    GEM_EXPECT(cellGemGetVideoConvertSize(CELL_GEM_YUV_640x480) == 921600);
    GEM_EXPECT(cellGemGetVideoConvertSize(CELL_GEM_YUV422_640x480) == 614400);
    GEM_EXPECT(cellGemGetVideoConvertSize(CELL_GEM_YUV411_640x480) == 460800);
    GEM_EXPECT(cellGemGetVideoConvertSize(CELL_GEM_RGBA_320x240) == 307200);
    GEM_EXPECT(cellGemGetVideoConvertSize(CELL_GEM_BAYER_RESTORED) == 307200);
    GEM_EXPECT(cellGemGetVideoConvertSize(CELL_GEM_BAYER_RESTORED_RGGB) == 307200);
    GEM_EXPECT(cellGemGetVideoConvertSize(CELL_GEM_BAYER_RESTORED_RASTERIZED) == 307200);
    GEM_EXPECT(cellGemGetVideoConvertSize(0) == -1);
    GEM_EXPECT(cellGemGetVideoConvertSize(10) == -1);

    cellGemVideoConvertAttributeInit(&v, CELL_GEM_YUV422_640x480,
                                     CELL_GEM_GAMMA_BOOST, 2.0f, 3.0f, 4.0f,
                                     5.0f, prev, out, 7);
    GEM_EXPECT(v.version == CELL_GEM_VERSION);
    GEM_EXPECT(v.output_format == CELL_GEM_YUV422_640x480);
    GEM_EXPECT(v.conversion_flags == CELL_GEM_GAMMA_BOOST);
    GEM_EXPECT(v.gain == 2.0f && v.red_gain == 3.0f);
    GEM_EXPECT(v.green_gain == 4.0f && v.blue_gain == 5.0f);
    GEM_EXPECT(v.buffer_memory == (void *)prev);
    GEM_EXPECT(v.video_data_out == (void *)out);
    GEM_EXPECT(v.alpha == 7);

    cellGemVideoConvertAttributeInitRgba(&v, out, 0);
    GEM_EXPECT(v.output_format == CELL_GEM_RGBA_640x480);
    GEM_EXPECT(v.conversion_flags == (CELL_GEM_AUTO_WHITE_BALANCE |
                                      CELL_GEM_GAMMA_BOOST |
                                      CELL_GEM_FILTER_OUTLIER_PIXELS));
    GEM_EXPECT(v.gain == 1.0f && v.red_gain == 1.0f);
    GEM_EXPECT(v.green_gain == 1.0f && v.blue_gain == 1.0f);
    GEM_EXPECT(v.buffer_memory == 0 && v.video_data_out == (void *)out);
    GEM_EXPECT(v.alpha == 255);

    cellGemVideoConvertAttributeInitRgba(&v, out, prev);
    GEM_EXPECT(v.conversion_flags == (CELL_GEM_AUTO_WHITE_BALANCE |
                                      CELL_GEM_GAMMA_BOOST |
                                      CELL_GEM_FILTER_OUTLIER_PIXELS |
                                      CELL_GEM_COMBINE_PREVIOUS_INPUT_FRAME));
    GEM_EXPECT(v.buffer_memory == (void *)prev);
}

int main(void)
{
    gem_check_helpers();
    return gem_ref_Init == 0;
}
