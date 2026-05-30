/*
 * cell/sysutil_avc2_video.h - AVChat 2.0 window / screen / video API.
 *
 * Covers chat screen show/hide, per-member video windows (create/destroy,
 * position/size, show/hide, string overlay, attributes, z-order),
 * video muting, camera status, and video resolution change.
 */
#ifndef __PS3DK_CELL_SYSUTIL_AVC2_VIDEO_H__
#define __PS3DK_CELL_SYSUTIL_AVC2_VIDEO_H__

#include <cell/sysutil_avc2.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- Window attributes ------------------------------------------------ */

typedef enum {
    CELL_SYSUTIL_AVC2_WINDOW_ATTRIBUTE_ALPHA               = 0x00002001,
    CELL_SYSUTIL_AVC2_WINDOW_ATTRIBUTE_TRANSITION_TYPE     = 0x00002002,
    CELL_SYSUTIL_AVC2_WINDOW_ATTRIBUTE_TRANSITION_DURATION = 0x00002003,
    CELL_SYSUTIL_AVC2_WINDOW_ATTRIBUTE_STRING_VISIBLE      = 0x00002004,
    CELL_SYSUTIL_AVC2_WINDOW_ATTRIBUTE_ROTATION            = 0x00002005,
    CELL_SYSUTIL_AVC2_WINDOW_ATTRIBUTE_ZORDER              = 0x00002006,
    CELL_SYSUTIL_AVC2_WINDOW_ATTRIBUTE_SURFACE             = 0x00002007,
} CellSysutilAvc2WindowAttributeId;

/* ---- Transition / animation types ------------------------------------- */

typedef enum {
    CELL_SYSUTIL_AVC2_TRANSITION_NONE                      = 0xffffffff,
    CELL_SYSUTIL_AVC2_TRANSITION_LINEAR                    = 0x00000000,
    CELL_SYSUTIL_AVC2_TRANSITION_SLOWDOWN                  = 0x00000001,
    CELL_SYSUTIL_AVC2_TRANSITION_FASTUP                    = 0x00000002,
    CELL_SYSUTIL_AVC2_TRANSITION_ANGULAR                   = 0x00000003,
    CELL_SYSUTIL_AVC2_TRANSITION_EXPONENT                  = 0x00000004,
} CellSysutilAvc2TransitionType;

/* ---- Z-order mode ----------------------------------------------------- */

typedef enum
{
    CELL_SYSUTIL_AVC2_ZORDER_FORWARD_MOST                  = 0x00000001,
    CELL_SYSUTIL_AVC2_ZORDER_BEHIND_MOST                   = 0x00000002,
} CellSysutilAvc2WindowZorderMode;

/* ---- Constants -------------------------------------------------------- */

#define AVC2_SPECIAL_ROOM_MEMBER_ID_CUSTOM_VIDEO_WINDOW    0xfff0

/* ---- Structs ---------------------------------------------------------- */

/* Window attribute parameter (union of int, float, and EA vectors) */
typedef union CellSysutilAvc2WindowAttributeParam
{
    int    int_vector[4];
    float  float_vector[4];
    uint32_t ptr_vector[4];
} CellSysutilAvc2WindowAttributeParam;

/* Window attribute descriptor */
typedef struct CellSysutilAvc2WindowAttribute
{
    CellSysutilAvc2WindowAttributeId    attr_id;
    CellSysutilAvc2WindowAttributeParam attr_param;
} CellSysutilAvc2WindowAttribute;

/* ---- Screen control --------------------------------------------------- */

int cellSysutilAvc2ShowScreen(void);

int cellSysutilAvc2HideScreen(void);

int cellSysutilAvc2GetScreenShowStatus(uint8_t *visible);

/* ---- Window lifecycle ------------------------------------------------- */

int cellSysutilAvc2CreateWindow(const SceNpMatching2RoomMemberId member_id);

int cellSysutilAvc2DestroyWindow(const SceNpMatching2RoomMemberId member_id);

/* ---- Window string ---------------------------------------------------- */

int cellSysutilAvc2SetWindowString(const SceNpMatching2RoomMemberId member_id,
                                   const char *string);

int cellSysutilAvc2GetWindowString(const SceNpMatching2RoomMemberId member_id,
                                   char *string, uint8_t *len);

/* ---- Window position -------------------------------------------------- */

int cellSysutilAvc2SetWindowPosition(const SceNpMatching2RoomMemberId member_id,
                                     const float x,
                                     const float y,
                                     const float z);

int cellSysutilAvc2GetWindowPosition(const SceNpMatching2RoomMemberId member_id,
                                     float *x,
                                     float *y,
                                     float *z);

/* ---- Window size ------------------------------------------------------ */

int cellSysutilAvc2SetWindowSize(const SceNpMatching2RoomMemberId member_id,
                                 const float width,
                                 const float height);

int cellSysutilAvc2GetWindowSize(const SceNpMatching2RoomMemberId member_id,
                                 float *width,
                                 float *height);

/* ---- Window visibility ------------------------------------------------ */

int cellSysutilAvc2ShowWindow(const SceNpMatching2RoomMemberId member_id);

int cellSysutilAvc2HideWindow(const SceNpMatching2RoomMemberId member_id);

int cellSysutilAvc2GetWindowShowStatus(const SceNpMatching2RoomMemberId member_id,
                                       uint8_t *visible);

/* ---- Video muting ----------------------------------------------------- */

int cellSysutilAvc2SetVideoMuting(const uint8_t muting);

int cellSysutilAvc2GetVideoMuting(uint8_t *muting);

/* ---- Camera status ---------------------------------------------------- */

int cellSysutilAvc2IsCameraAttached(uint8_t *status);

/* ---- Video resolution ------------------------------------------------- */

int cellSysutilAvc2ChangeVideoResolution(
    const CellSysutilAvc2VideoResolution resolution);

/* ---- Window attributes ------------------------------------------------ */

int cellSysutilAvc2SetWindowAttribute(
    const SceNpMatching2RoomMemberId member_id,
    const CellSysutilAvc2WindowAttribute *attr);

int cellSysutilAvc2GetWindowAttribute(
    const SceNpMatching2RoomMemberId member_id,
    CellSysutilAvc2WindowAttribute *attr);

#ifdef __cplusplus
}
#endif

#endif /* __PS3DK_CELL_SYSUTIL_AVC2_VIDEO_H__ */
