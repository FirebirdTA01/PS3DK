#ifndef __PS3DK_CELL_NP_BASIC_H__
#define __PS3DK_CELL_NP_BASIC_H__

#include <cell/np/common.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/memory.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SCE_NP_BASIC_MAX_MESSAGE_SIZE 512
#define SCE_NP_BASIC_MAX_PRESENCE_SIZE 128
#define SCE_NP_BASIC_MAX_MESSAGE_ATTACHMENT_SIZE (1024 * 1024)
#define SCE_NP_BASIC_SUBJECT_CHARACTER_MAX 18
#define SCE_NP_BASIC_BODY_CHARACTER_MAX 512
#define SCE_NP_BASIC_DESCRIPTION_CHARACTER_MAX 341
#define SCE_NP_BASIC_SEND_MESSAGE_MAX_RECIPIENTS 12
#define SCE_NP_BASIC_PRESENCE_TITLE_SIZE_MAX 128
#define SCE_NP_BASIC_PRESENCE_STATUS_SIZE_MAX 64
#define SCE_NP_BASIC_PRESENCE_STATUS_CHARACTER_MAX 21
#define SCE_NP_BASIC_PRESENCE_EXTENDED_STATUS_SIZE_MAX 192
#define SCE_NP_BASIC_PRESENCE_EXTENDED_STATUS_CHARACTER_MAX 63
#define SCE_NP_BASIC_PRESENCE_COMMENT_SIZE_MAX 64
#define SCE_NP_BASIC_MAX_INVITATION_DATA_SIZE (1 * 1024)
#define SCE_NP_BASIC_MAX_URL_ATTACHMENT_SIZE (2 * 1024)
#define SCE_NP_BASIC_PLAYER_HISTORY_MAX_PLAYERS 8

#define SCE_NP_BASIC_EVENT_UNKNOWN -1
#define SCE_NP_BASIC_EVENT_OFFLINE 0
#define SCE_NP_BASIC_EVENT_PRESENCE 1
#define SCE_NP_BASIC_EVENT_MESSAGE 2
#define SCE_NP_BASIC_EVENT_ADD_FRIEND_RESULT 3
#define SCE_NP_BASIC_EVENT_INCOMING_ATTACHMENT 4
#define SCE_NP_BASIC_EVENT_INCOMING_INVITATION 5
#define SCE_NP_BASIC_EVENT_END_OF_INITIAL_PRESENCE 6
#define SCE_NP_BASIC_EVENT_SEND_ATTACHMENT_RESULT 7
#define SCE_NP_BASIC_EVENT_RECV_ATTACHMENT_RESULT 8
#define SCE_NP_BASIC_EVENT_OUT_OF_CONTEXT 9
#define SCE_NP_BASIC_EVENT_FRIEND_REMOVED 10
#define SCE_NP_BASIC_EVENT_ADD_BLOCKLIST_RESULT 11
#define SCE_NP_BASIC_EVENT_SEND_MESSAGE_RESULT 12
#define SCE_NP_BASIC_EVENT_SEND_INVITATION_RESULT 13
#define SCE_NP_BASIC_EVENT_RECV_INVITATION_RESULT 14
#define SCE_NP_BASIC_EVENT_MESSAGE_MARKED_AS_USED_RESULT 15
#define SCE_NP_BASIC_EVENT_INCOMING_CUSTOM_INVITATION 16
#define SCE_NP_BASIC_EVENT_INCOMING_CLAN_MESSAGE 17
#define SCE_NP_BASIC_EVENT_ADD_PLAYERS_HISTORY_RESULT 18
#define SCE_NP_BASIC_EVENT_SEND_CUSTOM_DATA_RESULT 19
#define SCE_NP_BASIC_EVENT_RECV_CUSTOM_DATA_RESULT 20
#define SCE_NP_BASIC_EVENT_INCOMING_CUSTOM_DATA_MESSAGE 21
#define SCE_NP_BASIC_EVENT_SEND_URL_ATTACHMENT_RESULT 22
#define SCE_NP_BASIC_EVENT_INCOMING_BOOTABLE_INVITATION 23
#define SCE_NP_BASIC_EVENT_BLOCKLIST_UPDATE 24
#define SCE_NP_BASIC_EVENT_INCOMING_BOOTABLE_CUSTOM_DATA_MESSAGE 25

#define SCE_NP_BASIC_INVALID_ATTACHMENT_DATA_ID 0
#define SCE_NP_BASIC_SELECTED_INVITATION_DATA 1
#define SCE_NP_BASIC_SELECTED_MESSAGE_DATA 2
#define SCE_NP_BASIC_INVALID_MESSAGE_ID 0

#define SCE_NP_BASIC_PLAYERS_HISTORY_OPTIONS_BY_NP_COMM_ID 0
#define SCE_NP_BASIC_PLAYERS_HISTORY_OPTIONS_ALL 1

#define SCE_NP_BASIC_PRESENCE_STATE_OFFLINE 0
#define SCE_NP_BASIC_PRESENCE_STATE_OUT_OF_CONTEXT 1
#define SCE_NP_BASIC_PRESENCE_STATE_IN_CONTEXT 2

#define SCE_NP_BASIC_PRESENCE_OPTIONS_SET_DATA 0x00000001
#define SCE_NP_BASIC_PRESENCE_OPTIONS_SET_STATUS 0x00000002
#define SCE_NP_BASIC_PRESENCE_OPTIONS_ALL_OPTIONS \
    (SCE_NP_BASIC_PRESENCE_OPTIONS_SET_DATA | SCE_NP_BASIC_PRESENCE_OPTIONS_SET_STATUS)

#define SCE_NP_BASIC_MESSAGE_MAIN_TYPE_DATA_ATTACHMENT 0x0000
#define SCE_NP_BASIC_MESSAGE_MAIN_TYPE_GENERAL 0x0001
#define SCE_NP_BASIC_MESSAGE_MAIN_TYPE_ADD_FRIEND 0x0002
#define SCE_NP_BASIC_MESSAGE_MAIN_TYPE_INVITE 0x0003
#define SCE_NP_BASIC_MESSAGE_MAIN_TYPE_CUSTOM_DATA 0x0004
#define SCE_NP_BASIC_MESSAGE_MAIN_TYPE_URL_ATTACHMENT 0x0005

#define SCE_NP_BASIC_MESSAGE_DATA_ATTACHMENT_SUBTYPE_ACTION_USE 0x0000
#define SCE_NP_BASIC_MESSAGE_GENERAL_SUBTYPE_NONE 0x0000
#define SCE_NP_BASIC_MESSAGE_ADD_FRIEND_SUBTYPE_NONE 0x0000
#define SCE_NP_BASIC_MESSAGE_INVITE_SUBTYPE_ACTION_ACCEPT_DENY 0x0000
#define SCE_NP_BASIC_MESSAGE_INVITE_SUBTYPE_ACTION_ACCEPT 0x0001
#define SCE_NP_BASIC_MESSAGE_CUSTOM_DATA_SUBTYPE_ACTION_USE 0x0000
#define SCE_NP_BASIC_MESSAGE_URL_ATTACHMENT_SUBTYPE_ACTION_USE 0x0000

#define SCE_NP_BASIC_MESSAGE_FEATURES_MULTI_RECEIPIENTS 0x00000001
#define SCE_NP_BASIC_MESSAGE_FEATURES_BOOTABLE 0x00000002
#define SCE_NP_BASIC_MESSAGE_FEATURES_ASSUME_SEND 0x00000004
#define SCE_NP_BASIC_MESSAGE_FEATURES_EXP_MIN(min) ((((uint32_t)(min) << 16) | (0U << 15)) & 0xffff8000U)
#define SCE_NP_BASIC_MESSAGE_FEATURES_ALL_FEATURES \
    (SCE_NP_BASIC_MESSAGE_FEATURES_MULTI_RECEIPIENTS | \
     SCE_NP_BASIC_MESSAGE_FEATURES_BOOTABLE | \
     SCE_NP_BASIC_MESSAGE_FEATURES_ASSUME_SEND | \
     SCE_NP_BASIC_MESSAGE_FEATURES_EXP_MIN(0xffffU))

#define SCE_NP_BASIC_MESSAGE_ACTION_UNKNOWN 0
#define SCE_NP_BASIC_MESSAGE_ACTION_USE 1
#define SCE_NP_BASIC_MESSAGE_ACTION_ACCEPT 2
#define SCE_NP_BASIC_MESSAGE_ACTION_DENY 3

#define SCE_NP_BASIC_RECV_MESSAGE_OPTIONS_PRESERVE 0x00000001
#define SCE_NP_BASIC_RECV_MESSAGE_OPTIONS_INCLUDE_BOOTABLE 0x00000002
#define SCE_NP_BASIC_RECV_MESSAGE_OPTIONS_ASSUME_LATEST 0x00000004
#define SCE_NP_BASIC_RECV_MESSAGE_OPTIONS_ALL_OPTIONS \
    (SCE_NP_BASIC_RECV_MESSAGE_OPTIONS_PRESERVE | \
     SCE_NP_BASIC_RECV_MESSAGE_OPTIONS_ASSUME_LATEST | \
     SCE_NP_BASIC_RECV_MESSAGE_OPTIONS_INCLUDE_BOOTABLE)

#define SCE_NP_BASIC_MESSAGE_INFO_TYPE_MESSAGE_ATTACHMENT 0
#define SCE_NP_BASIC_MESSAGE_INFO_TYPE_MATCHING_INVITATION 1
#define SCE_NP_BASIC_MESSAGE_INFO_TYPE_CUSTOM_INVITATION 2
#define SCE_NP_BASIC_MESSAGE_INFO_TYPE_CLAN_MESSAGE 3
#define SCE_NP_BASIC_MESSAGE_INFO_TYPE_CUSTOM_DATA_MESSAGE 4
#define SCE_NP_BASIC_MESSAGE_INFO_TYPE_ANY_UNREAD_MESSAGE 5
#define SCE_NP_BASIC_MESSAGE_INFO_TYPE_BOOTABLE_INVITATION 6
#define SCE_NP_BASIC_MESSAGE_INFO_TYPE_BOOTABLE_CUSTOM_DATA_MESSAGE 7

typedef uint64_t SceNpBasicMessageId;
typedef uint16_t SceNpBasicMessageMainType;
typedef uint16_t SceNpBasicMessageSubType;
typedef uint32_t SceNpBasicMessageFeatures;
typedef uint32_t SceNpBasicMessageRecvOptions;
typedef uint32_t SceNpBasicMessageRecvAction;
typedef uint32_t SceNpBasicMessageInfoType;

typedef struct SceNpBasicMessageDetails {
    SceNpBasicMessageId msgId;
    SceNpBasicMessageMainType mainType;
    SceNpBasicMessageSubType subType;
    SceNpBasicMessageFeatures msgFeatures;
    const SceNpId *npids ATTRIBUTE_PRXPTR;
    uint32_t count;
    const char *subject ATTRIBUTE_PRXPTR;
    const char *body ATTRIBUTE_PRXPTR;
    const void *data ATTRIBUTE_PRXPTR;
    uint32_t size;
} SceNpBasicMessageDetails;

typedef uint32_t SceNpBasicAttachmentDataId;

typedef struct SceNpBasicAttachmentData {
    SceNpBasicAttachmentDataId id;
    uint32_t size;
} SceNpBasicAttachmentData;

typedef struct SceNpBasicExtendedAttachmentData {
    uint64_t flags;
    SceNpBasicMessageId msgId;
    SceNpBasicAttachmentData data;
    SceNpBasicMessageRecvAction userAction;
    bool markedAsUsed;
    uint8_t reserved[3];
} SceNpBasicExtendedAttachmentData;

typedef struct SceNpBasicPresenceDetails {
    char title[SCE_NP_BASIC_PRESENCE_TITLE_SIZE_MAX];
    char status[SCE_NP_BASIC_PRESENCE_STATUS_SIZE_MAX];
    char comment[SCE_NP_BASIC_PRESENCE_COMMENT_SIZE_MAX];
    uint8_t data[SCE_NP_BASIC_MAX_PRESENCE_SIZE];
    uint32_t size;
    int32_t state;
} SceNpBasicPresenceDetails;

typedef struct SceNpBasicPresenceDetails2 {
    uint32_t struct_size;
    int32_t state;
    char title[SCE_NP_BASIC_PRESENCE_TITLE_SIZE_MAX];
    char status[SCE_NP_BASIC_PRESENCE_EXTENDED_STATUS_SIZE_MAX];
    char comment[SCE_NP_BASIC_PRESENCE_COMMENT_SIZE_MAX];
    uint8_t data[SCE_NP_BASIC_MAX_PRESENCE_SIZE];
    uint32_t size;
} SceNpBasicPresenceDetails2;

typedef int (*SceNpBasicEventHandler)(int event, int retCode, uint32_t reqId, void *arg);

static inline int sceNpBasicInit(void) { return 0; }
static inline int sceNpBasicTerm(void) { return 0; }

int sceNpBasicRegisterHandler(const SceNpCommunicationId *context, SceNpBasicEventHandler handler, void *arg);
int sceNpBasicRegisterContextSensitiveHandler(const SceNpCommunicationId *context, SceNpBasicEventHandler handler, void *arg);
int sceNpBasicUnregisterHandler(void);
int sceNpBasicSetPresence(const void *data, size_t size);
int sceNpBasicSetPresenceDetails(const SceNpBasicPresenceDetails *pres, uint32_t options);
int sceNpBasicSetPresenceDetails2(const SceNpBasicPresenceDetails2 *pres, uint32_t options);
int sceNpBasicSendMessage(const SceNpId *to, const void *data, size_t size);
int sceNpBasicSendMessageGui(const SceNpBasicMessageDetails *msg, sys_memory_container_t containerId);
int sceNpBasicSendMessageAttachment(const SceNpId *to, const char *subject, const char *body, const void *data, size_t size, sys_memory_container_t containerId);
int sceNpBasicRecvMessageAttachment(sys_memory_container_t containerId);
int sceNpBasicRecvMessageAttachmentLoad(SceNpBasicAttachmentDataId id, void *buffer, size_t *size);
int sceNpBasicRecvMessageCustom(SceNpBasicMessageMainType mainType, SceNpBasicMessageRecvOptions recvOptions, sys_memory_container_t containerId);
int sceNpBasicMarkMessageAsUsed(SceNpBasicMessageId msgId);
int sceNpBasicAbortGui(void);
int sceNpBasicAddFriend(const SceNpId *contact, const char *body, sys_memory_container_t containerId);
int sceNpBasicGetFriendListEntryCount(uint32_t *count);
int sceNpBasicGetFriendListEntry(uint32_t index, SceNpId *npid);
int sceNpBasicGetFriendPresenceByIndex(uint32_t index, SceNpUserInfo *user, SceNpBasicPresenceDetails *pres, uint32_t options);
int sceNpBasicGetFriendPresenceByNpId(const SceNpId *npid, SceNpBasicPresenceDetails *pres, uint32_t options);
int sceNpBasicGetFriendPresenceByIndex2(uint32_t index, SceNpUserInfo *user, SceNpBasicPresenceDetails2 *pres, uint32_t options);
int sceNpBasicGetFriendPresenceByNpId2(const SceNpId *npid, SceNpBasicPresenceDetails2 *pres, uint32_t options);
int sceNpBasicAddPlayersHistory(const SceNpId *npid, const char *description);
int sceNpBasicAddPlayersHistoryAsync(const SceNpId *npids, size_t count, const char *description, uint32_t *reqId);
int sceNpBasicGetPlayersHistoryEntryCount(uint32_t options, uint32_t *count);
int sceNpBasicGetPlayersHistoryEntry(uint32_t options, uint32_t index, SceNpId *npid);
int sceNpBasicAddBlockListEntry(const SceNpId *user);
int sceNpBasicGetBlockListEntryCount(uint32_t *count);
int sceNpBasicGetBlockListEntry(uint32_t index, SceNpId *npid);
int sceNpBasicGetMessageAttachmentEntryCount(uint32_t *count);
int sceNpBasicGetMessageAttachmentEntry(uint32_t index, SceNpUserInfo *from);
int sceNpBasicGetMatchingInvitationEntryCount(uint32_t *count);
int sceNpBasicGetMatchingInvitationEntry(uint32_t index, SceNpUserInfo *from);
int sceNpBasicGetCustomInvitationEntryCount(uint32_t *count);
int sceNpBasicGetCustomInvitationEntry(uint32_t index, SceNpUserInfo *from);
int sceNpBasicGetClanMessageEntryCount(uint32_t *count);
int sceNpBasicGetClanMessageEntry(uint32_t index, SceNpUserInfo *from);
int sceNpBasicGetMessageEntryCount(SceNpBasicMessageInfoType type, uint32_t *count);
int sceNpBasicGetMessageEntry(SceNpBasicMessageInfoType type, uint32_t index, SceNpUserInfo *from);
int sceNpBasicGetEvent(int *event, SceNpUserInfo *from, void *data, size_t *size);

#ifdef __cplusplus
}
#endif

#endif /* __PS3DK_CELL_NP_BASIC_H__ */
