#ifndef __PS3DK_CELL_NP_COMMON_H__
#define __PS3DK_CELL_NP_COMMON_H__

#include <cell/rtc.h>
#include <ppu-types.h>
#include <stdint.h>
#include <stddef.h>
#include <sys/sys_types.h>

#ifndef ATTRIBUTE_PRXPTR
#define ATTRIBUTE_PRXPTR
#endif

#ifdef __cplusplus
extern "C" {
#endif

#define SCE_NET_NP_ONLINEID_MIN_LENGTH 3
#define SCE_NET_NP_ONLINEID_MAX_LENGTH 16
#define SCE_NET_NP_ONLINENAME_MAX_LENGTH 48
#define SCE_NET_NP_AVATAR_URL_MAX_LENGTH 127
#define SCE_NET_NP_ABOUT_ME_MAX_LENGTH 63
#define SCE_NET_NP_AVATAR_IMAGE_MAX_SIZE_LARGE (200U * 1024U)
#define SCE_NET_NP_AVATAR_IMAGE_MAX_SIZE_MIDDLE (100U * 1024U)
#define SCE_NET_NP_AVATAR_IMAGE_MAX_SIZE_SMALL (10U * 1024U)
#define SCE_NET_NP_AVATAR_IMAGE_MAX_SIZE SCE_NET_NP_AVATAR_IMAGE_MAX_SIZE_LARGE

#define SCE_NP_COMMUNICATION_ID_LENGTH 9
#define SCE_NP_COMMUNICATION_PASSPHRASE_SIZE 128
#define SCE_NP_COMMUNICATION_PASSPHRASE_LENGTH SCE_NP_COMMUNICATION_PASSPHRASE_SIZE
#define SCE_NET_NP_COMMUNICATION_PASSPHRASE_SIZE SCE_NP_COMMUNICATION_PASSPHRASE_SIZE
#define SCE_NP_COMMUNICATION_SIGNATURE_SIZE 160
#define SCE_NP_COMMUNICATION_SIGNATURE_LENGTH SCE_NP_COMMUNICATION_SIGNATURE_SIZE
#define SCE_NP_TITLE_SECRET_SIZE 128

#define SCE_NP_TICKET_MAX_SIZE 65536
#define SCE_NP_TICKET_SERIAL_ID_SIZE 20
#define SCE_NP_SUBJECT_REGION_SIZE 4
#define SCE_NP_SUBJECT_DOMAIN_SIZE 4
#define SCE_NP_SERVICE_ID_SIZE 24
#define SCE_NP_ENTITLEMENT_ID_SIZE 32
#define SCE_NP_COOKIE_MAX_SIZE 1024
#define SCE_NP_TICKET_PARAM_DATA_LEN 256

#define SCE_NP_ERROR_NOT_INITIALIZED            0x8002aa01
#define SCE_NP_ERROR_ALREADY_INITIALIZED        0x8002aa02
#define SCE_NP_ERROR_INVALID_ARGUMENT           0x8002aa03
#define SCE_NP_ERROR_OUT_OF_MEMORY              0x8002aa04
#define SCE_NP_ERROR_ID_NO_SPACE                0x8002aa05
#define SCE_NP_ERROR_ID_NOT_FOUND               0x8002aa06
#define SCE_NP_ERROR_SESSION_RUNNING            0x8002aa07
#define SCE_NP_ERROR_LOGINID_ALREADY_EXISTS     0x8002aa08
#define SCE_NP_ERROR_INVALID_TICKET_SIZE        0x8002aa09
#define SCE_NP_ERROR_INVALID_STATE              0x8002aa0a
#define SCE_NP_ERROR_ABORTED                    0x8002aa0b
#define SCE_NP_ERROR_OFFLINE                    0x8002aa0c
#define SCE_NP_ERROR_VARIANT_ACCOUNT_ID         0x8002aa0d
#define SCE_NP_ERROR_GET_CLOCK                  0x8002aa0e
#define SCE_NP_ERROR_INSUFFICIENT_BUFFER        0x8002aa0f
#define SCE_NP_ERROR_EXPIRED_TICKET             0x8002aa10
#define SCE_NP_ERROR_TICKET_PARAM_NOT_FOUND     0x8002aa11
#define SCE_NP_ERROR_UNSUPPORTED_TICKET_VERSION 0x8002aa12
#define SCE_NP_ERROR_TICKET_STATUS_CODE_INVALID 0x8002aa13
#define SCE_NP_ERROR_INVALID_TICKET_VERSION     0x8002aa14
#define SCE_NP_ERROR_ALREADY_USED               0x8002aa15
#define SCE_NP_ERROR_DIFFERENT_USER             0x8002aa16
#define SCE_NP_ERROR_ALREADY_DONE               0x8002aa17
#define SCE_NP_ERROR_INTERNAL                   0x8002aaff

#define CELL_NP_ERROR_NOT_INITIALIZED     SCE_NP_ERROR_NOT_INITIALIZED
#define CELL_NP_ERROR_ALREADY_INITIALIZED SCE_NP_ERROR_ALREADY_INITIALIZED
#define CELL_NP_ERROR_INVALID_ARGUMENT    SCE_NP_ERROR_INVALID_ARGUMENT
#define CELL_NP_ERROR_OUT_OF_MEMORY       SCE_NP_ERROR_OUT_OF_MEMORY

typedef int SceNpPlatformType;

#define SCE_NP_PLATFORM_TYPE_NONE 0
#define SCE_NP_PLATFORM_TYPE_PS3  1
#define SCE_NP_PLATFORM_TYPE_VITA 2
#define SCE_NP_PLATFORM_TYPE_PS4  3

typedef struct SceNpOnlineId {
    char data[SCE_NET_NP_ONLINEID_MAX_LENGTH];
    char term;
    char dummy[3];
} SceNpOnlineId;

typedef SceNpOnlineId SceNpPsHandle;

typedef struct SceNpId {
    SceNpOnlineId handle;
    uint8_t opt[8];
    uint8_t reserved[8];
} SceNpId;

typedef struct SceNpCommunicationId {
    char data[SCE_NP_COMMUNICATION_ID_LENGTH];
    char term;
    uint8_t num;
    char dummy;
} SceNpCommunicationId;

typedef SceNpCommunicationId SceNpTitleId;

typedef struct SceNpCommunicationPassphrase {
    uint8_t data[SCE_NP_COMMUNICATION_PASSPHRASE_SIZE];
} SceNpCommunicationPassphrase;

typedef struct SceNpCommunicationSignature {
    uint8_t data[SCE_NP_COMMUNICATION_SIGNATURE_SIZE];
} SceNpCommunicationSignature;

typedef struct SceNpTitleSecret {
    uint8_t data[SCE_NP_TITLE_SECRET_SIZE];
} SceNpTitleSecret;

typedef struct SceNpOnlineName {
    char data[SCE_NET_NP_ONLINENAME_MAX_LENGTH];
    char term;
    char padding[3];
} SceNpOnlineName;

typedef SceNpOnlineName SceNpSubHandle;

typedef struct SceNpAvatarUrl {
    char data[SCE_NET_NP_AVATAR_URL_MAX_LENGTH];
    char term;
} SceNpAvatarUrl;

typedef struct SceNpUserInfo {
    SceNpId userId;
    SceNpOnlineName name;
    SceNpAvatarUrl icon;
} SceNpUserInfo;

typedef struct SceNpUserInfo2 {
    SceNpId npId;
    SceNpOnlineName *onlineName ATTRIBUTE_PRXPTR;
    SceNpAvatarUrl *avatarUrl ATTRIBUTE_PRXPTR;
} SceNpUserInfo2;

typedef struct SceNpLobbyId {
    uint8_t opt[28];
    uint8_t reserved[8];
} SceNpLobbyId;

typedef struct SceNpRoomId {
    uint8_t opt[28];
    uint8_t reserved[8];
} SceNpRoomId;

typedef struct SceNpMyLanguages {
    int32_t language1;
    int32_t language2;
    int32_t language3;
    uint8_t padding[4];
} SceNpMyLanguages;

typedef struct SceNpAvatarImage {
    uint8_t data[SCE_NET_NP_AVATAR_IMAGE_MAX_SIZE];
    uint32_t size;
    uint8_t reserved[12];
} SceNpAvatarImage;

typedef struct SceNpAboutMe {
    char data[SCE_NET_NP_ABOUT_ME_MAX_LENGTH];
    char term;
} SceNpAboutMe;

typedef struct SceNpManagerCacheParam {
    uint32_t size;
    SceNpOnlineId onlineId;
    SceNpId npId;
    SceNpOnlineName onlineName;
    SceNpAvatarUrl avatarUrl;
} SceNpManagerCacheParam;

typedef struct SceNpCountryCode {
    char data[2];
    char term;
    char padding[1];
} SceNpCountryCode;

typedef struct SceNpDate {
    uint16_t year;
    uint8_t month;
    uint8_t day;
} SceNpDate;

typedef int64_t SceNpTime;

typedef struct SceNpTicketVersion {
    uint16_t major;
    uint16_t minor;
} SceNpTicketVersion;

typedef union SceNpTicketParam {
    int32_t i32;
    int64_t i64;
    uint32_t u32;
    uint64_t u64;
    SceNpDate date;
    uint8_t data[SCE_NP_TICKET_PARAM_DATA_LEN];
} SceNpTicketParam;

typedef enum SceNpTicketParamId {
    SCE_NP_TICKET_PARAM_SERIAL_ID = 0,
    SCE_NP_TICKET_PARAM_ISSUER_ID,
    SCE_NP_TICKET_PARAM_ISSUED_DATE,
    SCE_NP_TICKET_PARAM_EXPIRE_DATE,
    SCE_NP_TICKET_PARAM_SUBJECT_ACCOUNT_ID,
    SCE_NP_TICKET_PARAM_SUBJECT_ONLINE_ID,
    SCE_NP_TICKET_PARAM_SUBJECT_REGION,
    SCE_NP_TICKET_PARAM_SUBJECT_DOMAIN,
    SCE_NP_TICKET_PARAM_SERVICE_ID,
    SCE_NP_TICKET_PARAM_SUBJECT_STATUS,
    SCE_NP_TICKET_PARAM_STATUS_DURATION,
    SCE_NP_TICKET_PARAM_SUBJECT_DOB,
    SCE_NP_TICKET_PARAM_MAX
} SceNpTicketParamId;

typedef struct SceNpEntitlementId {
    uint8_t data[SCE_NP_ENTITLEMENT_ID_SIZE];
} SceNpEntitlementId;

typedef enum SceNpEntitlementType {
    SCE_NP_ENTITLEMENT_TYPE_NON_CONSUMABLE = 0,
    SCE_NP_ENTITLEMENT_TYPE_CONSUMABLE = 1
} SceNpEntitlementType;

typedef struct SceNpEntitlement {
    SceNpEntitlementId id;
    SceNpTime created_date;
    SceNpTime expire_date;
    uint32_t type;
    int32_t remaining_count;
    uint32_t consumed_count;
    uint8_t padding[4];
} SceNpEntitlement;

typedef enum SceNpAvatarSizeType {
    SCE_NP_AVATAR_SIZE_LARGE = 0,
    SCE_NP_AVATAR_SIZE_MIDDLE = 1,
    SCE_NP_AVATAR_SIZE_SMALL = 2
} SceNpAvatarSizeType;

#ifdef __cplusplus
}
#endif

#endif /* __PS3DK_CELL_NP_COMMON_H__ */
