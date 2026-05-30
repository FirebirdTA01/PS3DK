#ifndef __PS3DK_CELL_NP_MATCHING_H__
#define __PS3DK_CELL_NP_MATCHING_H__

#include <cell/np/common.h>
#include <stdint.h>
#include <sys/memory.h>

#ifndef ATTRIBUTE_PRXPTR
#define ATTRIBUTE_PRXPTR
#endif

#ifdef __cplusplus
extern "C" {
#endif

#define SCE_NP_MATCHING_ATTR_TYPE_BASIC 1
#define SCE_NP_MATCHING_ATTR_TYPE_GAME 2

#define SCE_NP_MATCHING_ATTR_COMP_OP_EQ 0
#define SCE_NP_MATCHING_ATTR_COMP_OP_NE 1
#define SCE_NP_MATCHING_ATTR_COMP_OP_LT 2
#define SCE_NP_MATCHING_ATTR_COMP_OP_LE 3
#define SCE_NP_MATCHING_ATTR_COMP_OP_GT 4
#define SCE_NP_MATCHING_ATTR_COMP_OP_GE 5

#define SCE_NP_MATCHING_ATTR_COMP_TYPE_NUM 1
#define SCE_NP_MATCHING_ATTR_COMP_TYPE_BIN 2

#define SCE_NP_MATCHING_ROOM_SEARCH_FLAG_PUBLIC 0
#define SCE_NP_MATCHING_ROOM_SEARCH_FLAG_PRIVATE 1

#define SCE_NP_MATCHING_EVENT_ROOM_CREATED 1
#define SCE_NP_MATCHING_EVENT_ROOM_JOINED 2
#define SCE_NP_MATCHING_EVENT_ROOM_LEFT 3
#define SCE_NP_MATCHING_EVENT_ROOM_SEARCHED 4
#define SCE_NP_MATCHING_EVENT_ROOM_INFO_UPDATED 5
#define SCE_NP_MATCHING_EVENT_ROOM_OWNER_CHANGED 6
#define SCE_NP_MATCHING_EVENT_INVITATION_RECEIVED 7

typedef uint32_t SceNpMatchingCtxId;
typedef uint32_t SceNpMatchingRequestId;
typedef uint32_t SceNpMatchingAttributeId;

typedef void (*SceNpMatchingHandler)(uint32_t ctxId, uint32_t reqId, int32_t event, int32_t errorCode, void *arg);
typedef void (*SceNpMatchingGUIHandler)(uint32_t ctxId, int32_t event, int32_t errorCode, void *arg);

typedef struct SceNpMatchingAttr {
    struct SceNpMatchingAttr *next ATTRIBUTE_PRXPTR;
    int32_t type;
    SceNpMatchingAttributeId id;
    union {
        uint32_t num;
        struct {
            void *ptr ATTRIBUTE_PRXPTR;
            uint32_t size;
        } data;
    } value;
} SceNpMatchingAttr;

typedef struct SceNpMatchingSearchCondition {
    struct SceNpMatchingSearchCondition *next ATTRIBUTE_PRXPTR;
    int32_t targetAttrType;
    SceNpMatchingAttributeId targetAttrId;
    int32_t compOp;
    int32_t compType;
    SceNpMatchingAttr compared;
} SceNpMatchingSearchCondition;

typedef struct SceNpMatchingReqRange {
    uint32_t start;
    uint32_t max;
} SceNpMatchingReqRange;

typedef struct SceNpMatchingRoomMember {
    struct SceNpMatchingRoomMember *next ATTRIBUTE_PRXPTR;
    SceNpUserInfo userInfo;
    int32_t owner;
} SceNpMatchingRoomMember;

typedef struct SceNpMatchingRoomStatus {
    SceNpRoomId id;
    SceNpMatchingRoomMember *members ATTRIBUTE_PRXPTR;
    int32_t num;
    SceNpId *kickActor ATTRIBUTE_PRXPTR;
    void *opt ATTRIBUTE_PRXPTR;
    int32_t optLen;
} SceNpMatchingRoomStatus;

typedef struct SceNpMatchingJoinedRoomInfo {
    SceNpLobbyId lobbyId;
    SceNpMatchingRoomStatus roomStatus;
} SceNpMatchingJoinedRoomInfo;

typedef struct SceNpMatchingRange {
    uint32_t start;
    uint32_t results;
    uint32_t total;
} SceNpMatchingRange;

typedef struct SceNpMatchingRoom {
    struct SceNpMatchingRoom *next ATTRIBUTE_PRXPTR;
    SceNpRoomId id;
    SceNpMatchingAttr *attr ATTRIBUTE_PRXPTR;
} SceNpMatchingRoom;

typedef struct SceNpMatchingRoomList {
    SceNpLobbyId lobbyId;
    SceNpMatchingRange range;
    SceNpMatchingRoom *head ATTRIBUTE_PRXPTR;
} SceNpMatchingRoomList;

typedef struct SceNpMatchingSearchJoinRoomInfo {
    SceNpLobbyId lobbyId;
    SceNpMatchingRoomStatus roomStatus;
    SceNpMatchingAttr *attr ATTRIBUTE_PRXPTR;
} SceNpMatchingSearchJoinRoomInfo;

int sceNpMatchingCreateCtx(const SceNpId *npId, SceNpMatchingHandler handler, void *arg, SceNpMatchingCtxId *ctxId);
int sceNpMatchingDestroyCtx(SceNpMatchingCtxId ctxId);
int sceNpMatchingGetResult(SceNpMatchingCtxId ctxId, SceNpMatchingRequestId reqId, void *buf, uint32_t *size, int32_t *event);
int sceNpMatchingGetResultGUI(void *buf, uint32_t *size, int32_t *event);
int sceNpMatchingSetRoomInfo(SceNpMatchingCtxId ctxId, SceNpLobbyId *lobbyId, SceNpRoomId *roomId, SceNpMatchingAttr *attr, SceNpMatchingRequestId *reqId);
int sceNpMatchingSetRoomInfoNoLimit(SceNpMatchingCtxId ctxId, SceNpLobbyId *lobbyId, SceNpRoomId *roomId, SceNpMatchingAttr *attr, SceNpMatchingRequestId *reqId);
int sceNpMatchingGetRoomInfo(SceNpMatchingCtxId ctxId, SceNpLobbyId *lobbyId, SceNpRoomId *roomId, SceNpMatchingAttr *attr, SceNpMatchingRequestId *reqId);
int sceNpMatchingGetRoomInfoNoLimit(SceNpMatchingCtxId ctxId, SceNpLobbyId *lobbyId, SceNpRoomId *roomId, SceNpMatchingAttr *attr, SceNpMatchingRequestId *reqId);
int sceNpMatchingSetRoomSearchFlag(SceNpMatchingCtxId ctxId, SceNpLobbyId *lobbyId, SceNpRoomId *roomId, int32_t flag, SceNpMatchingRequestId *reqId);
int sceNpMatchingGetRoomSearchFlag(SceNpMatchingCtxId ctxId, SceNpLobbyId *lobbyId, SceNpRoomId *roomId, SceNpMatchingRequestId *reqId);
int sceNpMatchingGetRoomMemberListLocal(SceNpMatchingCtxId ctxId, SceNpRoomId *roomId, uint32_t *bufLen, void *buf);
int sceNpMatchingGetRoomListLimitGUI(SceNpMatchingCtxId ctxId, SceNpCommunicationId *communicationId, SceNpMatchingReqRange *range, SceNpMatchingSearchCondition *cond, SceNpMatchingAttr *attr, SceNpMatchingGUIHandler handler, void *arg);
int sceNpMatchingKickRoomMember(SceNpMatchingCtxId ctxId, const SceNpRoomId *roomId, const SceNpId *userId, SceNpMatchingRequestId *reqId);
int sceNpMatchingKickRoomMemberWithOpt(SceNpMatchingCtxId ctxId, const SceNpRoomId *roomId, const SceNpId *userId, const void *opt, int32_t optLen, SceNpMatchingRequestId *reqId);
int sceNpMatchingQuickMatchGUI(SceNpMatchingCtxId ctxId, const SceNpCommunicationId *communicationId, const SceNpMatchingSearchCondition *cond, int32_t availableNum, int32_t timeout, SceNpMatchingGUIHandler handler, void *arg);
int sceNpMatchingSendInvitationGUI(SceNpMatchingCtxId ctxId, const SceNpRoomId *roomId, const SceNpCommunicationId *communicationId, const SceNpId *dsts, int32_t num, int32_t slotType, const char *subject, const char *body, sys_memory_container_t container, SceNpMatchingGUIHandler handler, void *arg);
int sceNpMatchingAcceptInvitationGUI(SceNpMatchingCtxId ctxId, const SceNpCommunicationId *communicationId, sys_memory_container_t container, SceNpMatchingGUIHandler handler, void *arg);
int sceNpMatchingCreateRoomGUI(SceNpMatchingCtxId ctxId, const SceNpCommunicationId *communicationId, const SceNpMatchingAttr *attr, SceNpMatchingGUIHandler handler, void *arg);
int sceNpMatchingJoinRoomGUI(SceNpMatchingCtxId ctxId, SceNpRoomId *roomId, SceNpMatchingGUIHandler handler, void *arg);
int sceNpMatchingLeaveRoom(SceNpMatchingCtxId ctxId, const SceNpRoomId *roomId, SceNpMatchingRequestId *reqId);
int sceNpMatchingSearchJoinRoomGUI(SceNpMatchingCtxId ctxId, const SceNpCommunicationId *communicationId, const SceNpMatchingSearchCondition *cond, const SceNpMatchingAttr *attr, SceNpMatchingGUIHandler handler, void *arg);
int sceNpMatchingGrantOwnership(SceNpMatchingCtxId ctxId, const SceNpRoomId *roomId, const SceNpId *userId, SceNpMatchingRequestId *reqId);

static inline int sceNpMatchingInit(void) { return 0; }
static inline int sceNpMatchingTerm(void) { return 0; }

#ifdef __cplusplus
}
#endif

#endif /* __PS3DK_CELL_NP_MATCHING_H__ */
