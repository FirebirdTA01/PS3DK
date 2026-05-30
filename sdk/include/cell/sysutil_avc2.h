/*
 * cell/sysutil_avc2.h - AVChat 2.0 chat / voice / video conference API.
 *
 * Provides NP-room-backed voice/video chat sessions.  Covers lifecycle
 * (load/unload), chat session join/leave, player enumeration, voice
 * muting/volume, streaming start/stop, microphone/camera status,
 * and internal attribute get/set.
 *
 * Depends on <cell/np.h> (SceNpMatching2* types) and
 * <sysutil/sysutil_common.h> (sysutil error bases).
 */
#ifndef __PS3DK_CELL_SYSUTIL_AVC2_H__
#define __PS3DK_CELL_SYSUTIL_AVC2_H__

#include <cell/np.h>
#include <sysutil/sysutil_common.h>
#include <ppu-types.h>
#include <stdint.h>
#include <sys/memory.h>
#include <netinet/in.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- Error codes ------------------------------------------------------ */
#define CELL_AVC2_ERROR_UNKNOWN                 (0x8002b701)
#define CELL_AVC2_ERROR_NOT_SUPPORTED           (0x8002b702)
#define CELL_AVC2_ERROR_NOT_INITIALIZED         (0x8002b703)
#define CELL_AVC2_ERROR_ALREADY_INITIALIZED     (0x8002b704)
#define CELL_AVC2_ERROR_INVALID_ARGUMENT        (0x8002b705)
#define CELL_AVC2_ERROR_OUT_OF_MEMORY           (0x8002b706)
#define CELL_AVC2_ERROR_BAD_ID                  (0x8002b707)
#define CELL_AVC2_ERROR_INVALID_STATUS          (0x8002b70a)
#define CELL_AVC2_ERROR_TIMEOUT                 (0x8002b70b)
#define CELL_AVC2_ERROR_NO_SESSION              (0x8002b70d)
#define CELL_AVC2_ERROR_INCOMPATIBLE_PROTOCOL   (0x8002b70e)
#define CELL_AVC2_ERROR_WINDOW_ALREADY_EXISTS   (0x8002b70f)
#define CELL_AVC2_ERROR_TOO_MANY_WINDOWS        (0x8002b710)
#define CELL_AVC2_ERROR_TOO_MANY_PEER_WINDOWS   (0x8002b711)
#define CELL_AVC2_ERROR_WINDOW_NOT_FOUND        (0x8002b712)

/* ---- Events ----------------------------------------------------------- */

/* Asynchronous operation results */
#define CELL_AVC2_EVENT_LOAD_SUCCEEDED                      (0x00000001)
#define CELL_AVC2_EVENT_LOAD_FAILED                         (0x00000002)
#define CELL_AVC2_EVENT_UNLOAD_SUCCEEDED                    (0x00000003)
#define CELL_AVC2_EVENT_UNLOAD_FAILED                       (0x00000004)
#define CELL_AVC2_EVENT_JOIN_SUCCEEDED                      (0x00000005)
#define CELL_AVC2_EVENT_JOIN_FAILED                         (0x00000006)
#define CELL_AVC2_EVENT_LEAVE_SUCCEEDED                     (0x00000007)
#define CELL_AVC2_EVENT_LEAVE_FAILED                        (0x00000008)

/* Chat session state changes */
#define CELL_AVC2_EVENT_SYSTEM_NEW_MEMBER_JOINED            (0x10000001)
#define CELL_AVC2_EVENT_SYSTEM_MEMBER_LEFT                  (0x10000002)
#define CELL_AVC2_EVENT_SYSTEM_SESSION_ESTABLISHED          (0x10000003)
#define CELL_AVC2_EVENT_SYSTEM_SESSION_CANNOT_ESTABLISHED   (0x10000004)
#define CELL_AVC2_EVENT_SYSTEM_SESSION_DISCONNECTED         (0x10000005)
#define CELL_AVC2_EVENT_SYSTEM_VOICE_DETECTED               (0x10000006)
#define CELL_AVC2_EVENT_SYSTEM_MIC_DETECTED                 (0x10000007)
#define CELL_AVC2_EVENT_SYSTEM_CAMERA_DETECTED              (0x10000008)

/* Event parameter error codes */
#define CELL_AVC2_EVENT_PARAM_ERROR_UNKNOWN                 (0x0000000000000001ULL)
#define CELL_AVC2_EVENT_PARAM_ERROR_NOT_SUPPORTED           (0x0000000000000002ULL)
#define CELL_AVC2_EVENT_PARAM_ERROR_INVALID_ARGUMENT        (0x0000000000000003ULL)
#define CELL_AVC2_EVENT_PARAM_ERROR_OUT_OF_MEMORY           (0x0000000000000004ULL)
#define CELL_AVC2_EVENT_PARAM_ERROR_INVALID_STATUS          (0x0000000000000005ULL)
#define CELL_AVC2_EVENT_PARAM_ERROR_CONTEXT_DOES_NOT_EXIST  (0x0000000000000006ULL)
#define CELL_AVC2_EVENT_PARAM_ERROR_ROOM_DOES_NOT_EXIST     (0x0000000000000007ULL)
#define CELL_AVC2_EVENT_PARAM_ERROR_NETWORK_ERROR           (0x0000000000000008ULL)

/* ---- Request IDs ------------------------------------------------------ */
#define CELL_AVC2_REQUEST_ID_SYSTEM_EVENT                   (0x00000000)

/* ---- Player mic status ------------------------------------------------ */
#define CELL_AVC2_MIC_STATUS_DETACHED                       (0)
#define CELL_AVC2_MIC_STATUS_ATTACHED_OFF                   (1)
#define CELL_AVC2_MIC_STATUS_ATTACHED_ON                    (2)
#define CELL_AVC2_MIC_STATUS_UNKNOWN                        (3)

/* ---- Player camera status --------------------------------------------- */
#define CELL_AVC2_CAMERA_STATUS_DETACHED                    (0)
#define CELL_AVC2_CAMERA_STATUS_ATTACHED_OFF                (1)
#define CELL_AVC2_CAMERA_STATUS_ATTACHED_ON                 (2)
#define CELL_AVC2_CAMERA_STATUS_UNKNOWN                     (3)

/* ---- Streaming mode --------------------------------------------------- */
#define CELL_SYSUTIL_AVC2_STREAMING_MODE_NORMAL             (0)
#define CELL_SYSUTIL_AVC2_STREAMING_MODE_DIRECT_WAN         (1)
#define CELL_SYSUTIL_AVC2_STREAMING_MODE_DIRECT_LAN         (2)

/* ---- Video stream sharing mode ---------------------------------------- */
#define CELL_SYSUTIL_AVC2_VIDEO_SHARING_MODE_DISABLE        (0)
#define CELL_SYSUTIL_AVC2_VIDEO_SHARING_MODE_1              (1)
#define CELL_SYSUTIL_AVC2_VIDEO_SHARING_MODE_2              (2)
#define CELL_SYSUTIL_AVC2_VIDEO_SHARING_MODE_3              (3)

/* ---- Enums ------------------------------------------------------------ */

/* Media types offered by the chat service */
typedef enum {
    CELL_SYSUTIL_AVC2_VOICE_CHAT                            = 0x00000001,
    CELL_SYSUTIL_AVC2_VIDEO_CHAT                            = 0x00000010,
} CellSysutilAvc2MediaType;

/* Selectable voice quality */
typedef enum {
    CELL_SYSUTIL_AVC2_VOICE_QUALITY_NORMAL                  = 0x00000001,
} CellSysutilAvc2VoiceQuality;

/* Selectable video quality */
typedef enum {
    CELL_SYSUTIL_AVC2_VIDEO_QUALITY_NORMAL                  = 0x00000001,
} CellSysutilAvc2VideoQuality;

/* Voice transmission target */
typedef enum {
    CELL_SYSUTIL_AVC2_CHAT_TARGET_MODE_ROOM                 = 0x00000100,
    CELL_SYSUTIL_AVC2_CHAT_TARGET_MODE_TEAM                 = 0x00000200,
    CELL_SYSUTIL_AVC2_CHAT_TARGET_MODE_PRIVATE              = 0x00000300,
    CELL_SYSUTIL_AVC2_CHAT_TARGET_MODE_DIRECT               = 0x00001000,
} CellSysutilAvc2ChatTargetMode;

/* Video resolution */
typedef enum {
    CELL_SYSUTIL_AVC2_VIDEO_RESOLUTION_QQVGA                = 0x00000001,
    CELL_SYSUTIL_AVC2_VIDEO_RESOLUTION_QVGA                 = 0x00000002,
} CellSysutilAvc2VideoResolution;

/* Video frame transmission mode */
typedef enum {
    CELL_SYSUTIL_AVC2_FRAME_MODE_NORMAL                     = 0x00000001,
    CELL_SYSUTIL_AVC2_FRAME_MODE_INTRA_ONLY                 = 0x00000002,
} CellSysutilAvc2FrameMode;

/* Coordinate system */
typedef enum {
    CELL_SYSUTIL_AVC2_VIRTUAL_COORDINATES                   = 0x00000001,
    CELL_SYSUTIL_AVC2_ABSOLUTE_COORDINATES                  = 0x00000002,
} CellSysutilAvc2CoordinatesForm;

/* Service attribute IDs */
typedef enum {
    CELL_SYSUTIL_AVC2_ATTRIBUTE_VOICE_DETECT_EVENT_TYPE     = 0x00001001,
    CELL_SYSUTIL_AVC2_ATTRIBUTE_VOICE_DETECT_INTERVAL_TIME  = 0x00001002,
    CELL_SYSUTIL_AVC2_ATTRIBUTE_VOICE_DETECT_SIGNAL_LEVEL   = 0x00001003,
    CELL_SYSUTIL_AVC2_ATTRIBUTE_VOICE_MAX_BITRATE           = 0x00001004,
    CELL_SYSUTIL_AVC2_ATTRIBUTE_VOICE_DATA_FEC              = 0x00001005,
    CELL_SYSUTIL_AVC2_ATTRIBUTE_VOICE_PACKET_CONTENTION     = 0x00001006,
    CELL_SYSUTIL_AVC2_ATTRIBUTE_VOICE_DTX_MODE              = 0x00001007,
    CELL_SYSUTIL_AVC2_ATTRIBUTE_MIC_STATUS_DETECTION        = 0x00001008,
    CELL_SYSUTIL_AVC2_ATTRIBUTE_MIC_SETTING_NOTIFICATION    = 0x00001009,
    CELL_SYSUTIL_AVC2_ATTRIBUTE_VOICE_MUTING_NOTIFICATION   = 0x0000100A,
    CELL_SYSUTIL_AVC2_ATTRIBUTE_CAMERA_STATUS_DETECTION     = 0x0000100B,
} CellSysutilAvc2AttributeId;

/* ---- Init param version ----------------------------------------------- */
#define CELL_SYSUTIL_AVC2_INIT_PARAM_VERSION                (140)

/* ---- Structs ---------------------------------------------------------- */

/* Voice configuration parameters */
typedef struct CellSysutilAvc2VoiceInitParam
{
    CellSysutilAvc2VoiceQuality voice_quality;
    uint16_t                    max_speakers;
    uint8_t                     mic_out_stream_sharing;
    uint8_t                     reserved[25];
} CellSysutilAvc2VoiceInitParam;

/* Video configuration parameters */
typedef struct CellSysutilAvc2VideoInitParam
{
    CellSysutilAvc2VideoQuality     video_quality;
    CellSysutilAvc2FrameMode        frame_mode;
    CellSysutilAvc2VideoResolution  max_video_resolution;
    uint16_t                        max_video_windows;
    uint16_t                        max_video_framerate;
    uint32_t                        max_video_bitrate;
    CellSysutilAvc2CoordinatesForm  coordinates_form;
    uint8_t                         video_stream_sharing;
    uint8_t                         no_use_camera_device;
    uint8_t                         reserved[6];
} CellSysutilAvc2VideoInitParam;

/* LAN chat / streaming mode parameters */
typedef struct CellSysutilAvc2StreamingModeParam
{
    uint16_t    mode;
    in_port_t   port;
    uint8_t     reserved[10];
} CellSysutilAvc2StreamingModeParam;

/* Load option parameters */
typedef struct CellSysutilAvc2InitParam
{
    uint16_t                              avc_init_param_version;
    uint16_t                              max_players;
    uint16_t                              spu_load_average;
    union
    {
        uint16_t                             direct_streaming_mode;
        CellSysutilAvc2StreamingModeParam    streaming_mode;
    };
    uint8_t                               reserved[18];
    CellSysutilAvc2MediaType              media_type;
    CellSysutilAvc2VoiceInitParam         voice_param;
    CellSysutilAvc2VideoInitParam         video_param;
    uint8_t                               reserved2[22];
} CellSysutilAvc2InitParam;

/* Per-player information */
typedef struct CellSysutilAvc2PlayerInfo
{
    SceNpMatching2RoomMemberId member_id;
    uint8_t                    joined;
    uint8_t                    connected;
    uint8_t                    mic_attached;
    uint8_t                    reserved[11];
} CellSysutilAvc2PlayerInfo;

/* Attribute parameter (union of int, float, pointer) */
typedef union CellSysutilAvc2AttributeParam
{
    uint64_t int_param;
    float    float_param;
    void    *ptr_param ATTRIBUTE_PRXPTR;
    uint8_t  reserved[64];
} CellSysutilAvc2AttributeParam;

/* Attribute descriptor */
typedef struct CellSysutilAvc2Attribute
{
    CellSysutilAvc2AttributeId    attr_id;
    CellSysutilAvc2AttributeParam attr_param;
} CellSysutilAvc2Attribute;

/* Room member list */
typedef struct CellSysutilAvc2MemberList
{
    SceNpMatching2RoomMemberId *member_id ATTRIBUTE_PRXPTR;
    uint8_t                     member_num;
    uint8_t                     reserved[3];
} CellSysutilAvc2RoomMemberList;

/* Member IP-and-port list (direct streaming) */
typedef struct CellSysutilAvc2MemberIpAndPortList
{
    SceNpMatching2RoomMemberId *member_id ATTRIBUTE_PRXPTR;
    struct in_addr             *dst_addr ATTRIBUTE_PRXPTR;
    in_port_t                  *dst_port ATTRIBUTE_PRXPTR;
    SceNpMatching2RoomMemberId  my_member_id;
    uint8_t                     member_num;
    uint8_t                     reserved[7];
} CellSysutilAvc2MemberIpAndPortList;

/* Streaming target specification */
typedef struct CellSysutilAvc2StreamingTarget
{
    CellSysutilAvc2ChatTargetMode target_mode;
    union {
        CellSysutilAvc2RoomMemberList     room_member_list;
        SceNpMatching2TeamId              team_id;
        CellSysutilAvc2MemberIpAndPortList ip_and_port_list;
        uint8_t                           reserved[32];
    };
    uint8_t reserved2[28];
} CellSysutilAvc2StreamingTarget;

/* ---- Callback types --------------------------------------------------- */

typedef uint32_t CellSysutilAvc2EventId;
typedef uint64_t CellSysutilAvc2EventParam;

typedef void (*CellSysutilAvc2Callback)(CellSysutilAvc2EventId event_id,
                                        CellSysutilAvc2EventParam event_param,
                                        void *userdata);

/* ---- Lifecycle -------------------------------------------------------- */

int cellSysutilAvc2InitParam(const uint16_t version,
                             CellSysutilAvc2InitParam *option);

int cellSysutilAvc2EstimateMemoryContainerSize(
    const CellSysutilAvc2InitParam *option,
    uint32_t *size);

int cellSysutilAvc2LoadAsync(SceNpMatching2ContextId ctx_id,
                             const sys_memory_container_t container,
                             CellSysutilAvc2Callback callback_func,
                             void *user_data,
                             const CellSysutilAvc2InitParam *init_param);

int cellSysutilAvc2UnloadAsync(void);

int cellSysutilAvc2Load(SceNpMatching2ContextId ctx_id,
                        const sys_memory_container_t container,
                        CellSysutilAvc2Callback callback_func,
                        void *user_data,
                        const CellSysutilAvc2InitParam *init_param);

int cellSysutilAvc2Unload(void);

int cellSysutilAvc2UnloadAsync2(const CellSysutilAvc2MediaType mediaType);

int cellSysutilAvc2Unload2(const CellSysutilAvc2MediaType mediaType);

/* ---- Stream control --------------------------------------------------- */

int cellSysutilAvc2StartStreaming(void);

int cellSysutilAvc2StopStreaming(void);

int cellSysutilAvc2StartStreaming2(const CellSysutilAvc2MediaType mediaType);

int cellSysutilAvc2StopStreaming2(const CellSysutilAvc2MediaType mediaType);

int cellSysutilAvc2SetStreamingTarget(const CellSysutilAvc2StreamingTarget target);

int cellSysutilAvc2SetStreamPriority(const uint8_t priority);

/* ---- Chat session management ------------------------------------------ */

int cellSysutilAvc2JoinChatRequest(const SceNpMatching2RoomId *room_id);

int cellSysutilAvc2LeaveChatRequest(void);

int cellSysutilAvc2JoinChat(const SceNpMatching2RoomId *room_id,
                            CellSysutilAvc2EventId *eventId,
                            CellSysutilAvc2EventParam *eventParam);

int cellSysutilAvc2LeaveChat(CellSysutilAvc2EventId *eventId,
                             CellSysutilAvc2EventParam *eventParam);

int cellSysutilAvc2EnumPlayers(int *players_num,
                               SceNpMatching2RoomMemberId players_id[]);

int cellSysutilAvc2GetPlayerInfo(const SceNpMatching2RoomMemberId *player_id,
                                 CellSysutilAvc2PlayerInfo *player_info);

/* ---- Voice control ---------------------------------------------------- */

int cellSysutilAvc2SetVoiceMuting(const uint8_t muting);

int cellSysutilAvc2GetVoiceMuting(uint8_t *muting);

int cellSysutilAvc2SetSpeakerMuting(const uint8_t muting);

int cellSysutilAvc2GetSpeakerMuting(uint8_t *muting);

int cellSysutilAvc2SetPlayerVoiceMuting(const SceNpMatching2RoomMemberId member_id,
                                        const uint8_t muting);

int cellSysutilAvc2GetPlayerVoiceMuting(const SceNpMatching2RoomMemberId member_id,
                                        uint8_t *muting);

int cellSysutilAvc2SetSpeakerVolumeLevel(const float level);

int cellSysutilAvc2GetSpeakerVolumeLevel(float *level);

int cellSysutilAvc2StartVoiceDetection(void);

int cellSysutilAvc2StopVoiceDetection(void);

/* ---- Mic / camera status ---------------------------------------------- */

int cellSysutilAvc2IsMicAttached(uint8_t *status);

int cellSysutilAvc2MicRead(void *ptr, uint32_t *pSize);

/* ---- Attribute access ------------------------------------------------- */

int cellSysutilAvc2SetAttribute(const CellSysutilAvc2Attribute *attr);

int cellSysutilAvc2GetAttribute(CellSysutilAvc2Attribute *attr);

#ifdef __cplusplus
}
#endif

#endif /* __PS3DK_CELL_SYSUTIL_AVC2_H__ */
