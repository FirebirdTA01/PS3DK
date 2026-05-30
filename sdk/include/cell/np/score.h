#ifndef __PS3DK_CELL_NP_SCORE_H__
#define __PS3DK_CELL_NP_SCORE_H__

#include <cell/np/common.h>
#include <cell/rtc.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#ifndef ATTRIBUTE_PRXPTR
#define ATTRIBUTE_PRXPTR
#endif

#ifdef __cplusplus
extern "C" {
#endif

#define SCE_NP_SCORE_COMMENT_MAXLEN 63
#define SCE_NP_SCORE_GAMEINFO_SIZE 64
#define SCE_NP_SCORE_VARIABLE_SIZE_GAMEINFO_MAXSIZE 189
#define SCE_NP_SCORE_GAMEDATA_ID_LEN 63
#define SCE_NP_SCORE_PASSPHRASE_SIZE 128
#define SCE_NP_SCORE_CENSOR_COMMENT_MAXLEN 255
#define SCE_NP_SCORE_SANITIZE_COMMENT_MAXLEN 255

#define SCE_NP_SCORE_NORMAL_UPDATE 0
#define SCE_NP_SCORE_FORCE_UPDATE 1
#define SCE_NP_SCORE_DESCENDING_ORDER 0
#define SCE_NP_SCORE_ASCENDING_ORDER 1

#define SCE_NP_SCORE_MAX_RANGE_NUM_PER_TRANS 100
#define SCE_NP_SCORE_MAX_NPID_NUM_PER_TRANS 101
#define SCE_NP_SCORE_MAX_CLAN_NUM_PER_TRANS 101
#define SCE_NP_SCORE_MAX_SELECTED_FRIENDS_NUM 100
#define SCE_NP_SCORE_MAX_CTX_NUM 32

typedef uint32_t SceNpScoreBoardId;
typedef int64_t SceNpScoreValue;
typedef uint32_t SceNpScoreRankNumber;
typedef int32_t SceNpScorePcId;
typedef SceNpCommunicationPassphrase SceNpScorePassphrase;
typedef uint32_t SceNpScoreClansBoardId;

typedef struct SceNpScoreGameInfo {
    uint8_t nativeData[SCE_NP_SCORE_GAMEINFO_SIZE];
} SceNpScoreGameInfo;

typedef struct SceNpScoreGameDataId {
    char data[SCE_NP_SCORE_GAMEDATA_ID_LEN];
    char term[1];
} SceNpScoreGameDataId;

typedef struct SceNpScoreComment {
    char data[SCE_NP_SCORE_COMMENT_MAXLEN];
    char term[1];
} SceNpScoreComment;

typedef struct SceNpScoreRankData {
    SceNpId npId;
    SceNpOnlineName onlineName;
    SceNpScorePcId pcId;
    SceNpScoreRankNumber serialRank;
    SceNpScoreRankNumber rank;
    SceNpScoreRankNumber highestRank;
    int32_t hasGameData;
    uint8_t pad0[4];
    SceNpScoreValue scoreValue;
    CellRtcTick recordDate;
} SceNpScoreRankData;

typedef struct SceNpScorePlayerRankData {
    int32_t hasData;
    uint8_t pad0[4];
    SceNpScoreRankData rankData;
} SceNpScorePlayerRankData;

typedef struct SceNpScoreBoardInfo {
    uint32_t rankLimit;
    uint32_t updateMode;
    uint32_t sortMode;
    uint32_t uploadNumLimit;
    uint32_t uploadSizeLimit;
} SceNpScoreBoardInfo;

typedef struct SceNpScoreNpIdPcId {
    SceNpId npId;
    SceNpScorePcId pcId;
    uint8_t pad[4];
} SceNpScoreNpIdPcId;

typedef struct SceNpScoreClanBasicInfo {
    char clanName[SCE_NP_CLANS_CLAN_NAME_MAX_LENGTH + 1];
    char clanTag[SCE_NP_CLANS_CLAN_TAG_MAX_LENGTH + 1];
    uint8_t reserved[10];
} SceNpScoreClanBasicInfo;

typedef struct SceNpScoreClansMemberDescription {
    char description[SCE_NP_CLANS_CLAN_DESCRIPTION_MAX_LENGTH + 1];
} SceNpScoreClansMemberDescription;

typedef struct SceNpScoreClanRankData {
    SceNpClanId clanId;
    SceNpScoreClanBasicInfo clanInfo;
    uint32_t regularMemberCount;
    uint32_t recordMemberCount;
    SceNpScoreRankNumber serialRank;
    SceNpScoreRankNumber rank;
    SceNpScoreValue scoreValue;
    CellRtcTick recordDate;
    SceNpId npId;
    SceNpOnlineName onlineName;
    uint8_t reserved[32];
} SceNpScoreClanRankData;

typedef struct SceNpScoreClanIdRankData {
    int32_t hasData;
    uint8_t pad0[4];
    SceNpScoreClanRankData rankData;
} SceNpScoreClanIdRankData;

typedef struct SceNpScoreVariableSizeGameInfo {
    uint32_t infoSize;
    uint8_t data[SCE_NP_SCORE_VARIABLE_SIZE_GAMEINFO_MAXSIZE];
    uint8_t pad2[3];
} SceNpScoreVariableSizeGameInfo;

typedef struct SceNpScoreRecordOptParam {
    uint32_t size;
    SceNpScoreVariableSizeGameInfo *vsInfo ATTRIBUTE_PRXPTR;
    CellRtcTick *compareDate ATTRIBUTE_PRXPTR;
} SceNpScoreRecordOptParam;

int sceNpScoreInit(void);
int sceNpScoreTerm(void);
int sceNpScoreCreateTitleCtx(const SceNpCommunicationId *titleId, const SceNpCommunicationPassphrase *passphrase, const SceNpId *selfNpId);
int sceNpScoreDestroyTitleCtx(int32_t titleCtxId);
int sceNpScoreCreateTransactionCtx(int32_t titleCtxId);
int sceNpScoreDestroyTransactionCtx(int32_t transId);
int sceNpScoreAbortTransaction(int32_t transId);
int sceNpScoreSetTimeout(int32_t ctxId, usecond_t timeout);
int sceNpScoreSetPlayerCharacterId(int32_t ctxId, SceNpScorePcId pcId);
int sceNpScoreWaitAsync(int32_t transId, int32_t *result);
int sceNpScorePollAsync(int32_t transId, int32_t *result);
int sceNpScoreGetBoardInfo(int32_t transId, SceNpScoreBoardId boardId, SceNpScoreBoardInfo *boardInfo, void *option);
int sceNpScoreGetBoardInfoAsync(int32_t transId, SceNpScoreBoardId boardId, SceNpScoreBoardInfo *boardInfo, int32_t prio, void *option);
int32_t sceNpScoreRecordScore(int32_t transId, SceNpScoreBoardId boardId, SceNpScoreValue score, const SceNpScoreComment *scoreComment, const SceNpScoreGameInfo *gameInfo, SceNpScoreRankNumber *tmpRank, SceNpScoreRecordOptParam *option);
int sceNpScoreRecordScoreAsync(int32_t transId, SceNpScoreBoardId boardId, SceNpScoreValue score, const SceNpScoreComment *scoreComment, const SceNpScoreGameInfo *gameInfo, SceNpScoreRankNumber *tmpRank, int32_t prio, SceNpScoreRecordOptParam *option);
int sceNpScoreRecordGameData(int32_t transId, SceNpScoreBoardId boardId, SceNpScoreValue score, size_t totalSize, size_t sendSize, const void *data, void *option);
int sceNpScoreRecordGameDataAsync(int32_t transId, SceNpScoreBoardId boardId, SceNpScoreValue score, size_t totalSize, size_t sendSize, const void *data, int32_t prio, void *option);
int sceNpScoreGetGameData(int32_t transId, SceNpScoreBoardId boardId, const SceNpId *npId, size_t *totalSize, size_t recvSize, void *data, void *option);
int sceNpScoreGetGameDataAsync(int32_t transId, SceNpScoreBoardId boardId, const SceNpId *npId, size_t *totalSize, size_t recvSize, void *data, int32_t prio, void *option);
int sceNpScoreGetRankingByNpId(int32_t transId, SceNpScoreBoardId boardId, const SceNpId *npIdArray, size_t npIdArraySize, SceNpScorePlayerRankData *rankArray, size_t rankArraySize, SceNpScoreComment *commentArray, size_t commentArraySize, void *infoArray, size_t infoArraySize, size_t arrayNum, CellRtcTick *lastSortDate, SceNpScoreRankNumber *totalRecord, void *option);
int sceNpScoreGetRankingByNpIdAsync(int32_t transId, SceNpScoreBoardId boardId, const SceNpId *npIdArray, size_t npIdArraySize, SceNpScorePlayerRankData *rankArray, size_t rankArraySize, SceNpScoreComment *commentArray, size_t commentArraySize, void *infoArray, size_t infoArraySize, size_t arrayNum, CellRtcTick *lastSortDate, SceNpScoreRankNumber *totalRecord, int32_t prio, void *option);
int sceNpScoreGetRankingByRange(int32_t transId, SceNpScoreBoardId boardId, SceNpScoreRankNumber startSerialRank, SceNpScoreRankData *rankArray, size_t rankArraySize, SceNpScoreComment *commentArray, size_t commentArraySize, void *infoArray, size_t infoArraySize, size_t arrayNum, CellRtcTick *lastSortDate, SceNpScoreRankNumber *totalRecord, void *option);
int sceNpScoreGetRankingByRangeAsync(int32_t transId, SceNpScoreBoardId boardId, SceNpScoreRankNumber startSerialRank, SceNpScoreRankData *rankArray, size_t rankArraySize, SceNpScoreComment *commentArray, size_t commentArraySize, void *infoArray, size_t infoArraySize, size_t arrayNum, CellRtcTick *lastSortDate, SceNpScoreRankNumber *totalRecord, int32_t prio, void *option);
int sceNpScoreGetRankingByNpIdPcId(int32_t transId, SceNpScoreBoardId boardId, const SceNpScoreNpIdPcId *idArray, size_t idArraySize, SceNpScorePlayerRankData *rankArray, size_t rankArraySize, SceNpScoreComment *commentArray, size_t commentArraySize, void *infoArray, size_t infoArraySize, size_t arrayNum, CellRtcTick *lastSortDate, SceNpScoreRankNumber *totalRecord, void *option);
int sceNpScoreGetRankingByNpIdPcIdAsync(int32_t transId, SceNpScoreBoardId boardId, const SceNpScoreNpIdPcId *idArray, size_t idArraySize, SceNpScorePlayerRankData *rankArray, size_t rankArraySize, SceNpScoreComment *commentArray, size_t commentArraySize, void *infoArray, size_t infoArraySize, size_t arrayNum, CellRtcTick *lastSortDate, SceNpScoreRankNumber *totalRecord, int32_t prio, void *option);
int sceNpScoreGetFriendsRanking(int32_t transId, SceNpScoreBoardId boardId, int32_t includeSelf, SceNpScoreRankData *rankArray, size_t rankArraySize, SceNpScoreComment *commentArray, size_t commentArraySize, void *infoArray, size_t infoArraySize, size_t arrayNum, CellRtcTick *lastSortDate, SceNpScoreRankNumber *totalRecord, void *option);
int sceNpScoreGetFriendsRankingAsync(int32_t transId, SceNpScoreBoardId boardId, int32_t includeSelf, SceNpScoreRankData *rankArray, size_t rankArraySize, SceNpScoreComment *commentArray, size_t commentArraySize, void *infoArray, size_t infoArraySize, size_t arrayNum, CellRtcTick *lastSortDate, SceNpScoreRankNumber *totalRecord, int32_t prio, void *option);
int sceNpScoreCensorComment(int32_t transId, const void *comment, void *option);
int sceNpScoreCensorCommentAsync(int32_t transId, const void *comment, int32_t prio, void *option);
int sceNpScoreSanitizeComment(int32_t transId, const char *comment, char *sanitizedComment, void *option);
int sceNpScoreSanitizeCommentAsync(int32_t transId, const char *comment, char *sanitizedComment, int32_t prio, void *option);
int sceNpScoreGetClanMemberGameData(int32_t transId, SceNpScoreClansBoardId boardId, SceNpClanId clanId, const SceNpId *npId, size_t *totalSize, size_t recvSize, void *data, void *option);
int sceNpScoreGetClanMemberGameDataAsync(int32_t transId, SceNpScoreClansBoardId boardId, SceNpClanId clanId, const SceNpId *npId, size_t *totalSize, size_t recvSize, void *data, int32_t prio, void *option);
int sceNpScoreGetClansMembersRankingByRange(int32_t transId, SceNpClanId clanId, SceNpScoreBoardId boardId, SceNpScoreRankNumber startSerialRank, SceNpScoreRankData *rankArray, size_t rankArraySize, SceNpScoreComment *commentArray, size_t commentArraySize, SceNpScoreGameInfo *infoArray, size_t infoArraySize, SceNpScoreClansMemberDescription *descriptArray, size_t descriptArraySize, size_t arrayNum, SceNpScoreClanBasicInfo *clanInfo, CellRtcTick *lastSortDate, SceNpScoreRankNumber *totalRecord, void *option);
int sceNpScoreGetClansMembersRankingByRangeAsync(int32_t transId, SceNpClanId clanId, SceNpScoreBoardId boardId, SceNpScoreRankNumber startSerialRank, SceNpScoreRankData *rankArray, size_t rankArraySize, SceNpScoreComment *commentArray, size_t commentArraySize, SceNpScoreGameInfo *infoArray, size_t infoArraySize, SceNpScoreClansMemberDescription *descriptArray, size_t descriptArraySize, size_t arrayNum, SceNpScoreClanBasicInfo *clanInfo, CellRtcTick *lastSortDate, SceNpScoreRankNumber *totalRecord, int32_t prio, void *option);
int sceNpScoreGetClansMembersRankingByNpId(int32_t transId, SceNpClanId clanId, SceNpScoreBoardId boardId, const SceNpId *idArray, size_t idArraySize, SceNpScorePlayerRankData *rankArray, size_t rankArraySize, SceNpScoreComment *commentArray, size_t commentArraySize, SceNpScoreGameInfo *infoArray, size_t infoArraySize, SceNpScoreClansMemberDescription *descriptArray, size_t descriptArraySize, size_t arrayNum, SceNpScoreClanBasicInfo *clanInfo, CellRtcTick *lastSortDate, SceNpScoreRankNumber *totalRecord, void *option);
int sceNpScoreGetClansMembersRankingByNpIdAsync(int32_t transId, SceNpClanId clanId, SceNpScoreBoardId boardId, const SceNpId *idArray, size_t idArraySize, SceNpScorePlayerRankData *rankArray, size_t rankArraySize, SceNpScoreComment *commentArray, size_t commentArraySize, SceNpScoreGameInfo *infoArray, size_t infoArraySize, SceNpScoreClansMemberDescription *descriptArray, size_t descriptArraySize, size_t arrayNum, SceNpScoreClanBasicInfo *clanInfo, CellRtcTick *lastSortDate, SceNpScoreRankNumber *totalRecord, int32_t prio, void *option);
int sceNpScoreGetClansMembersRankingByNpIdPcId(int32_t transId, SceNpClanId clanId, SceNpScoreBoardId boardId, const SceNpScoreNpIdPcId *idArray, size_t idArraySize, SceNpScorePlayerRankData *rankArray, size_t rankArraySize, SceNpScoreComment *commentArray, size_t commentArraySize, SceNpScoreGameInfo *infoArray, size_t infoArraySize, SceNpScoreClansMemberDescription *descriptArray, size_t descriptArraySize, size_t arrayNum, SceNpScoreClanBasicInfo *clanInfo, CellRtcTick *lastSortDate, SceNpScoreRankNumber *totalRecord, void *option);
int sceNpScoreGetClansMembersRankingByNpIdPcIdAsync(int32_t transId, SceNpClanId clanId, SceNpScoreBoardId boardId, const SceNpScoreNpIdPcId *idArray, size_t idArraySize, SceNpScorePlayerRankData *rankArray, size_t rankArraySize, SceNpScoreComment *commentArray, size_t commentArraySize, SceNpScoreGameInfo *infoArray, size_t infoArraySize, SceNpScoreClansMemberDescription *descriptArray, size_t descriptArraySize, size_t arrayNum, SceNpScoreClanBasicInfo *clanInfo, CellRtcTick *lastSortDate, SceNpScoreRankNumber *totalRecord, int32_t prio, void *option);
int sceNpScoreGetClansRankingByRange(int32_t transId, SceNpScoreClansBoardId clanBoardId, SceNpScoreRankNumber startSerialRank, SceNpScoreClanRankData *clanRankArray, size_t rankArraySize, void *reserved1, size_t reservedSize1, void *reserved2, size_t reservedSize2, size_t arrayNum, CellRtcTick *lastSortDate, SceNpScoreRankNumber *totalRecord, void *option);
int sceNpScoreGetClansRankingByRangeAsync(int32_t transId, SceNpScoreClansBoardId clanBoardId, SceNpScoreRankNumber startSerialRank, SceNpScoreClanRankData *clanRankArray, size_t rankArraySize, void *reserved1, size_t reservedSize1, void *reserved2, size_t reservedSize2, size_t arrayNum, CellRtcTick *lastSortDate, SceNpScoreRankNumber *totalRecord, int32_t prio, void *option);
int sceNpScoreGetClansRankingByClanId(int32_t transId, SceNpScoreClansBoardId clanBoardId, const SceNpClanId *clanIdArray, size_t clanIdArraySize, SceNpScoreClanIdRankData *rankArray, size_t rankArraySize, void *reserved1, size_t reservedSize1, void *reserved2, size_t reservedSize2, size_t arrayNum, CellRtcTick *lastSortDate, SceNpScoreRankNumber *totalRecord, void *option);
int sceNpScoreGetClansRankingByClanIdAsync(int32_t transId, SceNpScoreClansBoardId clanBoardId, const SceNpClanId *clanIdArray, size_t clanIdArraySize, SceNpScoreClanIdRankData *rankArray, size_t rankArraySize, void *reserved1, size_t reservedSize1, void *reserved2, size_t reservedSize2, size_t arrayNum, CellRtcTick *lastSortDate, SceNpScoreRankNumber *totalRecord, int32_t prio, void *option);

#ifdef __cplusplus
}
#endif

#endif /* __PS3DK_CELL_NP_SCORE_H__ */
