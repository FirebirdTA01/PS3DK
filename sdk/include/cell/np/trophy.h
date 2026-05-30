#ifndef __PS3DK_CELL_NP_TROPHY_H__
#define __PS3DK_CELL_NP_TROPHY_H__

#include <cell/np/common.h>
#include <cell/rtc.h>
#include <stdint.h>
#include <sys/memory.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SCE_NP_TROPHY_ERROR_ALREADY_INITIALIZED          0x80022901
#define SCE_NP_TROPHY_ERROR_NOT_INITIALIZED              0x80022902
#define SCE_NP_TROPHY_ERROR_NOT_SUPPORTED                0x80022903
#define SCE_NP_TROPHY_ERROR_CONTEXT_NOT_REGISTERED       0x80022904
#define SCE_NP_TROPHY_ERROR_OUT_OF_MEMORY                0x80022905
#define SCE_NP_TROPHY_ERROR_INVALID_ARGUMENT             0x80022906
#define SCE_NP_TROPHY_ERROR_EXCEEDS_MAX                  0x80022907
#define SCE_NP_TROPHY_ERROR_INSUFFICIENT                 0x80022909
#define SCE_NP_TROPHY_ERROR_UNKNOWN_CONTEXT              0x8002290a
#define SCE_NP_TROPHY_ERROR_INVALID_FORMAT               0x8002290b
#define SCE_NP_TROPHY_ERROR_BAD_RESPONSE                 0x8002290c
#define SCE_NP_TROPHY_ERROR_INVALID_GRADE                0x8002290d
#define SCE_NP_TROPHY_ERROR_INVALID_CONTEXT              0x8002290e
#define SCE_NP_TROPHY_ERROR_PROCESSING_ABORTED           0x8002290f
#define SCE_NP_TROPHY_ERROR_ABORT                        0x80022910
#define SCE_NP_TROPHY_ERROR_UNKNOWN_HANDLE               0x80022911
#define SCE_NP_TROPHY_ERROR_LOCKED                       0x80022912
#define SCE_NP_TROPHY_ERROR_HIDDEN                       0x80022913
#define SCE_NP_TROPHY_ERROR_CANNOT_UNLOCK_PLATINUM       0x80022914
#define SCE_NP_TROPHY_ERROR_ALREADY_UNLOCKED             0x80022915
#define SCE_NP_TROPHY_ERROR_INVALID_TYPE                 0x80022916
#define SCE_NP_TROPHY_ERROR_INVALID_HANDLE               0x80022917
#define SCE_NP_TROPHY_ERROR_INVALID_NP_COMM_ID           0x80022918
#define SCE_NP_TROPHY_ERROR_UNKNOWN_NP_COMM_ID           0x80022919
#define SCE_NP_TROPHY_ERROR_DISC_IO                      0x8002291a
#define SCE_NP_TROPHY_ERROR_CONF_DOES_NOT_EXIST          0x8002291b
#define SCE_NP_TROPHY_ERROR_UNSUPPORTED_FORMAT           0x8002291c
#define SCE_NP_TROPHY_ERROR_ALREADY_INSTALLED            0x8002291d
#define SCE_NP_TROPHY_ERROR_BROKEN_DATA                  0x8002291e
#define SCE_NP_TROPHY_ERROR_VERIFICATION_FAILURE         0x8002291f
#define SCE_NP_TROPHY_ERROR_INVALID_TROPHY_ID            0x80022920
#define SCE_NP_TROPHY_ERROR_UNKNOWN_TROPHY_ID            0x80022921
#define SCE_NP_TROPHY_ERROR_UNKNOWN_TITLE                0x80022922
#define SCE_NP_TROPHY_ERROR_UNKNOWN_FILE                 0x80022923
#define SCE_NP_TROPHY_ERROR_DISC_NOT_MOUNTED             0x80022924
#define SCE_NP_TROPHY_ERROR_SHUTDOWN                     0x80022925
#define SCE_NP_TROPHY_ERROR_TITLE_ICON_NOT_FOUND         0x80022926
#define SCE_NP_TROPHY_ERROR_TROPHY_ICON_NOT_FOUND        0x80022927
#define SCE_NP_TROPHY_ERROR_INSUFFICIENT_DISK_SPACE      0x80022928
#define SCE_NP_TROPHY_ERROR_ILLEGAL_UPDATE               0x8002292a
#define SCE_NP_TROPHY_ERROR_SAVEDATA_USER_DOES_NOT_MATCH 0x8002292b
#define SCE_NP_TROPHY_ERROR_TROPHY_ID_DOES_NOT_EXIST     0x8002292c
#define SCE_NP_TROPHY_ERROR_SERVICE_UNAVAILABLE          0x8002292d
#define SCE_NP_TROPHY_ERROR_UNKNOWN                      0x800229ff

#define SCE_NP_TROPHY_TITLE_MAX_SIZE      128
#define SCE_NP_TROPHY_GAME_DESCR_MAX_SIZE 1024
#define SCE_NP_TROPHY_NAME_MAX_SIZE       128
#define SCE_NP_TROPHY_NICKNAME_MAX_SIZE   SCE_NP_TROPHY_NAME_MAX_SIZE
#define SCE_NP_TROPHY_DESCR_MAX_SIZE      1024
#define SCE_NP_TROPHY_FLAG_SETSIZE        128
#define SCE_NP_TROPHY_FLAG_BITS_SHIFT     5

#define SCE_NP_TROPHY_INVALID_CONTEXT   0
#define SCE_NP_TROPHY_INVALID_HANDLE    0
#define SCE_NP_TROPHY_INVALID_TROPHY_ID 0xffffffffU

#define SCE_NP_TROPHY_OPTIONS_CREATE_CONTEXT_READ_ONLY          1
#define SCE_NP_TROPHY_OPTIONS_REGISTER_CONTEXT_SHOW_ERROR_EXIT  1

#define SCE_NP_TROPHY_UNLOCK_STATE_LOCKED   0
#define SCE_NP_TROPHY_UNLOCK_STATE_UNLOCKED 1
#define SCE_NP_TROPHY_UNLOCK_STATE_HIDDEN   2

typedef uint32_t SceNpTrophyContext;
typedef uint32_t SceNpTrophyHandle;
typedef int32_t SceNpTrophyId;

typedef enum SceNpTrophyGrade {
    SCE_NP_TROPHY_GRADE_UNKNOWN  = 0,
    SCE_NP_TROPHY_GRADE_PLATINUM = 1,
    SCE_NP_TROPHY_GRADE_GOLD     = 2,
    SCE_NP_TROPHY_GRADE_SILVER   = 3,
    SCE_NP_TROPHY_GRADE_BRONZE   = 4
} SceNpTrophyGrade;

typedef enum SceNpTrophyStatus {
    SCE_NP_TROPHY_STATUS_UNKNOWN             = 0,
    SCE_NP_TROPHY_STATUS_NOT_INSTALLED       = 1,
    SCE_NP_TROPHY_STATUS_DATA_CORRUPT        = 2,
    SCE_NP_TROPHY_STATUS_INSTALLED           = 3,
    SCE_NP_TROPHY_STATUS_REQUIRES_UPDATE     = 4,
    SCE_NP_TROPHY_STATUS_PROCESSING_SETUP    = 5,
    SCE_NP_TROPHY_STATUS_PROCESSING_PROGRESS = 6,
    SCE_NP_TROPHY_STATUS_PROCESSING_FINALIZE = 7,
    SCE_NP_TROPHY_STATUS_PROCESSING_COMPLETE = 8,
    SCE_NP_TROPHY_STATUS_CHANGES_DETECTED    = 9
} SceNpTrophyStatus;

typedef struct SceNpTrophyGameDetails {
    uint32_t numTrophies;
    uint32_t numPlatinum;
    uint32_t numGold;
    uint32_t numSilver;
    uint32_t numBronze;
    char title[SCE_NP_TROPHY_TITLE_MAX_SIZE];
    char description[SCE_NP_TROPHY_GAME_DESCR_MAX_SIZE];
    uint8_t reserved[4];
} SceNpTrophyGameDetails;

typedef struct SceNpTrophyGameData {
    uint32_t unlockedTrophies;
    uint32_t unlockedPlatinum;
    uint32_t unlockedGold;
    uint32_t unlockedSilver;
    uint32_t unlockedBronze;
} SceNpTrophyGameData;

typedef struct SceNpTrophyDetails {
    SceNpTrophyId trophyId;
    uint32_t trophyGrade;
    char name[SCE_NP_TROPHY_NAME_MAX_SIZE];
    char description[SCE_NP_TROPHY_DESCR_MAX_SIZE];
    uint8_t hidden;
    uint8_t reserved[3];
} SceNpTrophyDetails;

typedef struct SceNpTrophyData {
    CellRtcTick timestamp;
    SceNpTrophyId trophyId;
    uint8_t unlocked;
    uint8_t reserved[3];
} SceNpTrophyData;

typedef struct SceNpTrophyFlagArray {
    uint32_t flagBits[SCE_NP_TROPHY_FLAG_SETSIZE >> SCE_NP_TROPHY_FLAG_BITS_SHIFT];
} SceNpTrophyFlagArray;

typedef int32_t (*SceNpTrophyStatusCallback)(SceNpTrophyContext context, uint32_t status, int32_t completed, int32_t total, void *arg);

int sceNpTrophyInit(void *pool, uint32_t poolSize, sys_memory_container_t containerId, uint64_t options);
int sceNpTrophyTerm(void);
int sceNpTrophyCreateHandle(SceNpTrophyHandle *handle);
int sceNpTrophyDestroyHandle(SceNpTrophyHandle handle);
int sceNpTrophyAbortHandle(SceNpTrophyHandle handle);
int sceNpTrophyCreateContext(SceNpTrophyContext *context, const SceNpCommunicationId *commId, const SceNpCommunicationSignature *commSign, uint64_t options);
int sceNpTrophyDestroyContext(SceNpTrophyContext context);
int sceNpTrophyRegisterContext(SceNpTrophyContext context, SceNpTrophyHandle handle, SceNpTrophyStatusCallback statusCb, void *arg, uint64_t options);
int sceNpTrophyGetRequiredDiskSpace(SceNpTrophyContext context, SceNpTrophyHandle handle, uint64_t *requiredSpace, uint64_t options);
int sceNpTrophySetSoundLevel(SceNpTrophyContext context, SceNpTrophyHandle handle, uint32_t level, uint64_t options);
int sceNpTrophyGetGameInfo(SceNpTrophyContext context, SceNpTrophyHandle handle, SceNpTrophyGameDetails *details, SceNpTrophyGameData *data);
int sceNpTrophyUnlockTrophy(SceNpTrophyContext context, SceNpTrophyHandle handle, SceNpTrophyId trophyId, uint32_t *platinumId);
int sceNpTrophyGetTrophyUnlockState(SceNpTrophyContext context, SceNpTrophyHandle handle, SceNpTrophyFlagArray *flags, uint32_t *count);
int sceNpTrophyGetTrophyInfo(SceNpTrophyContext context, SceNpTrophyHandle handle, SceNpTrophyId trophyId, SceNpTrophyDetails *details, SceNpTrophyData *data);
int sceNpTrophyGetGameProgress(SceNpTrophyContext context, SceNpTrophyHandle handle, int32_t *percentage);
int sceNpTrophyGetGameIcon(SceNpTrophyContext context, SceNpTrophyHandle handle, void *buffer, uint32_t *size);
int sceNpTrophyGetTrophyIcon(SceNpTrophyContext context, SceNpTrophyHandle handle, SceNpTrophyId trophyId, void *buffer, uint32_t *size);

#ifdef __cplusplus
}
#endif

#endif /* __PS3DK_CELL_NP_TROPHY_H__ */
