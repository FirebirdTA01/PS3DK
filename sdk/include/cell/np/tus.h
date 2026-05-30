#ifndef __PS3DK_CELL_NP_TUS_H__
#define __PS3DK_CELL_NP_TUS_H__

#include <cell/np/common.h>
#include <cell/rtc.h>
#include <stdint.h>
#include <sys/types.h>

#ifndef ATTRIBUTE_PRXPTR
#define ATTRIBUTE_PRXPTR
#endif

#ifdef __cplusplus
extern "C" {
#endif

#define SCE_NP_TUS_ERROR_NOT_INITIALIZED                 0x80022c01
#define SCE_NP_TUS_ERROR_ALREADY_INITIALIZED             0x80022c02
#define SCE_NP_TUS_ERROR_INVALID_ARGUMENT                0x80022c03
#define SCE_NP_TUS_ERROR_OUT_OF_MEMORY                   0x80022c04
#define SCE_NP_TUS_ERROR_TRANSACTION_ALREADY_END         0x80022c05
#define SCE_NP_TUS_ERROR_ABORTED                         0x80022c06
#define SCE_NP_TUS_ERROR_INVALID_SLOT_ID                 0x80022c07
#define SCE_NP_TUS_ERROR_INVALID_SLOT_ID_ARRAY           0x80022c08
#define SCE_NP_TUS_ERROR_INVALID_VUSER                   0x80022c09
#define SCE_NP_TUS_ERROR_INVALID_NP_ID                   0x80022c0a
#define SCE_NP_TUS_ERROR_INVALID_TUS_CTX_ID              0x80022c0b
#define SCE_NP_TUS_ERROR_INVALID_TUS_TRANS_ID            0x80022c0c
#define SCE_NP_TUS_ERROR_INVALID_TUS_TRANS_STATE         0x80022c0d
#define SCE_NP_TUS_ERROR_BUSY                            0x80022c0e
#define SCE_NP_TUS_ERROR_TIMEOUT                         0x80022c0f
#define SCE_NP_TUS_ERROR_INVALID_DATA_SIZE               0x80022c10
#define SCE_NP_TUS_ERROR_INVALID_ARRAY_NUM               0x80022c11
#define SCE_NP_TUS_ERROR_INVALID_SIZE                    0x80022c12
#define SCE_NP_TUS_ERROR_INVALID_ALIGNMENT               0x80022c13
#define SCE_NP_TUS_ERROR_SERVER                          0x80022c14
#define SCE_NP_TUS_ERROR_DATA_NOT_FOUND                  0x80022c15
#define SCE_NP_TUS_ERROR_INVALID_OPERATION               0x80022c16
#define SCE_NP_TUS_ERROR_INVALID_TYPE                    0x80022c17
#define SCE_NP_TUS_ERROR_UNKNOWN                         0x80022cff

#define SCE_NP_TUS_DATA_INFO_MAX_SIZE 384
#define SCE_NP_TUS_MAX_CTX_NUM 32
#define SCE_NP_TUS_MAX_SLOT_NUM_PER_TRANS 64
#define SCE_NP_TUS_MAX_USER_NUM_PER_TRANS 101
#define SCE_NP_TUS_MAX_SELECTED_FRIENDS_NUM 100

#define SCE_NP_TUS_OPETYPE_EQUAL 1
#define SCE_NP_TUS_OPETYPE_NOT_EQUAL 2
#define SCE_NP_TUS_OPETYPE_GREATER_THAN 3
#define SCE_NP_TUS_OPETYPE_GREATER_OR_EQUAL 4
#define SCE_NP_TUS_OPETYPE_LESS_THAN 5
#define SCE_NP_TUS_OPETYPE_LESS_OR_EQUAL 6

#define SCE_NP_TUS_VARIABLE_SORTTYPE_DESCENDING_DATE 1
#define SCE_NP_TUS_VARIABLE_SORTTYPE_ASCENDING_DATE 2
#define SCE_NP_TUS_VARIABLE_SORTTYPE_DESCENDING_VALUE 3
#define SCE_NP_TUS_VARIABLE_SORTTYPE_ASCENDING_VALUE 4

#define SCE_NP_TUS_DATASTATUS_SORTTYPE_DESCENDING_DATE 1
#define SCE_NP_TUS_DATASTATUS_SORTTYPE_ASCENDING_DATE 2

#define SCE_NP_TSS_STATUS_TYPE_OK 0
#define SCE_NP_TSS_STATUS_TYPE_PARTIAL 1
#define SCE_NP_TSS_STATUS_TYPE_NOT_MODIFIED 2

#define SCE_NP_TSS_IFTYPE_IF_MODIFIED_SINCE 0
#define SCE_NP_TSS_IFTYPE_IF_RANGE 1

typedef int32_t SceNpTusCtxId;
typedef int32_t SceNpTusTitleCtxId;
typedef int32_t SceNpTusTransactionId;
typedef int32_t SceNpTusSlotId;
typedef int32_t SceNpTssSlotId;
typedef SceNpOnlineId SceNpTusVirtualUserId;

typedef struct SceNpTusVariable {
    SceNpId ownerId;
    int32_t hasData;
    CellRtcTick lastChangedDate;
    uint8_t pad[4];
    SceNpId lastChangedAuthorId;
    int64_t variable;
    int64_t oldVariable;
    uint8_t reserved[16];
} SceNpTusVariable;

typedef struct SceNpTusDataInfo {
    uint32_t infoSize;
    uint8_t pad[4];
    uint8_t data[SCE_NP_TUS_DATA_INFO_MAX_SIZE];
} SceNpTusDataInfo;

typedef struct SceNpTusDataStatus {
    SceNpId ownerId;
    int32_t hasData;
    CellRtcTick lastChangedDate;
    SceNpId lastChangedAuthorId;
    void *data ATTRIBUTE_PRXPTR;
    uint32_t dataSize;
    uint8_t pad[4];
    SceNpTusDataInfo info;
} SceNpTusDataStatus;

typedef struct SceNpTusAddAndGetVariableOptParam {
    uint32_t size;
    CellRtcTick *isLastChangedDate ATTRIBUTE_PRXPTR;
    SceNpId *isLastChangedAuthorId ATTRIBUTE_PRXPTR;
} SceNpTusAddAndGetVariableOptParam;

typedef struct SceNpTusTryAndSetVariableOptParam {
    uint32_t size;
    CellRtcTick *isLastChangedDate ATTRIBUTE_PRXPTR;
    SceNpId *isLastChangedAuthorId ATTRIBUTE_PRXPTR;
    int64_t *compareValue ATTRIBUTE_PRXPTR;
} SceNpTusTryAndSetVariableOptParam;

typedef struct SceNpTusSetDataOptParam {
    uint32_t size;
    CellRtcTick *isLastChangedDate ATTRIBUTE_PRXPTR;
    SceNpId *isLastChangedAuthorId ATTRIBUTE_PRXPTR;
} SceNpTusSetDataOptParam;

typedef struct SceNpTssDataStatus {
    CellRtcTick lastModified;
    int32_t statusCodeType;
    uint32_t contentLength;
} SceNpTssDataStatus;

typedef struct SceNpTssIfModifiedSinceParam {
    int32_t ifType;
    uint8_t padding[4];
    CellRtcTick lastModified;
} SceNpTssIfModifiedSinceParam;

typedef struct SceNpTssGetDataOptParam {
    uint32_t size;
    uint64_t *offset ATTRIBUTE_PRXPTR;
    uint64_t *lastByte ATTRIBUTE_PRXPTR;
    SceNpTssIfModifiedSinceParam *ifParam ATTRIBUTE_PRXPTR;
} SceNpTssGetDataOptParam;

int sceNpTusInit(int32_t prio);
int sceNpTusTerm(void);
int sceNpTusCreateTitleCtx(const SceNpCommunicationId *communicationId, const SceNpCommunicationPassphrase *passphrase, const SceNpId *selfNpId);
int sceNpTusDestroyTitleCtx(SceNpTusTitleCtxId titleCtxId);
int sceNpTusCreateTransactionCtx(SceNpTusTitleCtxId titleCtxId);
int sceNpTusDestroyTransactionCtx(SceNpTusTransactionId transId);
int sceNpTusSetTimeout(int32_t ctxId, usecond_t timeout);
int sceNpTusAbortTransaction(SceNpTusTransactionId transId);
int sceNpTusWaitAsync(SceNpTusTransactionId transId, int32_t *result);
int sceNpTusPollAsync(SceNpTusTransactionId transId, int32_t *result);

int sceNpTusSetMultiSlotVariable(SceNpTusTransactionId transId, const SceNpId *targetNpId, const SceNpTusSlotId *slotIdArray, const int64_t *variableArray, int32_t arrayNum, void *option);
int sceNpTusSetMultiSlotVariableAsync(SceNpTusTransactionId transId, const SceNpId *targetNpId, const SceNpTusSlotId *slotIdArray, const int64_t *variableArray, int32_t arrayNum, void *option);
int sceNpTusSetMultiSlotVariableVUser(SceNpTusTransactionId transId, const SceNpTusVirtualUserId *targetVirtualUserId, const SceNpTusSlotId *slotIdArray, const int64_t *variableArray, int32_t arrayNum, void *option);
int sceNpTusSetMultiSlotVariableVUserAsync(SceNpTusTransactionId transId, const SceNpTusVirtualUserId *targetVirtualUserId, const SceNpTusSlotId *slotIdArray, const int64_t *variableArray, int32_t arrayNum, void *option);

int sceNpTusGetMultiSlotVariable(SceNpTusTransactionId transId, const SceNpId *targetNpId, const SceNpTusSlotId *slotIdArray, SceNpTusVariable *variableArray, uint32_t variableArraySize, int32_t arrayNum, void *option);
int sceNpTusGetMultiSlotVariableAsync(SceNpTusTransactionId transId, const SceNpId *targetNpId, const SceNpTusSlotId *slotIdArray, SceNpTusVariable *variableArray, uint32_t variableArraySize, int32_t arrayNum, void *option);
int sceNpTusGetMultiSlotVariableVUser(SceNpTusTransactionId transId, const SceNpTusVirtualUserId *targetVirtualUserId, const SceNpTusSlotId *slotIdArray, SceNpTusVariable *variableArray, uint32_t variableArraySize, int32_t arrayNum, void *option);
int sceNpTusGetMultiSlotVariableVUserAsync(SceNpTusTransactionId transId, const SceNpTusVirtualUserId *targetVirtualUserId, const SceNpTusSlotId *slotIdArray, SceNpTusVariable *variableArray, uint32_t variableArraySize, int32_t arrayNum, void *option);

int sceNpTusGetMultiUserVariable(SceNpTusTransactionId transId, const SceNpId *targetNpIdArray, SceNpTusSlotId slotId, SceNpTusVariable *variableArray, uint32_t variableArraySize, int32_t arrayNum, void *option);
int sceNpTusGetMultiUserVariableAsync(SceNpTusTransactionId transId, const SceNpId *targetNpIdArray, SceNpTusSlotId slotId, SceNpTusVariable *variableArray, uint32_t variableArraySize, int32_t arrayNum, void *option);
int sceNpTusGetMultiUserVariableVUser(SceNpTusTransactionId transId, const SceNpTusVirtualUserId *targetVirtualUserIdArray, SceNpTusSlotId slotId, SceNpTusVariable *variableArray, uint32_t variableArraySize, int32_t arrayNum, void *option);
int sceNpTusGetMultiUserVariableVUserAsync(SceNpTusTransactionId transId, const SceNpTusVirtualUserId *targetVirtualUserIdArray, SceNpTusSlotId slotId, SceNpTusVariable *variableArray, uint32_t variableArraySize, int32_t arrayNum, void *option);

int sceNpTusGetFriendsVariable(SceNpTusTransactionId transId, SceNpTusSlotId slotId, int32_t includeSelf, int32_t sortType, SceNpTusVariable *variableArray, uint32_t variableArraySize, int32_t arrayNum, void *option);
int sceNpTusGetFriendsVariableAsync(SceNpTusTransactionId transId, SceNpTusSlotId slotId, int32_t includeSelf, int32_t sortType, SceNpTusVariable *variableArray, uint32_t variableArraySize, int32_t arrayNum, void *option);

int sceNpTusAddAndGetVariable(SceNpTusTransactionId transId, const SceNpId *targetNpId, SceNpTusSlotId slotId, int64_t inVariable, SceNpTusVariable *outVariable, uint32_t outVariableSize, SceNpTusAddAndGetVariableOptParam *option);
int sceNpTusAddAndGetVariableAsync(SceNpTusTransactionId transId, const SceNpId *targetNpId, SceNpTusSlotId slotId, int64_t inVariable, SceNpTusVariable *outVariable, uint32_t outVariableSize, SceNpTusAddAndGetVariableOptParam *option);
int sceNpTusAddAndGetVariableVUser(SceNpTusTransactionId transId, const SceNpTusVirtualUserId *targetVirtualUserId, SceNpTusSlotId slotId, int64_t inVariable, SceNpTusVariable *outVariable, uint32_t outVariableSize, SceNpTusAddAndGetVariableOptParam *option);
int sceNpTusAddAndGetVariableVUserAsync(SceNpTusTransactionId transId, const SceNpTusVirtualUserId *targetVirtualUserId, SceNpTusSlotId slotId, int64_t inVariable, SceNpTusVariable *outVariable, uint32_t outVariableSize, SceNpTusAddAndGetVariableOptParam *option);

int sceNpTusTryAndSetVariable(SceNpTusTransactionId transId, const SceNpId *targetNpId, SceNpTusSlotId slotId, int32_t opeType, int64_t variable, SceNpTusVariable *resultVariable, uint32_t resultVariableSize, SceNpTusTryAndSetVariableOptParam *option);
int sceNpTusTryAndSetVariableAsync(SceNpTusTransactionId transId, const SceNpId *targetNpId, SceNpTusSlotId slotId, int32_t opeType, int64_t variable, SceNpTusVariable *resultVariable, uint32_t resultVariableSize, SceNpTusTryAndSetVariableOptParam *option);
int sceNpTusTryAndSetVariableVUser(SceNpTusTransactionId transId, const SceNpTusVirtualUserId *targetVirtualUserId, SceNpTusSlotId slotId, int32_t opeType, int64_t variable, SceNpTusVariable *resultVariable, uint32_t resultVariableSize, SceNpTusTryAndSetVariableOptParam *option);
int sceNpTusTryAndSetVariableVUserAsync(SceNpTusTransactionId transId, const SceNpTusVirtualUserId *targetVirtualUserId, SceNpTusSlotId slotId, int32_t opeType, int64_t variable, SceNpTusVariable *resultVariable, uint32_t resultVariableSize, SceNpTusTryAndSetVariableOptParam *option);

int sceNpTusDeleteMultiSlotVariable(SceNpTusTransactionId transId, const SceNpId *targetNpId, const SceNpTusSlotId *slotIdArray, int32_t arrayNum, void *option);
int sceNpTusDeleteMultiSlotVariableAsync(SceNpTusTransactionId transId, const SceNpId *targetNpId, const SceNpTusSlotId *slotIdArray, int32_t arrayNum, void *option);
int sceNpTusDeleteMultiSlotVariableVUser(SceNpTusTransactionId transId, const SceNpTusVirtualUserId *targetVirtualUserId, const SceNpTusSlotId *slotIdArray, int32_t arrayNum, void *option);
int sceNpTusDeleteMultiSlotVariableVUserAsync(SceNpTusTransactionId transId, const SceNpTusVirtualUserId *targetVirtualUserId, const SceNpTusSlotId *slotIdArray, int32_t arrayNum, void *option);

int sceNpTusSetData(SceNpTusTransactionId transId, const SceNpId *targetNpId, SceNpTusSlotId slotId, uint32_t totalSize, uint32_t sendSize, const void *data, const SceNpTusDataInfo *info, uint32_t infoStructSize, SceNpTusSetDataOptParam *option);
int sceNpTusSetDataAsync(SceNpTusTransactionId transId, const SceNpId *targetNpId, SceNpTusSlotId slotId, uint32_t totalSize, uint32_t sendSize, const void *data, const SceNpTusDataInfo *info, uint32_t infoStructSize, SceNpTusSetDataOptParam *option);
int sceNpTusSetDataVUser(SceNpTusTransactionId transId, const SceNpTusVirtualUserId *targetVirtualUserId, SceNpTusSlotId slotId, uint32_t totalSize, uint32_t sendSize, const void *data, const SceNpTusDataInfo *info, uint32_t infoStructSize, SceNpTusSetDataOptParam *option);
int sceNpTusSetDataVUserAsync(SceNpTusTransactionId transId, const SceNpTusVirtualUserId *targetVirtualUserId, SceNpTusSlotId slotId, uint32_t totalSize, uint32_t sendSize, const void *data, const SceNpTusDataInfo *info, uint32_t infoStructSize, SceNpTusSetDataOptParam *option);

int sceNpTusGetData(SceNpTusTransactionId transId, const SceNpId *targetNpId, SceNpTusSlotId slotId, SceNpTusDataStatus *dataStatus, uint32_t dataStatusSize, void *data, uint32_t recvSize, void *option);
int sceNpTusGetDataAsync(SceNpTusTransactionId transId, const SceNpId *targetNpId, SceNpTusSlotId slotId, SceNpTusDataStatus *dataStatus, uint32_t dataStatusSize, void *data, uint32_t recvSize, void *option);
int sceNpTusGetDataVUser(SceNpTusTransactionId transId, const SceNpTusVirtualUserId *targetVirtualUserId, SceNpTusSlotId slotId, SceNpTusDataStatus *dataStatus, uint32_t dataStatusSize, void *data, uint32_t recvSize, void *option);
int sceNpTusGetDataVUserAsync(SceNpTusTransactionId transId, const SceNpTusVirtualUserId *targetVirtualUserId, SceNpTusSlotId slotId, SceNpTusDataStatus *dataStatus, uint32_t dataStatusSize, void *data, uint32_t recvSize, void *option);

int sceNpTusGetMultiSlotDataStatus(SceNpTusTransactionId transId, const SceNpId *targetNpId, const SceNpTusSlotId *slotIdArray, SceNpTusDataStatus *statusArray, uint32_t statusArraySize, int32_t arrayNum, void *option);
int sceNpTusGetMultiSlotDataStatusAsync(SceNpTusTransactionId transId, const SceNpId *targetNpId, const SceNpTusSlotId *slotIdArray, SceNpTusDataStatus *statusArray, uint32_t statusArraySize, int32_t arrayNum, void *option);
int sceNpTusGetMultiSlotDataStatusVUser(SceNpTusTransactionId transId, const SceNpTusVirtualUserId *targetVirtualUserId, const SceNpTusSlotId *slotIdArray, SceNpTusDataStatus *statusArray, uint32_t statusArraySize, int32_t arrayNum, void *option);
int sceNpTusGetMultiSlotDataStatusVUserAsync(SceNpTusTransactionId transId, const SceNpTusVirtualUserId *targetVirtualUserId, const SceNpTusSlotId *slotIdArray, SceNpTusDataStatus *statusArray, uint32_t statusArraySize, int32_t arrayNum, void *option);

int sceNpTusGetMultiUserDataStatus(SceNpTusTransactionId transId, const SceNpId *targetNpIdArray, SceNpTusSlotId slotId, SceNpTusDataStatus *statusArray, uint32_t statusArraySize, int32_t arrayNum, void *option);
int sceNpTusGetMultiUserDataStatusAsync(SceNpTusTransactionId transId, const SceNpId *targetNpIdArray, SceNpTusSlotId slotId, SceNpTusDataStatus *statusArray, uint32_t statusArraySize, int32_t arrayNum, void *option);
int sceNpTusGetMultiUserDataStatusVUser(SceNpTusTransactionId transId, const SceNpTusVirtualUserId *targetVirtualUserIdArray, SceNpTusSlotId slotId, SceNpTusDataStatus *statusArray, uint32_t statusArraySize, int32_t arrayNum, void *option);
int sceNpTusGetMultiUserDataStatusVUserAsync(SceNpTusTransactionId transId, const SceNpTusVirtualUserId *targetVirtualUserIdArray, SceNpTusSlotId slotId, SceNpTusDataStatus *statusArray, uint32_t statusArraySize, int32_t arrayNum, void *option);

int sceNpTusGetFriendsDataStatus(SceNpTusTransactionId transId, SceNpTusSlotId slotId, int32_t includeSelf, int32_t sortType, SceNpTusDataStatus *statusArray, uint32_t statusArraySize, int32_t arrayNum, void *option);
int sceNpTusGetFriendsDataStatusAsync(SceNpTusTransactionId transId, SceNpTusSlotId slotId, int32_t includeSelf, int32_t sortType, SceNpTusDataStatus *statusArray, uint32_t statusArraySize, int32_t arrayNum, void *option);

int sceNpTusDeleteMultiSlotData(SceNpTusTransactionId transId, const SceNpId *targetNpId, const SceNpTusSlotId *slotIdArray, int32_t arrayNum, void *option);
int sceNpTusDeleteMultiSlotDataAsync(SceNpTusTransactionId transId, const SceNpId *targetNpId, const SceNpTusSlotId *slotIdArray, int32_t arrayNum, void *option);
int sceNpTusDeleteMultiSlotDataVUser(SceNpTusTransactionId transId, const SceNpTusVirtualUserId *targetVirtualUserId, const SceNpTusSlotId *slotIdArray, int32_t arrayNum, void *option);
int sceNpTusDeleteMultiSlotDataVUserAsync(SceNpTusTransactionId transId, const SceNpTusVirtualUserId *targetVirtualUserId, const SceNpTusSlotId *slotIdArray, int32_t arrayNum, void *option);

int sceNpTssGetData(SceNpTusTransactionId transId, SceNpTssSlotId slotId, SceNpTssDataStatus *dataStatus, uint32_t dataStatusSize, void *data, uint32_t recvSize, SceNpTssGetDataOptParam *option);
int sceNpTssGetDataAsync(SceNpTusTransactionId transId, SceNpTssSlotId slotId, SceNpTssDataStatus *dataStatus, uint32_t dataStatusSize, void *data, uint32_t recvSize, SceNpTssGetDataOptParam *option);
int sceNpTssGetDataNoLimit(void);
int sceNpTssGetDataNoLimitAsync(void);

#ifdef __cplusplus
}
#endif

#endif /* __PS3DK_CELL_NP_TUS_H__ */
