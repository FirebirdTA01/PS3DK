/*! \file cell/sysutil_avc.h
 \brief Legacy AVC voice/video chat panel (cellSysutilAvc*).

  Predecessor to libsysutil_avc2.  Provides a system-managed chat
  panel with NP-room-backed voice/video sessions.  Stripped from
  475-era reference headers; signatures and types reproduced from
  reverse-engineered RPCS3 stubs (rpcs3/Emu/Cell/Modules/cellSysutilAvc.{h,cpp}),
  which is the only available source.  Capitalisation normalised to
  the `CellSysutil*` form Sony uses everywhere else (RPCS3 mixes
  `Sysutil` and `SysUtil` between adjacent typedefs).

  Resolves through libsysutil_stub.a; RPCS3 stubs most entry points
  to no-ops or return success without doing real work.
*/

#ifndef PS3TC_CELL_SYSUTIL_AVC_H
#define PS3TC_CELL_SYSUTIL_AVC_H

#include <stdint.h>
#include <sys/memory.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifndef PS3TC_SYS_MEMORY_CONTAINER_T_DEFINED
#define PS3TC_SYS_MEMORY_CONTAINER_T_DEFINED
typedef sys_mem_container_t sys_memory_container_t;
#endif

/* ---- error codes -------------------------------------------------- */
#define CELL_AVC_ERROR_UNKNOWN                 0x8002b701
#define CELL_AVC_ERROR_NOT_SUPPORTED           0x8002b702
#define CELL_AVC_ERROR_NOT_INITIALIZED         0x8002b703
#define CELL_AVC_ERROR_ALREADY_INITIALIZED     0x8002b704
#define CELL_AVC_ERROR_INVALID_ARGUMENT        0x8002b705
#define CELL_AVC_ERROR_OUT_OF_MEMORY           0x8002b706
#define CELL_AVC_ERROR_BAD_ID                  0x8002b707
#define CELL_AVC_ERROR_INVALID_STATUS          0x8002b70a
#define CELL_AVC_ERROR_TIMEOUT                 0x8002b70b
#define CELL_AVC_ERROR_NO_SESSION              0x8002b70d
#define CELL_AVC_ERROR_INCOMPATIBLE_PROTOCOL   0x8002b70e
#define CELL_AVC_ERROR_PEER_UNREACHABLE        0x8002b710

/* ---- async event ids (delivered to CellSysutilAvcCallback) -------- */
typedef enum {
    CELL_AVC_EVENT_LOAD_SUCCEEDED                    = 0x00000001,
    CELL_AVC_EVENT_LOAD_FAILED                       = 0x00000002,
    CELL_AVC_EVENT_UNLOAD_SUCCEEDED                  = 0x00000003,
    CELL_AVC_EVENT_UNLOAD_FAILED                     = 0x00000004,
    CELL_AVC_EVENT_JOIN_SUCCEEDED                    = 0x00000005,
    CELL_AVC_EVENT_JOIN_FAILED                       = 0x00000006,
    CELL_AVC_EVENT_BYE_SUCCEEDED                     = 0x00000007,
    CELL_AVC_EVENT_BYE_FAILED                        = 0x00000008,
    CELL_AVC_EVENT_SYSTEM_NEW_MEMBER_JOINED          = 0x10000001,
    CELL_AVC_EVENT_SYSTEM_MEMBER_LEFT                = 0x10000002,
    CELL_AVC_EVENT_SYSTEM_SESSION_ESTABLISHED        = 0x10000003,
    CELL_AVC_EVENT_SYSTEM_SESSION_CANNOT_ESTABLISHED = 0x10000004,
    CELL_AVC_EVENT_SYSTEM_SESSION_DISCONNECTED       = 0x10000005,
    CELL_AVC_EVENT_SYSTEM_VOICE_DETECTED             = 0x10000006,
    CELL_AVC_EVENT_SYSTEM_MIC_DETECTED               = 0x10000007,
    CELL_AVC_EVENT_SYSTEM_CAMERA_DETECTED            = 0x10000008
} CellSysutilAvcEvent;

typedef enum {
    CELL_AVC_EVENT_PARAM_ERROR_UNKNOWN                = 0x00000001,
    CELL_AVC_EVENT_PARAM_ERROR_NOT_SUPPORTED          = 0x00000002,
    CELL_AVC_EVENT_PARAM_ERROR_INVALID_ARGUMENT       = 0x00000003,
    CELL_AVC_EVENT_PARAM_ERROR_OUT_OF_MEMORY          = 0x00000004,
    CELL_AVC_EVENT_PARAM_ERROR_INVALID_STATUS         = 0x00000005,
    CELL_AVC_EVENT_PARAM_ERROR_TIMEOUT                = 0x00000006,
    CELL_AVC_EVENT_PARAM_ERROR_CONTEXT_DOES_NOT_EXIST = 0x00000007,
    CELL_AVC_EVENT_PARAM_ERROR_ROOM_DOES_NOT_EXIST    = 0x00000008,
    CELL_AVC_EVENT_PARAM_ERROR_MEDIA_MISMATCHED       = 0x00000009,
    CELL_AVC_EVENT_PARAM_ERROR_MEMBER_EXCEEDED        = 0x0000000a,
    CELL_AVC_EVENT_PARAM_ERROR_MASTER_LEFT            = 0x0000000b,
    CELL_AVC_EVENT_PARAM_ERROR_NETWORK_ERROR          = 0x0000000c,
    CELL_AVC_EVENT_PARAM_ERROR_INCOMPATIBLE_PROTOCOL  = 0x0000000d
} CellSysutilAvcEventParam;

/* ---- attribute ids ----------------------------------------------- */
typedef enum {
    CELL_SYSUTIL_AVC_ATTRIBUTE_DEFAULT_TRANSITION_TYPE     = 0x00000001,
    CELL_SYSUTIL_AVC_ATTRIBUTE_DEFAULT_TRANSITION_DURATION = 0x00000002,
    CELL_SYSUTIL_AVC_ATTRIBUTE_DEFAULT_INITIAL_SHOW_STATUS = 0x00000003,
    CELL_SYSUTIL_AVC_ATTRIBUTE_VOICE_DETECT_EVENT_TYPE     = 0x00000004,
    CELL_SYSUTIL_AVC_ATTRIBUTE_VOICE_DETECT_INTERVAL_TIME  = 0x00000005,
    CELL_SYSUTIL_AVC_ATTRIBUTE_VOICE_DETECT_SIGNAL_LEVEL   = 0x00000006,
    CELL_SYSUTIL_AVC_ATTRIBUTE_ROOM_PRIVILEGE_TYPE         = 0x00000007,
    CELL_SYSUTIL_AVC_ATTRIBUTE_VIDEO_MAX_BITRATE           = 0x00000008
} CellSysutilAvcAttribute;

/* ---- panel layout / media / quality ------------------------------ */
typedef enum {
    CELL_SYSUTIL_AVC_LAYOUT_LEFT   = 0x00000000,
    CELL_SYSUTIL_AVC_LAYOUT_RIGHT  = 0x00000001,
    CELL_SYSUTIL_AVC_LAYOUT_TOP    = 0x00000002,
    CELL_SYSUTIL_AVC_LAYOUT_BOTTOM = 0x00000003
} CellSysutilAvcLayoutMode;

typedef enum {
    CELL_SYSUTIL_AVC_VOICE_CHAT = 0x00000001,
    CELL_SYSUTIL_AVC_VIDEO_CHAT = 0x00000002
} CellSysutilAvcMediaType;

typedef enum {
    CELL_SYSUTIL_AVC_VIDEO_QUALITY_DEFAULT = 0x00000001
} CellSysutilAvcVideoQuality;

typedef enum {
    CELL_SYSUTIL_AVC_VOICE_QUALITY_DEFAULT = 0x00000001
} CellSysutilAvcVoiceQuality;

/* ---- handles / callback ------------------------------------------ */
typedef uint32_t CellSysutilAvcRequestId;

/* Forward-declared NP id/room types.  Layouts match np/common.h:
 *   SceNpId    : 36 bytes (16 onlineId + 8 opt + 8 reserved + 4 term/dummy)
 *   SceNpRoomId: 36 bytes (28 opt + 8 reserved)
 * Users mixing this header with cell/np stuff should include np/common.h
 * first; the include guards there will dominate. */
#ifndef _SCE_NP_COMMON_H_
typedef struct CellSysutilAvcSceNpOnlineId {
    char    data[16];
    char    term;
    char    dummy[3];
} CellSysutilAvcSceNpOnlineId;

typedef struct {
    CellSysutilAvcSceNpOnlineId handle;
    uint8_t                     opt[8];
    uint8_t                     reserved[8];
} SceNpId;

typedef struct {
    uint8_t opt[28];
    uint8_t reserved[8];
} SceNpRoomId;
#endif

/* Async-event callback.  Fired from the sysutil callback thread once
 * cellSysutilCheckCallback() runs after Load/Unload/Join/Bye/system
 * events. */
typedef void (*CellSysutilAvcCallback)(CellSysutilAvcRequestId request_id,
                                       CellSysutilAvcEvent event_id,
                                       CellSysutilAvcEventParam event_param,
                                       void *userdata);

/* ---- option / data structs --------------------------------------- */
typedef struct {
    int32_t   avcOptionParamVersion;
    uint8_t   sharingVideoBuffer;
    uint8_t   reserved[3];
    int32_t   maxPlayers;
} CellSysutilAvcOptionParam;

typedef struct {
    SceNpId  npid;
    int32_t  data;
} CellSysutilAvcVoiceDetectData;

/* ---- entry points ------------------------------------------------- */
extern int cellSysutilAvcLoadAsync(CellSysutilAvcCallback func,
                                   void *userdata,
                                   sys_memory_container_t container,
                                   CellSysutilAvcMediaType media,
                                   CellSysutilAvcVideoQuality videoQuality,
                                   CellSysutilAvcVoiceQuality voiceQuality,
                                   CellSysutilAvcRequestId *request_id);
extern int cellSysutilAvcUnloadAsync(CellSysutilAvcRequestId *request_id);

extern int cellSysutilAvcShowPanel(void);
extern int cellSysutilAvcHidePanel(void);

extern int cellSysutilAvcGetShowStatus(uint8_t *is_visible);
extern int cellSysutilAvcGetLayoutMode(CellSysutilAvcLayoutMode *layout);
extern int cellSysutilAvcSetLayoutMode(CellSysutilAvcLayoutMode layout);

extern int cellSysutilAvcGetVoiceMuting(uint8_t *is_muting);
extern int cellSysutilAvcSetVoiceMuting(uint8_t is_muting);
extern int cellSysutilAvcGetVideoMuting(uint8_t *is_muting);
extern int cellSysutilAvcSetVideoMuting(uint8_t is_muting);

extern int cellSysutilAvcGetSpeakerVolumeLevel(int32_t *volumeLevel);
extern int cellSysutilAvcSetSpeakerVolumeLevel(int32_t volumeLevel);

extern int cellSysutilAvcGetAttribute(CellSysutilAvcAttribute attr_id, void **param);
extern int cellSysutilAvcSetAttribute(CellSysutilAvcAttribute attr_id, void *param);

extern int cellSysutilAvcEnumPlayers(SceNpId *players_id, int32_t *players_num);

extern int cellSysutilAvcJoinRequest(uint32_t ctx_id,
                                     const SceNpRoomId *room_id,
                                     CellSysutilAvcRequestId *request_id);
extern int cellSysutilAvcByeRequest(CellSysutilAvcRequestId *request_id);
extern int cellSysutilAvcCancelJoinRequest(CellSysutilAvcRequestId *request_id);
extern int cellSysutilAvcCancelByeRequest(CellSysutilAvcRequestId *request_id);

#ifdef __cplusplus
}
#endif

#endif  /* PS3TC_CELL_SYSUTIL_AVC_H */
