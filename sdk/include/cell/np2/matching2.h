#ifndef __PS3DK_CELL_NP2_MATCHING2_H__
#define __PS3DK_CELL_NP2_MATCHING2_H__

#include <cell/np/common.h>
#include <cell/np/signaling.h>
#include <cell/rtc.h>
#include <netinet/in.h>
#include <stdint.h>

#ifndef ATTRIBUTE_PRXPTR
#define ATTRIBUTE_PRXPTR
#endif

#ifdef __cplusplus
extern "C" {
#endif

#define SCE_NP_MATCHING2_ERROR_OUT_OF_MEMORY               0x80022301
#define SCE_NP_MATCHING2_ERROR_ALREADY_INITIALIZED         0x80022302
#define SCE_NP_MATCHING2_ERROR_NOT_INITIALIZED             0x80022303
#define SCE_NP_MATCHING2_ERROR_CONTEXT_MAX                 0x80022304
#define SCE_NP_MATCHING2_ERROR_CONTEXT_ALREADY_EXISTS      0x80022305
#define SCE_NP_MATCHING2_ERROR_CONTEXT_NOT_FOUND           0x80022306
#define SCE_NP_MATCHING2_ERROR_CONTEXT_ALREADY_STARTED     0x80022307
#define SCE_NP_MATCHING2_ERROR_CONTEXT_NOT_STARTED         0x80022308
#define SCE_NP_MATCHING2_ERROR_SERVER_NOT_FOUND            0x80022309
#define SCE_NP_MATCHING2_ERROR_INVALID_ARGUMENT            0x8002230a
#define SCE_NP_MATCHING2_ERROR_INVALID_CONTEXT_ID          0x8002230b
#define SCE_NP_MATCHING2_ERROR_INVALID_SERVER_ID           0x8002230c
#define SCE_NP_MATCHING2_ERROR_INVALID_WORLD_ID            0x8002230d
#define SCE_NP_MATCHING2_ERROR_INVALID_LOBBY_ID            0x8002230e
#define SCE_NP_MATCHING2_ERROR_INVALID_ROOM_ID             0x8002230f
#define SCE_NP_MATCHING2_ERROR_INVALID_MEMBER_ID           0x80022310
#define SCE_NP_MATCHING2_ERROR_INVALID_ATTRIBUTE_ID        0x80022311
#define SCE_NP_MATCHING2_ERROR_INVALID_CASTTYPE            0x80022312
#define SCE_NP_MATCHING2_ERROR_INVALID_SORT_METHOD         0x80022313
#define SCE_NP_MATCHING2_ERROR_INVALID_MAX_SLOT            0x80022314
#define SCE_NP_MATCHING2_ERROR_INVALID_MATCHING_SPACE      0x80022316
#define SCE_NP_MATCHING2_ERROR_INVALID_BLOCK_KICK_FLAG     0x80022318
#define SCE_NP_MATCHING2_ERROR_INVALID_MESSAGE_TARGET      0x80022319
#define SCE_NP_MATCHING2_ERROR_RANGE_FILTER_MAX            0x8002231a
#define SCE_NP_MATCHING2_ERROR_INSUFFICIENT_BUFFER         0x8002231b
#define SCE_NP_MATCHING2_ERROR_DESTINATION_DISAPPEARED     0x8002231c
#define SCE_NP_MATCHING2_ERROR_REQUEST_TIMEOUT             0x8002231d
#define SCE_NP_MATCHING2_ERROR_INVALID_ALIGNMENT           0x8002231e
#define SCE_NP_MATCHING2_ERROR_REQUEST_CB_QUEUE_OVERFLOW   0x8002231f
#define SCE_NP_MATCHING2_ERROR_EVENT_CB_QUEUE_OVERFLOW     0x80022320
#define SCE_NP_MATCHING2_ERROR_MSG_CB_QUEUE_OVERFLOW       0x80022321
#define SCE_NP_MATCHING2_ERROR_CONNECTION_CLOSED_BY_SERVER 0x80022322
#define SCE_NP_MATCHING2_ERROR_SSL_VERIFY_FAILED           0x80022323
#define SCE_NP_MATCHING2_ERROR_SSL_HANDSHAKE               0x80022324
#define SCE_NP_MATCHING2_ERROR_SSL_SEND                    0x80022325
#define SCE_NP_MATCHING2_ERROR_SSL_RECV                    0x80022326
#define SCE_NP_MATCHING2_ERROR_JOINED_SESSION_MAX          0x80022327
#define SCE_NP_MATCHING2_ERROR_ALREADY_JOINED              0x80022328
#define SCE_NP_MATCHING2_ERROR_INVALID_SESSION_TYPE        0x80022329
#define SCE_NP_MATCHING2_ERROR_CLAN_LOBBY_NOT_EXIST        0x8002232a
#define SCE_NP_MATCHING2_ERROR_NP_SIGNED_OUT               0x8002232b
#define SCE_NP_MATCHING2_ERROR_CONTEXT_UNAVAILABLE         0x8002232c
#define SCE_NP_MATCHING2_ERROR_SERVER_NOT_AVAILABLE        0x8002232d
#define SCE_NP_MATCHING2_ERROR_NOT_ALLOWED                 0x8002232e
#define SCE_NP_MATCHING2_ERROR_ABORTED                     0x8002232f
#define SCE_NP_MATCHING2_ERROR_REQUEST_NOT_FOUND           0x80022330
#define SCE_NP_MATCHING2_ERROR_SESSION_DESTROYED           0x80022331
#define SCE_NP_MATCHING2_ERROR_CONTEXT_STOPPED             0x80022332
#define SCE_NP_MATCHING2_ERROR_INVALID_REQUEST_PARAMETER   0x80022333
#define SCE_NP_MATCHING2_ERROR_NOT_NP_SIGN_IN              0x80022334
#define SCE_NP_MATCHING2_ERROR_ROOM_NOT_FOUND              0x80022335
#define SCE_NP_MATCHING2_ERROR_ROOM_MEMBER_NOT_FOUND       0x80022336
#define SCE_NP_MATCHING2_ERROR_LOBBY_NOT_FOUND             0x80022337
#define SCE_NP_MATCHING2_ERROR_LOBBY_MEMBER_NOT_FOUND      0x80022338
#define SCE_NP_MATCHING2_ERROR_EVENT_DATA_NOT_FOUND        0x80022339
#define SCE_NP_MATCHING2_ERROR_KEEPALIVE_TIMEOUT           0x8002233a
#define SCE_NP_MATCHING2_ERROR_TIMEOUT_TOO_SHORT           0x8002233b
#define SCE_NP_MATCHING2_ERROR_TIMEDOUT                    0x8002233c
#define SCE_NP_MATCHING2_ERROR_CREATE_HEAP                 0x8002233d
#define SCE_NP_MATCHING2_ERROR_INVALID_ATTRIBUTE_SIZE      0x8002233e
#define SCE_NP_MATCHING2_ERROR_CANNOT_ABORT                0x8002233f
#define SCE_NP_MATCHING2_RESOLVER_ERROR_NO_DNS_SERVER      0x800223a2
#define SCE_NP_MATCHING2_RESOLVER_ERROR_INVALID_PACKET     0x800223ad
#define SCE_NP_MATCHING2_RESOLVER_ERROR_TIMEOUT            0x800223b0
#define SCE_NP_MATCHING2_RESOLVER_ERROR_NO_RECORD          0x800223b1
#define SCE_NP_MATCHING2_RESOLVER_ERROR_RES_PACKET_FORMAT  0x800223b2
#define SCE_NP_MATCHING2_RESOLVER_ERROR_RES_SERVER_FAILURE 0x800223b3
#define SCE_NP_MATCHING2_RESOLVER_ERROR_NO_HOST            0x800223b4
#define SCE_NP_MATCHING2_RESOLVER_ERROR_RESP_TRUNCATED     0x800223bc
#define SCE_NP_MATCHING2_SERVER_ERROR_BAD_REQUEST          0x80022b01
#define SCE_NP_MATCHING2_SERVER_ERROR_SERVICE_UNAVAILABLE  0x80022b02
#define SCE_NP_MATCHING2_SERVER_ERROR_BUSY                 0x80022b03
#define SCE_NP_MATCHING2_SERVER_ERROR_INTERNAL_SERVER_ERROR 0x80022b05
#define SCE_NP_MATCHING2_SERVER_ERROR_FORBIDDEN            0x80022b07
#define SCE_NP_MATCHING2_SERVER_ERROR_NO_SUCH_WORLD        0x80022b11
#define SCE_NP_MATCHING2_SERVER_ERROR_NO_SUCH_LOBBY        0x80022b12
#define SCE_NP_MATCHING2_SERVER_ERROR_NO_SUCH_ROOM         0x80022b13
#define SCE_NP_MATCHING2_SERVER_ERROR_LOBBY_FULL           0x80022b18
#define SCE_NP_MATCHING2_SERVER_ERROR_ROOM_FULL            0x80022b19
#define SCE_NP_MATCHING2_SERVER_ERROR_ALREADY_JOINED       0x80022b30

#define SCE_NP_MATCHING2_SESSION_PASSWORD_SIZE 8
#define SCE_NP_MATCHING2_PRESENCE_OPTION_DATA_SIZE 16
#define SCE_NP_MATCHING2_GROUP_LABEL_SIZE 8
#define SCE_NP_MATCHING2_TITLE_PASSPHRASE_SIZE 128
#define SCE_NP_MATCHING2_ROOM_MAX_SLOT 64
#define SCE_NP_MATCHING2_LOBBY_MAX_SLOT 256
#define SCE_NP_MATCHING2_CHAT_MSG_MAX_SIZE 1024
#define SCE_NP_MATCHING2_BIN_MSG_MAX_SIZE 1024

#define SCE_NP_MATCHING2_OPERATOR_EQ 1
#define SCE_NP_MATCHING2_OPERATOR_NE 2
#define SCE_NP_MATCHING2_OPERATOR_LT 3
#define SCE_NP_MATCHING2_OPERATOR_LE 4
#define SCE_NP_MATCHING2_OPERATOR_GT 5
#define SCE_NP_MATCHING2_OPERATOR_GE 6
#define SCE_NP_MATCHING2_CASTTYPE_BROADCAST 1
#define SCE_NP_MATCHING2_CASTTYPE_UNICAST 2
#define SCE_NP_MATCHING2_CASTTYPE_MULTICAST 3
#define SCE_NP_MATCHING2_SESSION_TYPE_LOBBY 1
#define SCE_NP_MATCHING2_SESSION_TYPE_ROOM 2
#define SCE_NP_MATCHING2_SIGNALING_TYPE_NONE 0
#define SCE_NP_MATCHING2_SIGNALING_TYPE_MESH 1
#define SCE_NP_MATCHING2_SIGNALING_TYPE_STAR 2
#define SCE_NP_MATCHING2_SIGNALING_FLAG_MANUAL_MODE 0x01
#define SCE_NP_MATCHING2_ROLE_MEMBER 1
#define SCE_NP_MATCHING2_ROLE_OWNER 2
#define SCE_NP_MATCHING2_BLOCKKICKFLAG_OK 0
#define SCE_NP_MATCHING2_BLOCKKICKFLAG_NG 1
#define SCE_NP_MATCHING2_SORT_METHOD_JOIN_DATE 0
#define SCE_NP_MATCHING2_SORT_METHOD_SLOT_NUMBER 1

typedef uint16_t SceNpMatching2ServerId;
typedef uint32_t SceNpMatching2WorldId;
typedef uint16_t SceNpMatching2WorldNumber;
typedef uint64_t SceNpMatching2LobbyId;
typedef uint16_t SceNpMatching2LobbyNumber;
typedef uint16_t SceNpMatching2LobbyMemberId;
typedef uint64_t SceNpMatching2RoomId;
typedef uint16_t SceNpMatching2RoomNumber;
typedef uint16_t SceNpMatching2RoomMemberId;
typedef uint8_t SceNpMatching2RoomGroupId;
typedef uint8_t SceNpMatching2TeamId;
typedef uint16_t SceNpMatching2ContextId;
typedef uint32_t SceNpMatching2RequestId;
typedef uint16_t SceNpMatching2AttributeId;
typedef uint32_t SceNpMatching2FlagAttr;
typedef uint8_t SceNpMatching2NatType;
typedef uint8_t SceNpMatching2Operator;
typedef uint8_t SceNpMatching2CastType;
typedef uint8_t SceNpMatching2SessionType;
typedef uint8_t SceNpMatching2SignalingType;
typedef uint8_t SceNpMatching2SignalingFlag;
typedef uint8_t SceNpMatching2EventCause;
typedef uint8_t SceNpMatching2ServerStatus;
typedef uint8_t SceNpMatching2Role;
typedef uint8_t SceNpMatching2BlockKickFlag;
typedef uint64_t SceNpMatching2RoomPasswordSlotMask;
typedef uint64_t SceNpMatching2RoomJoinedSlotMask;
typedef uint16_t SceNpMatching2Event;
typedef uint32_t SceNpMatching2EventKey;
typedef uint32_t SceNpMatching2SignalingRequestId;
typedef SceNpCommunicationPassphrase SceNpMatching2TitlePassphrase;

typedef void (*SceNpMatching2RequestCallback)(SceNpMatching2ContextId ctxId, SceNpMatching2RequestId reqId, SceNpMatching2Event event, SceNpMatching2EventKey eventKey, int32_t errorCode, uint32_t dataSize, void *arg);
typedef void (*SceNpMatching2RoomEventCallback)(SceNpMatching2ContextId ctxId, SceNpMatching2RoomId roomId, SceNpMatching2Event event, SceNpMatching2EventKey eventKey, int32_t errorCode, uint32_t dataSize, void *arg);
typedef void (*SceNpMatching2RoomMessageCallback)(SceNpMatching2ContextId ctxId, SceNpMatching2RoomId roomId, SceNpMatching2RoomMemberId srcMemberId, SceNpMatching2Event event, SceNpMatching2EventKey eventKey, int32_t errorCode, uint32_t dataSize, void *arg);
typedef void (*SceNpMatching2LobbyEventCallback)(SceNpMatching2ContextId ctxId, SceNpMatching2LobbyId lobbyId, SceNpMatching2Event event, SceNpMatching2EventKey eventKey, int32_t errorCode, uint32_t dataSize, void *arg);
typedef void (*SceNpMatching2LobbyMessageCallback)(SceNpMatching2ContextId ctxId, SceNpMatching2LobbyId lobbyId, SceNpMatching2LobbyMemberId srcMemberId, SceNpMatching2Event event, SceNpMatching2EventKey eventKey, int32_t errorCode, uint32_t dataSize, void *arg);
typedef void (*SceNpMatching2SignalingCallback)(SceNpMatching2ContextId ctxId, SceNpMatching2RoomId roomId, SceNpMatching2RoomMemberId peerMemberId, SceNpMatching2Event event, int32_t errorCode, void *arg);
typedef void (*SceNpMatching2ContextCallback)(SceNpMatching2ContextId ctxId, SceNpMatching2Event event, SceNpMatching2EventCause eventCause, int32_t errorCode, void *arg);

typedef struct SceNpMatching2SessionPassword { uint8_t data[SCE_NP_MATCHING2_SESSION_PASSWORD_SIZE]; } SceNpMatching2SessionPassword;
typedef struct SceNpMatching2GroupLabel { uint8_t data[SCE_NP_MATCHING2_GROUP_LABEL_SIZE]; } SceNpMatching2GroupLabel;

typedef struct SceNpMatching2PresenceOptionData {
    uint8_t data[SCE_NP_MATCHING2_PRESENCE_OPTION_DATA_SIZE];
    uint32_t length;
} SceNpMatching2PresenceOptionData;

typedef struct SceNpMatching2IntAttr {
    SceNpMatching2AttributeId id;
    uint8_t padding[2];
    uint32_t num;
} SceNpMatching2IntAttr;

typedef struct SceNpMatching2BinAttr {
    SceNpMatching2AttributeId id;
    uint8_t padding[2];
    uint8_t *ptr ATTRIBUTE_PRXPTR;
    uint32_t size;
} SceNpMatching2BinAttr;

typedef struct SceNpMatching2RangeFilter { uint32_t startIndex; uint32_t max; } SceNpMatching2RangeFilter;
typedef struct SceNpMatching2Range { uint32_t startIndex; uint32_t total; uint32_t size; } SceNpMatching2Range;

typedef struct SceNpMatching2RequestOptParam {
    SceNpMatching2RequestCallback cbFunc ATTRIBUTE_PRXPTR;
    void *cbFuncArg ATTRIBUTE_PRXPTR;
    uint32_t timeout;
    uint16_t appReqId;
    uint8_t padding[2];
} SceNpMatching2RequestOptParam;

typedef struct SceNpMatching2UtilityInitParam {
    uint32_t containerId;
    uint32_t requestCbQueueLen;
    uint32_t sessionEventCbQueueLen;
    uint32_t sessionMsgCbQueueLen;
    uint8_t reserved[16];
} SceNpMatching2UtilityInitParam;

typedef struct SceNpMatching2MemoryInfo {
    uint32_t totalMemSize;
    uint32_t curMemUsage;
    uint32_t maxMemUsage;
    uint8_t reserved[12];
} SceNpMatching2MemoryInfo;

typedef struct SceNpMatching2CbQueueInfo {
    uint32_t requestCbQueueLen;
    uint32_t curRequestCbQueueLen;
    uint32_t maxRequestCbQueueLen;
    uint32_t sessionEventCbQueueLen;
    uint32_t curSessionEventCbQueueLen;
    uint32_t maxSessionEventCbQueueLen;
    uint32_t sessionMsgCbQueueLen;
    uint32_t curSessionMsgCbQueueLen;
    uint32_t maxSessionMsgCbQueueLen;
    uint8_t reserved[12];
} SceNpMatching2CbQueueInfo;

typedef struct SceNpMatching2SignalingNetInfo {
    uint32_t size;
    struct in_addr localAddr;
    struct in_addr mappedAddr;
    uint32_t natStatus;
} SceNpMatching2SignalingNetInfo;

typedef struct SceNpMatching2JoinedSessionInfo {
    uint8_t sessionType;
    uint8_t padding1[1];
    SceNpMatching2ServerId serverId;
    SceNpMatching2WorldId worldId;
    SceNpMatching2LobbyId lobbyId;
    SceNpMatching2RoomId roomId;
    CellRtcTick joinDate;
} SceNpMatching2JoinedSessionInfo;

typedef struct SceNpMatching2UserInfo {
    struct SceNpMatching2UserInfo *next ATTRIBUTE_PRXPTR;
    SceNpUserInfo2 userInfo;
    SceNpMatching2BinAttr *userBinAttr ATTRIBUTE_PRXPTR;
    uint32_t userBinAttrNum;
    SceNpMatching2JoinedSessionInfo joinedSessionInfo;
    uint32_t joinedSessionInfoNum;
} SceNpMatching2UserInfo;

typedef struct SceNpMatching2Server {
    SceNpMatching2ServerId serverId;
    SceNpMatching2ServerStatus status;
    uint8_t padding[1];
} SceNpMatching2Server;

typedef struct SceNpMatching2World {
    SceNpMatching2WorldId worldId;
    uint32_t numOfLobby;
    uint32_t maxNumOfTotalLobbyMember;
    uint32_t curNumOfTotalLobbyMember;
    uint32_t curNumOfRoom;
    uint32_t curNumOfTotalRoomMember;
    uint8_t withEntitlementId;
    SceNpEntitlementId entitlementId;
    uint8_t padding[3];
} SceNpMatching2World;

typedef struct SceNpMatching2RoomGroup {
    SceNpMatching2RoomGroupId groupId;
    uint8_t withPassword;
    uint8_t withLabel;
    uint8_t padding[1];
    SceNpMatching2GroupLabel label;
    uint32_t slotNum;
    uint32_t curGroupMemberNum;
} SceNpMatching2RoomGroup;

typedef union SceNpMatching2RoomMessageDestination {
    SceNpMatching2RoomMemberId unicastTarget;
    struct {
        SceNpMatching2RoomMemberId *memberId ATTRIBUTE_PRXPTR;
        uint32_t memberIdNum;
    } multicastTarget;
    SceNpMatching2TeamId multicastTargetTeamId;
} SceNpMatching2RoomMessageDestination;

typedef union SceNpMatching2LobbyMessageDestination {
    SceNpMatching2LobbyMemberId unicastTarget;
    struct {
        SceNpMatching2LobbyMemberId *memberId ATTRIBUTE_PRXPTR;
        uint32_t memberIdNum;
    } multicastTarget;
} SceNpMatching2LobbyMessageDestination;

typedef struct SceNpMatching2InvitationData {
    SceNpMatching2JoinedSessionInfo *targetSession ATTRIBUTE_PRXPTR;
    uint32_t targetSessionNum;
    void *optData ATTRIBUTE_PRXPTR;
    uint32_t optDataLen;
} SceNpMatching2InvitationData;

typedef struct SceNpMatching2SignalingOptParam {
    SceNpMatching2SignalingType type;
    SceNpMatching2SignalingFlag flag;
    SceNpMatching2RoomMemberId hubMemberId;
    uint8_t reserved2[4];
} SceNpMatching2SignalingOptParam;

typedef struct SceNpMatching2RoomSlotInfo {
    SceNpMatching2RoomId roomId;
    SceNpMatching2RoomJoinedSlotMask joinedSlotMask;
    SceNpMatching2RoomPasswordSlotMask passwordSlotMask;
    uint16_t publicSlotNum;
    uint16_t privateSlotNum;
    uint16_t openPublicSlotNum;
    uint16_t openPrivateSlotNum;
} SceNpMatching2RoomSlotInfo;

typedef struct SceNpMatching2RoomMemberDataInternal {
    struct SceNpMatching2RoomMemberDataInternal *next ATTRIBUTE_PRXPTR;
    SceNpUserInfo2 userInfo;
    CellRtcTick joinDate;
    SceNpMatching2RoomMemberId memberId;
    SceNpMatching2TeamId teamId;
    uint8_t padding1[1];
    SceNpMatching2RoomGroup *roomGroup ATTRIBUTE_PRXPTR;
    SceNpMatching2NatType natType;
    uint8_t padding2[3];
    SceNpMatching2FlagAttr flagAttr;
    SceNpMatching2BinAttr *roomMemberBinAttrInternal ATTRIBUTE_PRXPTR;
    uint32_t roomMemberBinAttrInternalNum;
} SceNpMatching2RoomMemberDataInternal;

typedef struct SceNpMatching2RoomDataInternal {
    SceNpMatching2ServerId serverId;
    uint8_t padding1[2];
    SceNpMatching2WorldId worldId;
    SceNpMatching2LobbyId lobbyId;
    SceNpMatching2RoomId roomId;
    SceNpMatching2RoomPasswordSlotMask passwordSlotMask;
    uint32_t maxSlot;
    SceNpMatching2RoomMemberDataInternal *members ATTRIBUTE_PRXPTR;
    uint32_t membersNum;
    SceNpMatching2RoomMemberDataInternal *me ATTRIBUTE_PRXPTR;
    SceNpMatching2RoomMemberDataInternal *owner ATTRIBUTE_PRXPTR;
    SceNpMatching2RoomGroup *roomGroup ATTRIBUTE_PRXPTR;
    uint32_t roomGroupNum;
    SceNpMatching2FlagAttr flagAttr;
    SceNpMatching2BinAttr *roomBinAttrInternal ATTRIBUTE_PRXPTR;
    uint32_t roomBinAttrInternalNum;
} SceNpMatching2RoomDataInternal;

typedef struct SceNpMatching2RoomDataExternal {
    struct SceNpMatching2RoomDataExternal *next ATTRIBUTE_PRXPTR;
    SceNpMatching2ServerId serverId;
    uint8_t padding1[2];
    SceNpMatching2WorldId worldId;
    uint16_t publicSlotNum;
    uint16_t privateSlotNum;
    SceNpMatching2LobbyId lobbyId;
    SceNpMatching2RoomId roomId;
    uint16_t openPublicSlotNum;
    uint16_t maxSlot;
    uint16_t openPrivateSlotNum;
    uint16_t curMemberNum;
    SceNpMatching2RoomPasswordSlotMask passwordSlotMask;
    SceNpUserInfo2 *owner ATTRIBUTE_PRXPTR;
    SceNpMatching2RoomGroup *roomGroup ATTRIBUTE_PRXPTR;
    uint32_t roomGroupNum;
    SceNpMatching2FlagAttr flagAttr;
    SceNpMatching2IntAttr *roomSearchableIntAttrExternal ATTRIBUTE_PRXPTR;
    uint32_t roomSearchableIntAttrExternalNum;
    SceNpMatching2BinAttr *roomSearchableBinAttrExternal ATTRIBUTE_PRXPTR;
    uint32_t roomSearchableBinAttrExternalNum;
    SceNpMatching2BinAttr *roomBinAttrExternal ATTRIBUTE_PRXPTR;
    uint32_t roomBinAttrExternalNum;
} SceNpMatching2RoomDataExternal;

typedef struct SceNpMatching2CreateServerContextRequest { SceNpMatching2ServerId serverId; } SceNpMatching2CreateServerContextRequest;
typedef struct SceNpMatching2DeleteServerContextRequest { SceNpMatching2ServerId serverId; } SceNpMatching2DeleteServerContextRequest;
typedef struct SceNpMatching2GetServerInfoRequest { SceNpMatching2ServerId serverId; } SceNpMatching2GetServerInfoRequest;
typedef struct SceNpMatching2GetServerInfoResponse { SceNpMatching2Server server; } SceNpMatching2GetServerInfoResponse;
typedef struct SceNpMatching2GetWorldInfoListRequest { SceNpMatching2ServerId serverId; } SceNpMatching2GetWorldInfoListRequest;
typedef struct SceNpMatching2GetWorldInfoListResponse { SceNpMatching2World *world ATTRIBUTE_PRXPTR; uint32_t worldNum; } SceNpMatching2GetWorldInfoListResponse;

typedef struct SceNpMatching2SetUserInfoRequest {
    SceNpMatching2ServerId serverId;
    uint8_t padding[2];
    SceNpMatching2BinAttr *userBinAttr ATTRIBUTE_PRXPTR;
    uint32_t userBinAttrNum;
} SceNpMatching2SetUserInfoRequest;

typedef struct SceNpMatching2GetUserInfoListRequest {
    SceNpMatching2ServerId serverId;
    uint8_t padding[2];
    SceNpId *npId ATTRIBUTE_PRXPTR;
    uint32_t npIdNum;
    SceNpMatching2AttributeId *attrId ATTRIBUTE_PRXPTR;
    uint32_t attrIdNum;
    int32_t option;
} SceNpMatching2GetUserInfoListRequest;

typedef struct SceNpMatching2GetUserInfoListResponse { SceNpMatching2UserInfo *userInfo ATTRIBUTE_PRXPTR; uint32_t userInfoNum; } SceNpMatching2GetUserInfoListResponse;
typedef struct SceNpMatching2GetRoomMemberDataExternalListRequest { SceNpMatching2RoomId roomId; } SceNpMatching2GetRoomMemberDataExternalListRequest;
typedef struct SceNpMatching2GetRoomMemberDataExternalListResponse { void *roomMemberDataExternal ATTRIBUTE_PRXPTR; uint32_t roomMemberDataExternalNum; } SceNpMatching2GetRoomMemberDataExternalListResponse;

typedef struct SceNpMatching2SetRoomDataExternalRequest {
    SceNpMatching2RoomId roomId;
    SceNpMatching2IntAttr *roomSearchableIntAttrExternal ATTRIBUTE_PRXPTR;
    uint32_t roomSearchableIntAttrExternalNum;
    SceNpMatching2BinAttr *roomSearchableBinAttrExternal ATTRIBUTE_PRXPTR;
    uint32_t roomSearchableBinAttrExternalNum;
    SceNpMatching2BinAttr *roomBinAttrExternal ATTRIBUTE_PRXPTR;
    uint32_t roomBinAttrExternalNum;
} SceNpMatching2SetRoomDataExternalRequest;

typedef struct SceNpMatching2GetRoomDataExternalListRequest {
    SceNpMatching2RoomId *roomId ATTRIBUTE_PRXPTR;
    uint32_t roomIdNum;
    const SceNpMatching2AttributeId *attrId ATTRIBUTE_PRXPTR;
    uint32_t attrIdNum;
} SceNpMatching2GetRoomDataExternalListRequest;

typedef struct SceNpMatching2GetRoomDataExternalListResponse { SceNpMatching2RoomDataExternal *roomDataExternal ATTRIBUTE_PRXPTR; uint32_t roomDataExternalNum; } SceNpMatching2GetRoomDataExternalListResponse;

typedef struct SceNpMatching2CreateJoinRoomRequest {
    SceNpMatching2WorldId worldId;
    uint8_t padding1[4];
    SceNpMatching2LobbyId lobbyId;
    uint32_t maxSlot;
    SceNpMatching2FlagAttr flagAttr;
    SceNpMatching2BinAttr *roomBinAttrInternal ATTRIBUTE_PRXPTR;
    uint32_t roomBinAttrInternalNum;
    SceNpMatching2IntAttr *roomSearchableIntAttrExternal ATTRIBUTE_PRXPTR;
    uint32_t roomSearchableIntAttrExternalNum;
    SceNpMatching2BinAttr *roomSearchableBinAttrExternal ATTRIBUTE_PRXPTR;
    uint32_t roomSearchableBinAttrExternalNum;
    SceNpMatching2BinAttr *roomBinAttrExternal ATTRIBUTE_PRXPTR;
    uint32_t roomBinAttrExternalNum;
    SceNpMatching2SessionPassword *roomPassword ATTRIBUTE_PRXPTR;
    void *groupConfig ATTRIBUTE_PRXPTR;
    uint32_t groupConfigNum;
    SceNpMatching2RoomPasswordSlotMask *passwordSlotMask ATTRIBUTE_PRXPTR;
    SceNpId *allowedUser ATTRIBUTE_PRXPTR;
    uint32_t allowedUserNum;
    SceNpId *blockedUser ATTRIBUTE_PRXPTR;
    uint32_t blockedUserNum;
    SceNpMatching2GroupLabel *joinRoomGroupLabel ATTRIBUTE_PRXPTR;
    SceNpMatching2BinAttr *roomMemberBinAttrInternal ATTRIBUTE_PRXPTR;
    uint32_t roomMemberBinAttrInternalNum;
    SceNpMatching2TeamId teamId;
    uint8_t padding2[3];
    SceNpMatching2SignalingOptParam *sigOptParam ATTRIBUTE_PRXPTR;
    uint8_t padding3[4];
} SceNpMatching2CreateJoinRoomRequest;

typedef struct SceNpMatching2CreateJoinRoomResponse { SceNpMatching2RoomDataInternal *roomDataInternal ATTRIBUTE_PRXPTR; } SceNpMatching2CreateJoinRoomResponse;

typedef struct SceNpMatching2JoinRoomRequest {
    SceNpMatching2RoomId roomId;
    SceNpMatching2SessionPassword *roomPassword ATTRIBUTE_PRXPTR;
    SceNpMatching2GroupLabel *joinRoomGroupLabel ATTRIBUTE_PRXPTR;
    SceNpMatching2BinAttr *roomMemberBinAttrInternal ATTRIBUTE_PRXPTR;
    uint32_t roomMemberBinAttrInternalNum;
    SceNpMatching2PresenceOptionData optData;
    SceNpMatching2TeamId teamId;
    uint8_t padding[3];
} SceNpMatching2JoinRoomRequest;

typedef struct SceNpMatching2JoinRoomResponse { SceNpMatching2RoomDataInternal *roomDataInternal ATTRIBUTE_PRXPTR; } SceNpMatching2JoinRoomResponse;
typedef struct SceNpMatching2LeaveRoomRequest { SceNpMatching2RoomId roomId; SceNpMatching2PresenceOptionData optData; uint8_t padding[4]; } SceNpMatching2LeaveRoomRequest;
typedef struct SceNpMatching2GrantRoomOwnerRequest { SceNpMatching2RoomId roomId; SceNpMatching2RoomMemberId newOwner; uint8_t padding[2]; SceNpMatching2PresenceOptionData optData; } SceNpMatching2GrantRoomOwnerRequest;
typedef struct SceNpMatching2KickoutRoomMemberRequest { SceNpMatching2RoomId roomId; SceNpMatching2RoomMemberId target; SceNpMatching2BlockKickFlag blockKickFlag; uint8_t padding[1]; SceNpMatching2PresenceOptionData optData; } SceNpMatching2KickoutRoomMemberRequest;

typedef struct SceNpMatching2SearchRoomRequest {
    int32_t option;
    SceNpMatching2WorldId worldId;
    SceNpMatching2LobbyId lobbyId;
    SceNpMatching2RangeFilter rangeFilter;
    SceNpMatching2FlagAttr flagFilter;
    SceNpMatching2FlagAttr flagAttr;
    void *intFilter ATTRIBUTE_PRXPTR;
    uint32_t intFilterNum;
    void *binFilter ATTRIBUTE_PRXPTR;
    uint32_t binFilterNum;
    SceNpMatching2AttributeId *attrId ATTRIBUTE_PRXPTR;
    uint32_t attrIdNum;
} SceNpMatching2SearchRoomRequest;

typedef struct SceNpMatching2SearchRoomResponse { SceNpMatching2Range range; SceNpMatching2RoomDataExternal *roomDataExternal ATTRIBUTE_PRXPTR; } SceNpMatching2SearchRoomResponse;
typedef struct SceNpMatching2SendRoomMessageRequest { SceNpMatching2RoomId roomId; SceNpMatching2CastType castType; uint8_t padding[3]; SceNpMatching2RoomMessageDestination dst; const void *msg ATTRIBUTE_PRXPTR; uint32_t msgLen; int32_t option; } SceNpMatching2SendRoomMessageRequest;
typedef SceNpMatching2SendRoomMessageRequest SceNpMatching2SendRoomChatMessageRequest;
typedef struct SceNpMatching2SendRoomChatMessageResponse { uint8_t filtered; } SceNpMatching2SendRoomChatMessageResponse;
typedef struct SceNpMatching2SetRoomDataInternalRequest { SceNpMatching2RoomId roomId; SceNpMatching2FlagAttr flagFilter; SceNpMatching2FlagAttr flagAttr; SceNpMatching2BinAttr *roomBinAttrInternal ATTRIBUTE_PRXPTR; uint32_t roomBinAttrInternalNum; void *passwordConfig ATTRIBUTE_PRXPTR; uint32_t passwordConfigNum; SceNpMatching2RoomPasswordSlotMask *passwordSlotMask ATTRIBUTE_PRXPTR; SceNpMatching2RoomMemberId *ownerPrivilegeRank ATTRIBUTE_PRXPTR; uint32_t ownerPrivilegeRankNum; uint8_t padding[4]; } SceNpMatching2SetRoomDataInternalRequest;
typedef struct SceNpMatching2GetRoomDataInternalRequest { SceNpMatching2RoomId roomId; const SceNpMatching2AttributeId *attrId ATTRIBUTE_PRXPTR; uint32_t attrIdNum; } SceNpMatching2GetRoomDataInternalRequest;
typedef struct SceNpMatching2GetRoomDataInternalResponse { SceNpMatching2RoomDataInternal *roomDataInternal ATTRIBUTE_PRXPTR; } SceNpMatching2GetRoomDataInternalResponse;
typedef struct SceNpMatching2SetRoomMemberDataInternalRequest { SceNpMatching2RoomId roomId; SceNpMatching2RoomMemberId memberId; SceNpMatching2TeamId teamId; uint8_t padding[5]; SceNpMatching2FlagAttr flagFilter; SceNpMatching2FlagAttr flagAttr; SceNpMatching2BinAttr *roomMemberBinAttrInternal ATTRIBUTE_PRXPTR; uint32_t roomMemberBinAttrInternalNum; } SceNpMatching2SetRoomMemberDataInternalRequest;
typedef struct SceNpMatching2GetRoomMemberDataInternalRequest { SceNpMatching2RoomId roomId; SceNpMatching2RoomMemberId memberId; uint8_t padding[6]; const SceNpMatching2AttributeId *attrId ATTRIBUTE_PRXPTR; uint32_t attrIdNum; } SceNpMatching2GetRoomMemberDataInternalRequest;
typedef struct SceNpMatching2GetRoomMemberDataInternalResponse { SceNpMatching2RoomMemberDataInternal *roomMemberDataInternal ATTRIBUTE_PRXPTR; } SceNpMatching2GetRoomMemberDataInternalResponse;
typedef struct SceNpMatching2SetSignalingOptParamRequest { SceNpMatching2RoomId roomId; SceNpMatching2SignalingOptParam sigOptParam; } SceNpMatching2SetSignalingOptParamRequest;
typedef struct SceNpMatching2GetLobbyInfoListRequest { SceNpMatching2WorldId worldId; SceNpMatching2RangeFilter rangeFilter; SceNpMatching2AttributeId *attrId ATTRIBUTE_PRXPTR; uint32_t attrIdNum; } SceNpMatching2GetLobbyInfoListRequest;
typedef struct SceNpMatching2GetLobbyInfoListResponse { SceNpMatching2Range range; void *lobbyDataExternal ATTRIBUTE_PRXPTR; } SceNpMatching2GetLobbyInfoListResponse;
typedef struct SceNpMatching2JoinLobbyRequest { SceNpMatching2LobbyId lobbyId; SceNpMatching2JoinedSessionInfo *joinedSessionInfo ATTRIBUTE_PRXPTR; uint32_t joinedSessionInfoNum; SceNpMatching2BinAttr *lobbyMemberBinAttrInternal ATTRIBUTE_PRXPTR; uint32_t lobbyMemberBinAttrInternalNum; SceNpMatching2PresenceOptionData optData; uint8_t padding[4]; } SceNpMatching2JoinLobbyRequest;
typedef struct SceNpMatching2JoinLobbyResponse { void *lobbyDataInternal ATTRIBUTE_PRXPTR; } SceNpMatching2JoinLobbyResponse;
typedef struct SceNpMatching2LeaveLobbyRequest { SceNpMatching2LobbyId lobbyId; SceNpMatching2PresenceOptionData optData; uint8_t padding[4]; } SceNpMatching2LeaveLobbyRequest;
typedef struct SceNpMatching2SendLobbyChatMessageRequest { SceNpMatching2LobbyId lobbyId; SceNpMatching2CastType castType; uint8_t padding[3]; SceNpMatching2LobbyMessageDestination dst; const void *msg ATTRIBUTE_PRXPTR; uint32_t msgLen; int32_t option; } SceNpMatching2SendLobbyChatMessageRequest;
typedef struct SceNpMatching2SendLobbyChatMessageResponse { uint8_t filtered; } SceNpMatching2SendLobbyChatMessageResponse;
typedef struct SceNpMatching2SendLobbyInvitationRequest { SceNpMatching2LobbyId lobbyId; SceNpMatching2CastType castType; uint8_t padding[3]; SceNpMatching2LobbyMessageDestination dst; SceNpMatching2InvitationData invitationData; int32_t option; } SceNpMatching2SendLobbyInvitationRequest;
typedef struct SceNpMatching2SetLobbyMemberDataInternalRequest { SceNpMatching2LobbyId lobbyId; SceNpMatching2LobbyMemberId memberId; uint8_t padding1[2]; SceNpMatching2FlagAttr flagFilter; SceNpMatching2FlagAttr flagAttr; SceNpMatching2JoinedSessionInfo *joinedSessionInfo ATTRIBUTE_PRXPTR; uint32_t joinedSessionInfoNum; SceNpMatching2BinAttr *lobbyMemberBinAttrInternal ATTRIBUTE_PRXPTR; uint32_t lobbyMemberBinAttrInternalNum; uint8_t padding2[4]; } SceNpMatching2SetLobbyMemberDataInternalRequest;
typedef struct SceNpMatching2GetLobbyMemberDataInternalRequest { SceNpMatching2LobbyId lobbyId; SceNpMatching2LobbyMemberId memberId; uint8_t padding[6]; const SceNpMatching2AttributeId *attrId ATTRIBUTE_PRXPTR; uint32_t attrIdNum; } SceNpMatching2GetLobbyMemberDataInternalRequest;
typedef struct SceNpMatching2GetLobbyMemberDataInternalResponse { void *lobbyMemberDataInternal ATTRIBUTE_PRXPTR; } SceNpMatching2GetLobbyMemberDataInternalResponse;
typedef struct SceNpMatching2GetLobbyMemberDataInternalListRequest { SceNpMatching2LobbyId lobbyId; SceNpMatching2LobbyMemberId *memberId ATTRIBUTE_PRXPTR; uint32_t memberIdNum; const SceNpMatching2AttributeId *attrId ATTRIBUTE_PRXPTR; uint32_t attrIdNum; uint8_t extendedData; uint8_t padding[7]; } SceNpMatching2GetLobbyMemberDataInternalListRequest;
typedef struct SceNpMatching2GetLobbyMemberDataInternalListResponse { void *lobbyMemberDataInternal ATTRIBUTE_PRXPTR; uint32_t lobbyMemberDataInternalNum; } SceNpMatching2GetLobbyMemberDataInternalListResponse;
typedef struct SceNpMatching2SignalingGetPingInfoRequest { SceNpMatching2RoomId roomId; uint8_t reserved[16]; } SceNpMatching2SignalingGetPingInfoRequest;
typedef struct SceNpMatching2SignalingGetPingInfoResponse { SceNpMatching2ServerId serverId; uint8_t padding1[2]; SceNpMatching2WorldId worldId; SceNpMatching2RoomId roomId; uint32_t rtt; uint8_t reserved[20]; } SceNpMatching2SignalingGetPingInfoResponse;
typedef struct SceNpMatching2JoinProhibitiveRoomRequest { SceNpMatching2JoinRoomRequest joinParam; SceNpId *blockedUser ATTRIBUTE_PRXPTR; uint32_t blockedUserNum; } SceNpMatching2JoinProhibitiveRoomRequest;

int sceNpMatching2Init(uint32_t stackSize, int32_t priority);
int sceNpMatching2Init2(uint32_t stackSize, int32_t priority, SceNpMatching2UtilityInitParam *param);
int sceNpMatching2Term(void);
int sceNpMatching2Term2(void);
int sceNpMatching2CreateContext(const SceNpId *npId, const SceNpCommunicationId *commId, const SceNpCommunicationPassphrase *passPhrase, SceNpMatching2ContextId *ctxId, int32_t option);
int sceNpMatching2DestroyContext(SceNpMatching2ContextId ctxId);
int sceNpMatching2ContextStart(SceNpMatching2ContextId ctxId);
int sceNpMatching2ContextStartAsync(SceNpMatching2ContextId ctxId, uint32_t timeout);
int sceNpMatching2AbortContextStart(SceNpMatching2ContextId ctxId);
int sceNpMatching2ContextStop(SceNpMatching2ContextId ctxId);
int sceNpMatching2AbortRequest(SceNpMatching2ContextId ctxId, SceNpMatching2RequestId reqId);
int sceNpMatching2ClearEventData(SceNpMatching2ContextId ctxId, SceNpMatching2EventKey eventKey);
int sceNpMatching2GetEventData(SceNpMatching2ContextId ctxId, SceNpMatching2EventKey eventKey, void *buf, uint32_t bufLen);
int sceNpMatching2SetDefaultRequestOptParam(SceNpMatching2ContextId ctxId, const SceNpMatching2RequestOptParam *optParam);
int sceNpMatching2GetMemoryInfo(SceNpMatching2MemoryInfo *memInfo);
int sceNpMatching2GetCbQueueInfo(SceNpMatching2ContextId ctxId, SceNpMatching2CbQueueInfo *queueInfo);
int sceNpMatching2RegisterContextCallback(SceNpMatching2ContextId ctxId, SceNpMatching2ContextCallback cbFunc, void *cbFuncArg);
int sceNpMatching2RegisterLobbyEventCallback(SceNpMatching2ContextId ctxId, SceNpMatching2LobbyEventCallback cbFunc, void *cbFuncArg);
int sceNpMatching2RegisterLobbyMessageCallback(SceNpMatching2ContextId ctxId, SceNpMatching2LobbyMessageCallback cbFunc, void *cbFuncArg);
int sceNpMatching2RegisterRoomEventCallback(SceNpMatching2ContextId ctxId, SceNpMatching2RoomEventCallback cbFunc, void *cbFuncArg);
int sceNpMatching2RegisterRoomMessageCallback(SceNpMatching2ContextId ctxId, SceNpMatching2RoomMessageCallback cbFunc, void *cbFuncArg);
int sceNpMatching2RegisterSignalingCallback(SceNpMatching2ContextId ctxId, SceNpMatching2SignalingCallback cbFunc, void *cbFuncArg);
int sceNpMatching2CreateServerContext(SceNpMatching2ContextId ctxId, const SceNpMatching2CreateServerContextRequest *reqParam, const SceNpMatching2RequestOptParam *optParam, SceNpMatching2RequestId *assignedReqId);
int sceNpMatching2DeleteServerContext(SceNpMatching2ContextId ctxId, const SceNpMatching2DeleteServerContextRequest *reqParam, const SceNpMatching2RequestOptParam *optParam, SceNpMatching2RequestId *assignedReqId);
int sceNpMatching2GetServerInfo(SceNpMatching2ContextId ctxId, const SceNpMatching2GetServerInfoRequest *reqParam, const SceNpMatching2RequestOptParam *optParam, SceNpMatching2RequestId *assignedReqId);
int sceNpMatching2GetWorldInfoList(SceNpMatching2ContextId ctxId, const SceNpMatching2GetWorldInfoListRequest *reqParam, const SceNpMatching2RequestOptParam *optParam, SceNpMatching2RequestId *assignedReqId);
int sceNpMatching2SetUserInfo(SceNpMatching2ContextId ctxId, const SceNpMatching2SetUserInfoRequest *reqParam, const SceNpMatching2RequestOptParam *optParam, SceNpMatching2RequestId *assignedReqId);
int sceNpMatching2GetUserInfoList(SceNpMatching2ContextId ctxId, const SceNpMatching2GetUserInfoListRequest *reqParam, const SceNpMatching2RequestOptParam *optParam, SceNpMatching2RequestId *assignedReqId);
int sceNpMatching2GetClanLobbyId(SceNpMatching2ContextId ctxId, SceNpClanId clanId, SceNpMatching2LobbyId *lobbyId);
int sceNpMatching2CreateJoinRoom(SceNpMatching2ContextId ctxId, const SceNpMatching2CreateJoinRoomRequest *reqParam, const SceNpMatching2RequestOptParam *optParam, SceNpMatching2RequestId *assignedReqId);
int sceNpMatching2JoinRoom(SceNpMatching2ContextId ctxId, const SceNpMatching2JoinRoomRequest *reqParam, const SceNpMatching2RequestOptParam *optParam, SceNpMatching2RequestId *assignedReqId);
int sceNpMatching2JoinProhibitiveRoom(SceNpMatching2ContextId ctxId, const SceNpMatching2JoinProhibitiveRoomRequest *reqParam, const SceNpMatching2RequestOptParam *optParam, SceNpMatching2RequestId *assignedReqId);
int sceNpMatching2LeaveRoom(SceNpMatching2ContextId ctxId, const SceNpMatching2LeaveRoomRequest *reqParam, const SceNpMatching2RequestOptParam *optParam, SceNpMatching2RequestId *assignedReqId);
int sceNpMatching2GrantRoomOwner(SceNpMatching2ContextId ctxId, const SceNpMatching2GrantRoomOwnerRequest *reqParam, const SceNpMatching2RequestOptParam *optParam, SceNpMatching2RequestId *assignedReqId);
int sceNpMatching2KickoutRoomMember(SceNpMatching2ContextId ctxId, const SceNpMatching2KickoutRoomMemberRequest *reqParam, const SceNpMatching2RequestOptParam *optParam, SceNpMatching2RequestId *assignedReqId);
int sceNpMatching2SearchRoom(SceNpMatching2ContextId ctxId, const SceNpMatching2SearchRoomRequest *reqParam, const SceNpMatching2RequestOptParam *optParam, SceNpMatching2RequestId *assignedReqId);
int sceNpMatching2SendRoomMessage(SceNpMatching2ContextId ctxId, const SceNpMatching2SendRoomMessageRequest *reqParam, const SceNpMatching2RequestOptParam *optParam, SceNpMatching2RequestId *assignedReqId);
int sceNpMatching2SendRoomChatMessage(SceNpMatching2ContextId ctxId, const SceNpMatching2SendRoomChatMessageRequest *reqParam, const SceNpMatching2RequestOptParam *optParam, SceNpMatching2RequestId *assignedReqId);
int sceNpMatching2SetRoomDataExternal(SceNpMatching2ContextId ctxId, const SceNpMatching2SetRoomDataExternalRequest *reqParam, const SceNpMatching2RequestOptParam *optParam, SceNpMatching2RequestId *assignedReqId);
int sceNpMatching2GetRoomDataExternalList(SceNpMatching2ContextId ctxId, const SceNpMatching2GetRoomDataExternalListRequest *reqParam, const SceNpMatching2RequestOptParam *optParam, SceNpMatching2RequestId *assignedReqId);
int sceNpMatching2SetRoomDataInternal(SceNpMatching2ContextId ctxId, const SceNpMatching2SetRoomDataInternalRequest *reqParam, const SceNpMatching2RequestOptParam *optParam, SceNpMatching2RequestId *assignedReqId);
int sceNpMatching2GetRoomDataInternal(SceNpMatching2ContextId ctxId, const SceNpMatching2GetRoomDataInternalRequest *reqParam, const SceNpMatching2RequestOptParam *optParam, SceNpMatching2RequestId *assignedReqId);
int sceNpMatching2SetRoomMemberDataInternal(SceNpMatching2ContextId ctxId, const SceNpMatching2SetRoomMemberDataInternalRequest *reqParam, const SceNpMatching2RequestOptParam *optParam, SceNpMatching2RequestId *assignedReqId);
int sceNpMatching2GetRoomMemberDataInternal(SceNpMatching2ContextId ctxId, const SceNpMatching2GetRoomMemberDataInternalRequest *reqParam, const SceNpMatching2RequestOptParam *optParam, SceNpMatching2RequestId *assignedReqId);
int sceNpMatching2GetRoomMemberDataInternalLocal(SceNpMatching2ContextId ctxId, SceNpMatching2RoomId roomId, SceNpMatching2RoomMemberId memberId, const SceNpMatching2AttributeId *attrId, uint32_t attrIdNum, SceNpMatching2RoomMemberDataInternal *member, char *buf, uint32_t bufLen);
int sceNpMatching2GetRoomMemberDataExternalList(SceNpMatching2ContextId ctxId, const SceNpMatching2GetRoomMemberDataExternalListRequest *reqParam, const SceNpMatching2RequestOptParam *optParam, SceNpMatching2RequestId *assignedReqId);
int sceNpMatching2GetRoomMemberIdListLocal(SceNpMatching2ContextId ctxId, SceNpMatching2RoomId roomId, int32_t sortMethod, SceNpMatching2RoomMemberId *memberId, uint32_t memberIdNum);
int sceNpMatching2GetRoomPasswordLocal(SceNpMatching2ContextId ctxId, SceNpMatching2RoomId roomId, uint8_t *withPassword, SceNpMatching2SessionPassword *roomPassword);
int sceNpMatching2GetRoomSlotInfoLocal(SceNpMatching2ContextId ctxId, SceNpMatching2RoomId roomId, SceNpMatching2RoomSlotInfo *roomSlotInfo);
int sceNpMatching2JoinLobby(SceNpMatching2ContextId ctxId, const SceNpMatching2JoinLobbyRequest *reqParam, const SceNpMatching2RequestOptParam *optParam, SceNpMatching2RequestId *assignedReqId);
int sceNpMatching2LeaveLobby(SceNpMatching2ContextId ctxId, const SceNpMatching2LeaveLobbyRequest *reqParam, const SceNpMatching2RequestOptParam *optParam, SceNpMatching2RequestId *assignedReqId);
int sceNpMatching2SendLobbyChatMessage(SceNpMatching2ContextId ctxId, const SceNpMatching2SendLobbyChatMessageRequest *reqParam, const SceNpMatching2RequestOptParam *optParam, SceNpMatching2RequestId *assignedReqId);
int sceNpMatching2SendLobbyInvitation(SceNpMatching2ContextId ctxId, const SceNpMatching2SendLobbyInvitationRequest *reqParam, const SceNpMatching2RequestOptParam *optParam, SceNpMatching2RequestId *assignedReqId);
int sceNpMatching2SetLobbyMemberDataInternal(SceNpMatching2ContextId ctxId, const SceNpMatching2SetLobbyMemberDataInternalRequest *reqParam, const SceNpMatching2RequestOptParam *optParam, SceNpMatching2RequestId *assignedReqId);
int sceNpMatching2GetLobbyMemberDataInternal(SceNpMatching2ContextId ctxId, const SceNpMatching2GetLobbyMemberDataInternalRequest *reqParam, const SceNpMatching2RequestOptParam *optParam, SceNpMatching2RequestId *assignedReqId);
int sceNpMatching2GetLobbyMemberDataInternalList(SceNpMatching2ContextId ctxId, const SceNpMatching2GetLobbyMemberDataInternalListRequest *reqParam, const SceNpMatching2RequestOptParam *optParam, SceNpMatching2RequestId *assignedReqId);
int sceNpMatching2GetLobbyInfoList(SceNpMatching2ContextId ctxId, const SceNpMatching2GetLobbyInfoListRequest *reqParam, const SceNpMatching2RequestOptParam *optParam, SceNpMatching2RequestId *assignedReqId);
int sceNpMatching2GetLobbyMemberIdListLocal(SceNpMatching2ContextId ctxId, SceNpMatching2LobbyId lobbyId, SceNpMatching2LobbyMemberId *memberId, uint32_t memberIdNum, SceNpMatching2LobbyMemberId *me);
int sceNpMatching2GetServerIdListLocal(SceNpMatching2ContextId ctxId, SceNpMatching2ServerId *serverId, uint32_t serverIdNum);
int sceNpMatching2SetSignalingOptParam(SceNpMatching2ContextId ctxId, const SceNpMatching2SetSignalingOptParamRequest *reqParam, const SceNpMatching2RequestOptParam *optParam, SceNpMatching2RequestId *assignedReqId);
int sceNpMatching2GetSignalingOptParamLocal(SceNpMatching2ContextId ctxId, SceNpMatching2RoomId roomId, SceNpMatching2SignalingOptParam *signalingOptParam);
int sceNpMatching2SignalingGetPingInfo(SceNpMatching2ContextId ctxId, const SceNpMatching2SignalingGetPingInfoRequest *reqParam, const SceNpMatching2RequestOptParam *optParam, SceNpMatching2RequestId *assignedReqId);
int sceNpMatching2SignalingGetConnectionStatus(SceNpMatching2ContextId ctxId, SceNpMatching2RoomId roomId, SceNpMatching2RoomMemberId memberId, int32_t *connStatus, struct in_addr *peerAddr, in_port_t *peerPort);
int sceNpMatching2SignalingGetConnectionInfo(SceNpMatching2ContextId ctxId, SceNpMatching2RoomId roomId, SceNpMatching2RoomMemberId memberId, int32_t code, SceNpSignalingConnectionInfo *connInfo);
int sceNpMatching2SignalingGetCtxOpt(SceNpMatching2ContextId ctxId, int32_t optname, int32_t *optval);
int sceNpMatching2SignalingSetCtxOpt(SceNpMatching2ContextId ctxId, int32_t optname, int32_t optval);
int sceNpMatching2SignalingGetLocalNetInfo(SceNpMatching2SignalingNetInfo *netinfo);
int sceNpMatching2SignalingGetPeerNetInfo(SceNpMatching2ContextId ctxId, SceNpMatching2RoomId roomId, SceNpMatching2RoomMemberId roomMemberId, SceNpMatching2SignalingRequestId *reqId);
int sceNpMatching2SignalingGetPeerNetInfoResult(SceNpMatching2ContextId ctxId, SceNpMatching2SignalingRequestId reqId, SceNpMatching2SignalingNetInfo *netinfo);
int sceNpMatching2SignalingCancelPeerNetInfo(SceNpMatching2ContextId ctxId, SceNpMatching2SignalingRequestId reqId);

#ifdef __cplusplus
}
#endif

#endif /* __PS3DK_CELL_NP2_MATCHING2_H__ */
