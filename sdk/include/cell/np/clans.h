#ifndef __PS3DK_CELL_NP_CLANS_H__
#define __PS3DK_CELL_NP_CLANS_H__

#include <cell/np/common.h>
#include <cell/rtc.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SCE_NP_CLANS_SUCCESS                                    0
#define SCE_NP_CLANS_ERROR_ALREADY_INITIALIZED                  0x80022701
#define SCE_NP_CLANS_ERROR_NOT_INITIALIZED                      0x80022702
#define SCE_NP_CLANS_ERROR_NOT_SUPPORTED                        0x80022703
#define SCE_NP_CLANS_ERROR_OUT_OF_MEMORY                        0x80022704
#define SCE_NP_CLANS_ERROR_INVALID_ARGUMENT                     0x80022705
#define SCE_NP_CLANS_ERROR_EXCEEDS_MAX                          0x80022706
#define SCE_NP_CLANS_ERROR_BAD_RESPONSE                         0x80022707
#define SCE_NP_CLANS_ERROR_BAD_DATA                             0x80022708
#define SCE_NP_CLANS_ERROR_BAD_REQUEST                          0x80022709
#define SCE_NP_CLANS_ERROR_INVALID_SIGNATURE                    0x8002270a
#define SCE_NP_CLANS_ERROR_INSUFFICIENT                         0x8002270b
#define SCE_NP_CLANS_ERROR_INTERNAL_BUFFER                      0x8002270c
#define SCE_NP_CLANS_ERROR_SERVER_MAINTENANCE                   0x8002270d
#define SCE_NP_CLANS_ERROR_SERVER_END_OF_SERVICE                0x8002270e
#define SCE_NP_CLANS_ERROR_SERVER_BEFORE_START_OF_SERVICE       0x8002270f
#define SCE_NP_CLANS_ERROR_ABORTED                              0x80022710
#define SCE_NP_CLANS_ERROR_SERVICE_UNAVAILABLE                  0x80022711
#define SCE_NP_CLANS_SERVER_ERROR_BAD_REQUEST                   0x80022801
#define SCE_NP_CLANS_SERVER_ERROR_INVALID_TICKET                0x80022802
#define SCE_NP_CLANS_SERVER_ERROR_INVALID_SIGNATURE             0x80022803
#define SCE_NP_CLANS_SERVER_ERROR_TICKET_EXPIRED                0x80022804
#define SCE_NP_CLANS_SERVER_ERROR_INVALID_NPID                  0x80022805
#define SCE_NP_CLANS_SERVER_ERROR_FORBIDDEN                     0x80022806
#define SCE_NP_CLANS_SERVER_ERROR_INTERNAL_SERVER_ERROR         0x80022807
#define SCE_NP_CLANS_SERVER_ERROR_BANNED                        0x8002280a
#define SCE_NP_CLANS_SERVER_ERROR_BLACKLISTED                   0x80022811
#define SCE_NP_CLANS_SERVER_ERROR_INVALID_ENVIRONMENT           0x8002281d
#define SCE_NP_CLANS_SERVER_ERROR_NO_SUCH_CLAN_SERVICE          0x8002282f
#define SCE_NP_CLANS_SERVER_ERROR_NO_SUCH_CLAN                  0x80022830
#define SCE_NP_CLANS_SERVER_ERROR_NO_SUCH_CLAN_MEMBER           0x80022831
#define SCE_NP_CLANS_SERVER_ERROR_BEFORE_HOURS                  0x80022832
#define SCE_NP_CLANS_SERVER_ERROR_CLOSED_SERVICE                0x80022833
#define SCE_NP_CLANS_SERVER_ERROR_PERMISSION_DENIED             0x80022834
#define SCE_NP_CLANS_SERVER_ERROR_CLAN_LIMIT_REACHED            0x80022835
#define SCE_NP_CLANS_SERVER_ERROR_CLAN_LEADER_LIMIT_REACHED     0x80022836
#define SCE_NP_CLANS_SERVER_ERROR_CLAN_MEMBER_LIMIT_REACHED     0x80022837
#define SCE_NP_CLANS_SERVER_ERROR_CLAN_JOINED_LIMIT_REACHED     0x80022838
#define SCE_NP_CLANS_SERVER_ERROR_MEMBER_STATUS_INVALID         0x80022839
#define SCE_NP_CLANS_SERVER_ERROR_DUPLICATED_CLAN_NAME          0x8002283a
#define SCE_NP_CLANS_SERVER_ERROR_CLAN_LEADER_CANNOT_LEAVE      0x8002283b
#define SCE_NP_CLANS_SERVER_ERROR_INVALID_ROLE_PRIORITY         0x8002283c
#define SCE_NP_CLANS_SERVER_ERROR_ANNOUNCEMENT_LIMIT_REACHED    0x8002283d
#define SCE_NP_CLANS_SERVER_ERROR_CLAN_CONFIG_MASTER_NOT_FOUND  0x8002283e
#define SCE_NP_CLANS_SERVER_ERROR_DUPLICATED_CLAN_TAG           0x8002283f
#define SCE_NP_CLANS_SERVER_ERROR_EXCEEDS_CREATE_CLAN_FREQUENCY 0x80022840
#define SCE_NP_CLANS_SERVER_ERROR_CLAN_PASSPHRASE_INCORRECT     0x80022841
#define SCE_NP_CLANS_SERVER_ERROR_CANNOT_RECORD_BLACKLIST_ENTRY 0x80022842
#define SCE_NP_CLANS_SERVER_ERROR_NO_SUCH_CLAN_ANNOUNCEMENT     0x80022843
#define SCE_NP_CLANS_SERVER_ERROR_VULGAR_WORDS_POSTED           0x80022844
#define SCE_NP_CLANS_SERVER_ERROR_BLACKLIST_LIMIT_REACHED       0x80022845
#define SCE_NP_CLANS_SERVER_ERROR_NO_SUCH_BLACKLIST_ENTRY       0x80022846
#define SCE_NP_CLANS_SERVER_ERROR_INVALID_NP_MESSAGE_FORMAT     0x8002284b
#define SCE_NP_CLANS_SERVER_ERROR_FAILED_TO_SEND_NP_MESSAGE     0x8002284c

#define SCE_NP_CLANS_ROLE_UNKNOWN    0
#define SCE_NP_CLANS_ROLE_NON_MEMBER 1
#define SCE_NP_CLANS_ROLE_MEMBER     2
#define SCE_NP_CLANS_ROLE_SUB_LEADER 3
#define SCE_NP_CLANS_ROLE_LEADER     4

#define SCE_NP_CLANS_MEMBER_STATUS_UNKNOWN 0
#define SCE_NP_CLANS_MEMBER_STATUS_NORMAL  1
#define SCE_NP_CLANS_MEMBER_STATUS_INVITED 2
#define SCE_NP_CLANS_MEMBER_STATUS_PENDING 3

#define SCE_NP_CLANS_SEARCH_OPERATOR_EQUAL_TO                 0
#define SCE_NP_CLANS_SEARCH_OPERATOR_NOT_EQUAL_TO             1
#define SCE_NP_CLANS_SEARCH_OPERATOR_GREATER_THAN             2
#define SCE_NP_CLANS_SEARCH_OPERATOR_GREATER_THAN_OR_EQUAL_TO 3
#define SCE_NP_CLANS_SEARCH_OPERATOR_LESS_THAN                4
#define SCE_NP_CLANS_SEARCH_OPERATOR_LESS_THAN_OR_EQUAL_TO    5
#define SCE_NP_CLANS_SEARCH_OPERATOR_SIMILAR_TO               6

#define SCE_NP_CLANS_ANNOUNCEMENT_MESSAGE_BODY_MAX_LENGTH 1536
#define SCE_NP_CLANS_CLAN_BINARY_ATTRIBUTE1_MAX_SIZE      190
#define SCE_NP_CLANS_CLAN_BINARY_DATA_MAX_SIZE            10240
#define SCE_NP_CLANS_MEMBER_BINARY_ATTRIBUTE1_MAX_SIZE    16
#define SCE_NP_CLANS_MEMBER_DESCRIPTION_MAX_LENGTH        255
#define SCE_NP_CLANS_MEMBER_BINARY_DATA_MAX_SIZE          1024
#define SCE_NP_CLANS_MESSAGE_BODY_MAX_LENGTH              1536
#define SCE_NP_CLANS_MESSAGE_SUBJECT_MAX_LENGTH           54
#define SCE_NP_CLANS_MESSAGE_BODY_CHARACTER_MAX           512
#define SCE_NP_CLANS_MESSAGE_SUBJECT_CHARACTER_MAX        18
#define SCE_NP_CLANS_MESSAGE_BINARY_DATA_MAX_SIZE         1024
#define SCE_NP_CLANS_PAGING_REQUEST_START_POSITION_MAX    1000000
#define SCE_NP_CLANS_PAGING_REQUEST_PAGE_MAX              100

#define SCE_NP_CLANS_FIELDS_SEARCHABLE_ATTR_INT_ATTR1       0x00000001
#define SCE_NP_CLANS_FIELDS_SEARCHABLE_ATTR_INT_ATTR2       0x00000002
#define SCE_NP_CLANS_FIELDS_SEARCHABLE_ATTR_INT_ATTR3       0x00000004
#define SCE_NP_CLANS_FIELDS_SEARCHABLE_ATTR_BIN_ATTR1       0x00000008
#define SCE_NP_CLANS_FIELDS_SEARCHABLE_PROFILE_TAG          0x00000010
#define SCE_NP_CLANS_FIELDS_SEARCHABLE_PROFILE_NUM_MEMBERS  0x00000020
#define SCE_NP_CLANS_FIELDS_UPDATABLE_CLAN_INFO_DESCR       0x00000040
#define SCE_NP_CLANS_FIELDS_UPDATABLE_CLAN_INFO_BIN_DATA1   0x00000080
#define SCE_NP_CLANS_FIELDS_UPDATABLE_MEMBER_INFO_DESCR     0x00000100
#define SCE_NP_CLANS_FIELDS_UPDATABLE_MEMBER_INFO_BIN_ATTR1 0x00000200
#define SCE_NP_CLANS_FIELDS_UPDATABLE_MEMBER_INFO_BIN_DATA1 0x00000400
#define SCE_NP_CLANS_FIELDS_UPDATABLE_MEMBER_INFO_ALLOW_MSG 0x00000800

#define SCE_NP_CLANS_MESSAGE_OPTIONS_NONE   0x00000000
#define SCE_NP_CLANS_MESSAGE_OPTIONS_CENSOR 0x00000001

#define SCE_NP_CLANS_INVALID_ID             0
#define SCE_NP_CLANS_INVALID_REQUEST_HANDLE 0

typedef uint32_t SceNpClansRequestId;
typedef SceNpClansRequestId SceNpClansRequestHandle;
typedef uint32_t SceNpClansMessageId;
typedef uint32_t SceNpClansMemberRole;
typedef int32_t SceNpClansMemberStatus;

typedef struct SceNpClansPagingRequest {
    uint32_t startPos;
    uint32_t max;
} SceNpClansPagingRequest;

typedef struct SceNpClansPagingResult {
    uint32_t count;
    uint32_t total;
} SceNpClansPagingResult;

typedef struct SceNpClansClanBasicInfo {
    SceNpClanId clanId;
    uint32_t numMembers;
    char name[SCE_NP_CLANS_CLAN_NAME_MAX_LENGTH + 1];
    char tag[SCE_NP_CLANS_CLAN_TAG_MAX_LENGTH + 1];
    uint8_t reserved[2];
} SceNpClansClanBasicInfo;

typedef struct SceNpClansEntry {
    SceNpClansClanBasicInfo info;
    SceNpClansMemberRole role;
    SceNpClansMemberStatus status;
    uint8_t allowMsg;
    uint8_t reserved[3];
} SceNpClansEntry;

typedef struct SceNpClansSearchableAttr {
    uint32_t fields;
    uint32_t intAttr1;
    uint32_t intAttr2;
    uint32_t intAttr3;
    uint8_t binAttr1[SCE_NP_CLANS_CLAN_BINARY_ATTRIBUTE1_MAX_SIZE];
    uint8_t reserved[2];
} SceNpClansSearchableAttr;

typedef struct SceNpClansSearchableProfile {
    SceNpClansSearchableAttr attr;
    uint32_t fields;
    uint32_t numMembers;
    int32_t tagSearchOp;
    int32_t numMemberSearchOp;
    int32_t intAttr1SearchOp;
    int32_t intAttr2SearchOp;
    int32_t intAttr3SearchOp;
    int32_t binAttr1SearchOp;
    char tag[SCE_NP_CLANS_CLAN_TAG_MAX_LENGTH + 1];
    uint8_t reserved[3];
} SceNpClansSearchableProfile;

typedef struct SceNpClansSearchableName {
    int32_t nameSearchOp;
    char name[SCE_NP_CLANS_CLAN_NAME_MAX_LENGTH + 1];
    uint8_t reserved[3];
} SceNpClansSearchableName;

typedef struct SceNpClansUpdatableClanInfo {
    uint32_t fields;
    char description[SCE_NP_CLANS_CLAN_DESCRIPTION_MAX_LENGTH + 1];
    SceNpClansSearchableAttr attr;
    uint8_t binData1[SCE_NP_CLANS_CLAN_BINARY_DATA_MAX_SIZE];
    uint32_t binData1Size;
} SceNpClansUpdatableClanInfo;

typedef struct SceNpClansClanInfo {
    CellRtcTick dateCreated;
    SceNpClansClanBasicInfo info;
    SceNpClansUpdatableClanInfo updatable;
} SceNpClansClanInfo;

typedef struct SceNpClansUpdatableMemberInfo {
    uint32_t fields;
    uint8_t binData1[SCE_NP_CLANS_MEMBER_BINARY_DATA_MAX_SIZE];
    uint32_t binData1Size;
    uint8_t binAttr1[SCE_NP_CLANS_MEMBER_BINARY_ATTRIBUTE1_MAX_SIZE];
    char description[SCE_NP_CLANS_MEMBER_DESCRIPTION_MAX_LENGTH + 1];
    uint8_t allowMsg;
    uint8_t reserved[3];
} SceNpClansUpdatableMemberInfo;

typedef struct SceNpClansMemberEntry {
    SceNpId npid;
    SceNpClansMemberRole role;
    SceNpClansMemberStatus status;
    SceNpClansUpdatableMemberInfo updatable;
} SceNpClansMemberEntry;

typedef struct SceNpClansMessage {
    char subject[SCE_NP_CLANS_MESSAGE_SUBJECT_MAX_LENGTH + 1];
    char body[SCE_NP_CLANS_MESSAGE_BODY_MAX_LENGTH + 1];
    uint32_t options;
} SceNpClansMessage;

typedef struct SceNpClansMessageData {
    uint8_t binData1[SCE_NP_CLANS_MESSAGE_BINARY_DATA_MAX_SIZE];
    uint32_t binData1Size;
} SceNpClansMessageData;

typedef struct SceNpClansMessageEntry {
    CellRtcTick postDate;
    SceNpClansMessageId mId;
    SceNpClansMessage message;
    SceNpClansMessageData data;
    SceNpId npid;
    SceNpClanId postedBy;
} SceNpClansMessageEntry;

typedef struct SceNpClansBlacklistEntry {
    SceNpId entry;
    SceNpId registeredBy;
} SceNpClansBlacklistEntry;

int sceNpClansInit(const SceNpCommunicationId *commId, const SceNpCommunicationPassphrase *passphrase, void *pool, uint32_t *poolSize, uint32_t flags);
int sceNpClansTerm(void);
int sceNpClansCreateRequest(SceNpClansRequestHandle *handle, uint64_t flags);
int sceNpClansDestroyRequest(SceNpClansRequestHandle handle);
int sceNpClansAbortRequest(SceNpClansRequestHandle handle);
int sceNpClansCreateClan(SceNpClansRequestHandle handle, const char *name, const char *tag, SceNpClanId *clanId);
int sceNpClansDisbandClan(SceNpClansRequestHandle handle, SceNpClanId clanId);
int sceNpClansGetClanList(SceNpClansRequestHandle handle, const SceNpClansPagingRequest *paging, SceNpClansEntry *clanList, SceNpClansPagingResult *pageResult);
int sceNpClansGetClanListByNpId(SceNpClansRequestHandle handle, const SceNpClansPagingRequest *paging, const SceNpId *npid, SceNpClansEntry *clanList, SceNpClansPagingResult *pageResult);
int sceNpClansSearchByProfile(SceNpClansRequestHandle handle, const SceNpClansPagingRequest *paging, const SceNpClansSearchableProfile *search, SceNpClansClanBasicInfo *results, SceNpClansPagingResult *pageResult);
int sceNpClansSearchByName(SceNpClansRequestHandle handle, const SceNpClansPagingRequest *paging, const SceNpClansSearchableName *search, SceNpClansClanBasicInfo *results, SceNpClansPagingResult *pageResult);
int sceNpClansGetClanInfo(SceNpClansRequestHandle handle, SceNpClanId clanId, SceNpClansClanInfo *info);
int sceNpClansUpdateClanInfo(SceNpClansRequestHandle handle, SceNpClanId clanId, const SceNpClansUpdatableClanInfo *info);
int sceNpClansGetMemberList(SceNpClansRequestHandle handle, SceNpClanId clanId, const SceNpClansPagingRequest *paging, SceNpClansMemberStatus status, SceNpClansMemberEntry *memList, SceNpClansPagingResult *pageResult);
int sceNpClansGetMemberInfo(SceNpClansRequestHandle handle, SceNpClanId clanId, const SceNpId *npid, SceNpClansMemberEntry *memInfo);
int sceNpClansUpdateMemberInfo(SceNpClansRequestHandle handle, SceNpClanId clanId, const SceNpClansUpdatableMemberInfo *info);
int sceNpClansChangeMemberRole(SceNpClansRequestHandle handle, SceNpClanId clanId, const SceNpId *npid, uint32_t role);
int sceNpClansGetAutoAcceptStatus(SceNpClansRequestHandle handle, SceNpClanId clanId, uint8_t *enable);
int sceNpClansUpdateAutoAcceptStatus(SceNpClansRequestHandle handle, SceNpClanId clanId, uint8_t enable);
int sceNpClansJoinClan(SceNpClansRequestHandle handle, SceNpClanId clanId);
int sceNpClansLeaveClan(SceNpClansRequestHandle handle, SceNpClanId clanId);
int sceNpClansKickMember(SceNpClansRequestHandle handle, SceNpClanId clanId, const SceNpId *npid, const SceNpClansMessage *message);
int sceNpClansSendInvitation(SceNpClansRequestHandle handle, SceNpClanId clanId, const SceNpId *npid, const SceNpClansMessage *message);
int sceNpClansCancelInvitation(SceNpClansRequestHandle handle, SceNpClanId clanId, const SceNpId *npid);
int sceNpClansSendInvitationResponse(SceNpClansRequestHandle handle, SceNpClanId clanId, const SceNpClansMessage *message, uint8_t accept);
int sceNpClansSendMembershipRequest(SceNpClansRequestHandle handle, SceNpClanId clanId, const SceNpClansMessage *message);
int sceNpClansCancelMembershipRequest(SceNpClansRequestHandle handle, SceNpClanId clanId);
int sceNpClansSendMembershipResponse(SceNpClansRequestHandle handle, SceNpClanId clanId, const SceNpId *npid, const SceNpClansMessage *message, uint8_t allow);
int sceNpClansGetBlacklist(SceNpClansRequestHandle handle, SceNpClanId clanId, const SceNpClansPagingRequest *paging, SceNpClansBlacklistEntry *blacklist, SceNpClansPagingResult *pageResult);
int sceNpClansAddBlacklistEntry(SceNpClansRequestHandle handle, SceNpClanId clanId, const SceNpId *member);
int sceNpClansRemoveBlacklistEntry(SceNpClansRequestHandle handle, SceNpClanId clanId, const SceNpId *member);
int sceNpClansRetrieveAnnouncements(SceNpClansRequestHandle handle, SceNpClanId clanId, const SceNpClansPagingRequest *paging, SceNpClansMessageEntry *messageList, SceNpClansPagingResult *pageResult);
int sceNpClansPostAnnouncement(SceNpClansRequestHandle handle, SceNpClanId clanId, const SceNpClansMessage *message, const SceNpClansMessageData *data, uint32_t duration, SceNpClansMessageId *messageId);
int sceNpClansRemoveAnnouncement(SceNpClansRequestHandle handle, SceNpClanId clanId, SceNpClansMessageId messageId);
int sceNpClansPostChallenge(SceNpClansRequestHandle handle, SceNpClanId clanId, SceNpClanId targetClan, const SceNpClansMessage *message, const SceNpClansMessageData *data, uint32_t duration, SceNpClansMessageId *messageId);
int sceNpClansRetrievePostedChallenges(SceNpClansRequestHandle handle, SceNpClanId clanId, SceNpClanId targetClan, const SceNpClansPagingRequest *paging, SceNpClansMessageEntry *messageList, SceNpClansPagingResult *pageResult);
int sceNpClansRemovePostedChallenge(SceNpClansRequestHandle handle, SceNpClanId clanId, SceNpClanId targetClan, SceNpClansMessageId messageId);
int sceNpClansRetrieveChallenges(SceNpClansRequestHandle handle, SceNpClanId clanId, const SceNpClansPagingRequest *paging, SceNpClansMessageEntry *messageList, SceNpClansPagingResult *pageResult);
int sceNpClansRemoveChallenge(SceNpClansRequestHandle handle, SceNpClanId clanId, SceNpClansMessageId messageId);

#ifdef __cplusplus
}
#endif

#endif /* __PS3DK_CELL_NP_CLANS_H__ */
