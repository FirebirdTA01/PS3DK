#ifndef __PS3DK_CELL_NP_COMMERCE2_H__
#define __PS3DK_CELL_NP_COMMERCE2_H__

#include <cell/np/common.h>
#include <cell/rtc.h>
#include <stdint.h>
#include <sys/memory.h>

#ifndef ATTRIBUTE_PRXPTR
#define ATTRIBUTE_PRXPTR
#endif

#ifdef __cplusplus
extern "C" {
#endif

#define SCE_NP_COMMERCE2_ERROR_NOT_INITIALIZED                      0x80023001
#define SCE_NP_COMMERCE2_ERROR_ALREADY_INITIALIZED                  0x80023002
#define SCE_NP_COMMERCE2_ERROR_INVALID_ARGUMENT                     0x80023003
#define SCE_NP_COMMERCE2_ERROR_UNSUPPORTED_VERSION                  0x80023004
#define SCE_NP_COMMERCE2_ERROR_CTX_MAX                              0x80023005
#define SCE_NP_COMMERCE2_ERROR_INVALID_INDEX                        0x80023006
#define SCE_NP_COMMERCE2_ERROR_INVALID_SKUID                        0x80023007
#define SCE_NP_COMMERCE2_ERROR_INVALID_SKU_NUM                      0x80023008
#define SCE_NP_COMMERCE2_ERROR_INVALID_MEMORY_CONTAINER             0x80023009
#define SCE_NP_COMMERCE2_ERROR_INSUFFICIENT_MEMORY_CONTAINER        0x8002300a
#define SCE_NP_COMMERCE2_ERROR_OUT_OF_MEMORY                        0x8002300b
#define SCE_NP_COMMERCE2_ERROR_CTX_NOT_FOUND                        0x8002300c
#define SCE_NP_COMMERCE2_ERROR_CTXID_NOT_AVAILABLE                  0x8002300d
#define SCE_NP_COMMERCE2_ERROR_REQ_NOT_FOUND                        0x8002300e
#define SCE_NP_COMMERCE2_ERROR_REQID_NOT_AVAILABLE                  0x8002300f
#define SCE_NP_COMMERCE2_ERROR_ABORTED                              0x80023010
#define SCE_NP_COMMERCE2_ERROR_RESPONSE_BUF_TOO_SMALL               0x80023012
#define SCE_NP_COMMERCE2_ERROR_COULD_NOT_RECV_WHOLE_RESPONSE_DATA   0x80023013
#define SCE_NP_COMMERCE2_ERROR_INVALID_RESULT_DATA                  0x80023014
#define SCE_NP_COMMERCE2_ERROR_UNKNOWN                              0x80023015
#define SCE_NP_COMMERCE2_ERROR_SERVER_MAINTENANCE                   0x80023016
#define SCE_NP_COMMERCE2_ERROR_SERVER_UNKNOWN                       0x80023017
#define SCE_NP_COMMERCE2_ERROR_INSUFFICIENT_BUF_SIZE                0x80023018
#define SCE_NP_COMMERCE2_ERROR_REQ_MAX                              0x80023019
#define SCE_NP_COMMERCE2_ERROR_INVALID_TARGET_TYPE                  0x8002301a
#define SCE_NP_COMMERCE2_ERROR_INVALID_TARGET_ID                    0x8002301b
#define SCE_NP_COMMERCE2_ERROR_INVALID_SIZE                         0x8002301c
#define SCE_NP_COMMERCE2_ERROR_DATA_NOT_FOUND                       0x80023087
#define SCE_NP_COMMERCE2_SERVER_ERROR_BAD_REQUEST                   0x80023101
#define SCE_NP_COMMERCE2_SERVER_ERROR_UNKNOWN_ERROR                 0x80023102
#define SCE_NP_COMMERCE2_SERVER_ERROR_SESSION_EXPIRED               0x80023105
#define SCE_NP_COMMERCE2_SERVER_ERROR_ACCESS_PERMISSION_DENIED      0x80023107
#define SCE_NP_COMMERCE2_SERVER_ERROR_NO_SUCH_CATEGORY              0x80023110
#define SCE_NP_COMMERCE2_SERVER_ERROR_NO_SUCH_PRODUCT               0x80023111
#define SCE_NP_COMMERCE2_SERVER_ERROR_NOT_ELIGIBILITY               0x80023113
#define SCE_NP_COMMERCE2_SERVER_ERROR_INVALID_SKU                   0x8002311a
#define SCE_NP_COMMERCE2_SERVER_ERROR_ACCOUNT_SUSPENDED1            0x8002311b
#define SCE_NP_COMMERCE2_SERVER_ERROR_ACCOUNT_SUSPENDED2            0x8002311c
#define SCE_NP_COMMERCE2_SERVER_ERROR_OVER_SPENDING_LIMIT           0x80023120
#define SCE_NP_COMMERCE2_SERVER_ERROR_INVALID_VOUCHER               0x8002312f
#define SCE_NP_COMMERCE2_SERVER_ERROR_VOUCHER_ALREADY_CONSUMED      0x80023130
#define SCE_NP_COMMERCE2_SERVER_ERROR_EXCEEDS_AGE_LIMIT_IN_BROWSING 0x80023139
#define SCE_NP_COMMERCE2_SYSTEM_UTIL_ERROR_INVALID_VOUCHER          0x80024002

#define SCE_NP_COMMERCE2_EVENT_REQUEST_ERROR            0x0001
#define SCE_NP_COMMERCE2_EVENT_CREATE_SESSION_DONE      0x0011
#define SCE_NP_COMMERCE2_EVENT_CREATE_SESSION_ABORT     0x0012
#define SCE_NP_COMMERCE2_EVENT_DO_CHECKOUT_STARTED      0x0021
#define SCE_NP_COMMERCE2_EVENT_DO_CHECKOUT_SUCCESS      0x0022
#define SCE_NP_COMMERCE2_EVENT_DO_CHECKOUT_BACK         0x0023
#define SCE_NP_COMMERCE2_EVENT_DO_CHECKOUT_FINISHED     0x0024
#define SCE_NP_COMMERCE2_EVENT_DO_DL_LIST_STARTED       0x0031
#define SCE_NP_COMMERCE2_EVENT_DO_DL_LIST_SUCCESS       0x0032
#define SCE_NP_COMMERCE2_EVENT_DO_DL_LIST_FINISHED      0x0034
#define SCE_NP_COMMERCE2_EVENT_DO_PROD_BROWSE_STARTED   0x0041
#define SCE_NP_COMMERCE2_EVENT_DO_PROD_BROWSE_SUCCESS   0x0042
#define SCE_NP_COMMERCE2_EVENT_DO_PROD_BROWSE_BACK      0x0043
#define SCE_NP_COMMERCE2_EVENT_DO_PROD_BROWSE_FINISHED  0x0044
#define SCE_NP_COMMERCE2_EVENT_DO_PROD_BROWSE_OPENED    0x0045
#define SCE_NP_COMMERCE2_EVENT_DO_PRODUCT_CODE_STARTED  0x0051
#define SCE_NP_COMMERCE2_EVENT_DO_PRODUCT_CODE_SUCCESS  0x0052
#define SCE_NP_COMMERCE2_EVENT_DO_PRODUCT_CODE_BACK     0x0053
#define SCE_NP_COMMERCE2_EVENT_DO_PRODUCT_CODE_FINISHED 0x0054
#define SCE_NP_COMMERCE2_EVENT_EMPTY_STORE_CHECK_DONE   0x0061
#define SCE_NP_COMMERCE2_EVENT_EMPTY_STORE_CHECK_ABORT  0x0062

#define SCE_NP_COMMERCE2_CAT_DATA_TYPE_THIN 0
#define SCE_NP_COMMERCE2_CAT_DATA_TYPE_NORMAL 1
#define SCE_NP_COMMERCE2_CAT_DATA_TYPE_MAX 2
#define SCE_NP_COMMERCE2_GAME_PRODUCT_DATA_TYPE_THIN 0
#define SCE_NP_COMMERCE2_GAME_PRODUCT_DATA_TYPE_NORMAL 1
#define SCE_NP_COMMERCE2_GAME_PRODUCT_DATA_TYPE_MAX 2
#define SCE_NP_COMMERCE2_GAME_SKU_DATA_TYPE_THIN 0
#define SCE_NP_COMMERCE2_GAME_SKU_DATA_TYPE_NORMAL 1
#define SCE_NP_COMMERCE2_GAME_SKU_DATA_TYPE_MAX 2

#define SCE_NP_COMMERCE2_STORE_IS_NOT_EMPTY 0
#define SCE_NP_COMMERCE2_STORE_IS_EMPTY 1
#define SCE_NP_COMMERCE2_STORE_CHECK_TYPE_CATEGORY 1
#define SCE_NP_COMMERCE2_STORE_BROWSE_TYPE_CATEGORY 1
#define SCE_NP_COMMERCE2_STORE_BROWSE_TYPE_PRODUCT 2
#define SCE_NP_COMMERCE2_STORE_BROWSE_TYPE_PRODUCT_CODE 3
#define SCE_NP_COMMERCE2_CONTENT_TYPE_CATEGORY 1
#define SCE_NP_COMMERCE2_CONTENT_TYPE_PRODUCT 2
#define SCE_NP_COMMERCE2_CONTENT_RATING_DESC_TYPE_ICON 1
#define SCE_NP_COMMERCE2_CONTENT_RATING_DESC_TYPE_TEXT 2

#define SCE_NP_COMMERCE2_SKU_CHECKOUT_MAX 16
#define SCE_NP_COMMERCE2_SKU_DL_LIST_MAX 16
#define SCE_NP_COMMERCE2_SKU_PURCHASABILITY_FLAG_ON 1
#define SCE_NP_COMMERCE2_SKU_PURCHASABILITY_FLAG_OFF 0
#define SCE_NP_COMMERCE2_SKU_ANN_PURCHASED_CANNOT_PURCHASE_AGAIN 0x80000000
#define SCE_NP_COMMERCE2_SKU_ANN_PURCHASED_CAN_PURCHASE_AGAIN 0x40000000
#define SCE_NP_COMMERCE2_SKU_ANN_IN_THE_CART 0x20000000
#define SCE_NP_COMMERCE2_SKU_ANN_CONTENTLINK_SKU 0x10000000
#define SCE_NP_COMMERCE2_SKU_ANN_CREDIT_CARD_REQUIRED 0x08000000
#define SCE_NP_COMMERCE2_SKU_ANN_CHARGE_IMMEDIATELY 0x04000000

#define SCE_NP_COMMERCE2_VERSION 2
#define SCE_NP_COMMERCE2_CTX_MAX 1
#define SCE_NP_COMMERCE2_REQ_MAX 1
#define SCE_NP_COMMERCE2_CURRENCY_CODE_LEN 3
#define SCE_NP_COMMERCE2_CURRENCY_SYMBOL_LEN 3
#define SCE_NP_COMMERCE2_THOUSAND_SEPARATOR_LEN 4
#define SCE_NP_COMMERCE2_DECIMAL_LETTER_LEN 4
#define SCE_NP_COMMERCE2_SP_NAME_LEN 256
#define SCE_NP_COMMERCE2_CATEGORY_ID_LEN 56
#define SCE_NP_COMMERCE2_CATEGORY_NAME_LEN 256
#define SCE_NP_COMMERCE2_CATEGORY_DESCRIPTION_LEN 1024
#define SCE_NP_COMMERCE2_PRODUCT_ID_LEN 48
#define SCE_NP_COMMERCE2_PRODUCT_NAME_LEN 256
#define SCE_NP_COMMERCE2_PRODUCT_SHORT_DESCRIPTION_LEN 1024
#define SCE_NP_COMMERCE2_PRODUCT_LONG_DESCRIPTION_LEN 4000
#define SCE_NP_COMMERCE2_SKU_ID_LEN 56
#define SCE_NP_COMMERCE2_SKU_NAME_LEN 180
#define SCE_NP_COMMERCE2_URL_LEN 256
#define SCE_NP_COMMERCE2_RATING_SYSTEM_ID_LEN 16
#define SCE_NP_COMMERCE2_RATING_DESCRIPTION_LEN 60
#define SCE_NP_COMMERCE2_RECV_BUF_SIZE 262144
#define SCE_NP_COMMERCE2_PRODUCT_CODE_BLOCK_LEN 4
#define SCE_NP_COMMERCE2_PRODUCT_CODE_INPUT_MODE_USER_INPUT 0
#define SCE_NP_COMMERCE2_PRODUCT_CODE_INPUT_MODE_CODE_SPECIFIED 1
#define SCE_NP_COMMERCE2_GETCAT_MAX_COUNT 60
#define SCE_NP_COMMERCE2_GETPRODLIST_MAX_COUNT 60
#define SCE_NP_COMMERCE2_DO_CHECKOUT_MEMORY_CONTAINER_SIZE 10485760
#define SCE_NP_COMMERCE2_DO_PROD_BROWSE_MEMORY_CONTAINER_SIZE 16777216
#define SCE_NP_COMMERCE2_DO_DL_LIST_MEMORY_CONTAINER_SIZE 10485760
#define SCE_NP_COMMERCE2_DO_PRODUCT_CODE_MEMORY_CONTAINER_SIZE 16777216
#define SCE_NP_COMMERCE2_SYM_POS_PRE 0
#define SCE_NP_COMMERCE2_SYM_POS_POST 1

typedef uint32_t SceNpCommerce2CtxId;
typedef uint32_t SceNpCommerce2SessionId;
typedef uint32_t SceNpCommerce2ReqId;
typedef uint32_t SceNpCommerce2CategoryDataType;
typedef uint32_t SceNpCommerce2GameProductDataType;
typedef uint32_t SceNpCommerce2GameSkuDataType;
typedef void (*SceNpCommerce2Handler)(uint32_t ctxId, uint32_t subjectId, int32_t event, int32_t errorCode, void *arg);

typedef struct SceNpCommerce2CommonData {
    uint32_t version;
    uint32_t bufHead;
    uint32_t bufSize;
    uint32_t data;
    uint32_t dataSize;
    uint32_t data2;
    uint32_t reserved[4];
} SceNpCommerce2CommonData;

typedef struct SceNpCommerce2Range {
    uint32_t startPosition;
    uint32_t count;
    uint32_t totalCountOfResults;
} SceNpCommerce2Range;

typedef struct SceNpCommerce2SessionInfo {
    char currencyCode[SCE_NP_COMMERCE2_CURRENCY_CODE_LEN + 1];
    uint32_t decimals;
    char currencySymbol[SCE_NP_COMMERCE2_CURRENCY_SYMBOL_LEN + 1];
    uint32_t symbolPosition;
    uint8_t symbolWithSpace;
    uint8_t padding1[3];
    char thousandSeparator[SCE_NP_COMMERCE2_THOUSAND_SEPARATOR_LEN + 1];
    char decimalLetter[SCE_NP_COMMERCE2_DECIMAL_LETTER_LEN + 1];
    uint8_t padding2[1];
    uint32_t reserved[4];
} SceNpCommerce2SessionInfo;

typedef struct SceNpCommerce2CategoryInfo {
    SceNpCommerce2CommonData commonData;
    uint32_t dataType;
    int8_t categoryId;
    uint8_t padding1[7];
    CellRtcTick releaseDate;
    int8_t categoryName;
    int8_t categoryDescription;
    int8_t imageUrl;
    int8_t spName;
    uint32_t countOfSubCategory;
    uint32_t countOfProduct;
} SceNpCommerce2CategoryInfo;

typedef struct SceNpCommerce2ContentInfo {
    SceNpCommerce2CommonData commonData;
    uint32_t contentType;
} SceNpCommerce2ContentInfo;

typedef struct SceNpCommerce2GetProductInfoResult {
    SceNpCommerce2CommonData commonData;
} SceNpCommerce2GetProductInfoResult;

typedef struct SceNpCommerce2GameProductInfo {
    SceNpCommerce2CommonData commonData;
    uint32_t dataType;
    int8_t productId;
    uint8_t padding1[3];
    uint32_t countOfSku;
    uint8_t padding2[4];
    CellRtcTick releaseDate;
    int8_t productName;
    int8_t productShortDescription;
    int8_t imageUrl;
    int8_t spName;
    int8_t productLongDescription;
    int8_t legalDescription;
    uint8_t padding3[2];
} SceNpCommerce2GameProductInfo;

typedef struct SceNpCommerce2GetProductInfoListResult {
    SceNpCommerce2CommonData commonData;
    uint32_t countOfProductInfo;
} SceNpCommerce2GetProductInfoListResult;

typedef struct SceNpCommerce2ContentRatingInfo {
    SceNpCommerce2CommonData commonData;
    int8_t ratingSystemId;
    int8_t imageUrl;
    uint8_t padding1[2];
    uint32_t countOfContentRatingDescriptor;
} SceNpCommerce2ContentRatingInfo;

typedef struct SceNpCommerce2ContentRatingDescriptor {
    SceNpCommerce2CommonData commonData;
    uint32_t descriptorType;
    int8_t imageUrl;
    int8_t contentRatingDescription;
    uint8_t padding1[2];
} SceNpCommerce2ContentRatingDescriptor;

typedef struct SceNpCommerce2GameSkuInfo {
    SceNpCommerce2CommonData commonData;
    uint32_t dataType;
    int8_t skuId;
    uint8_t padding1[3];
    uint32_t skuType;
    uint32_t countUntilExpiration;
    uint32_t timeUntilExpiration;
    uint32_t purchasabilityFlag;
    uint32_t annotation;
    uint8_t downloadable;
    uint8_t padding2[3];
    uint32_t price;
    int8_t skuName;
    int8_t productId;
    int8_t contentLinkUrl;
    uint8_t padding3[1];
    uint32_t countOfRewardInfo;
    uint32_t reserved[8];
} SceNpCommerce2GameSkuInfo;

typedef struct SceNpCommerce2ProductBrowseParam {
    uint32_t size;
} SceNpCommerce2ProductBrowseParam;

typedef struct SceNpCommerce2ProductCodeParam {
    uint32_t size;
    uint32_t inputMode;
    char code1[SCE_NP_COMMERCE2_PRODUCT_CODE_BLOCK_LEN + 1];
    uint8_t padding1[3];
    char code2[SCE_NP_COMMERCE2_PRODUCT_CODE_BLOCK_LEN + 1];
    uint8_t padding2[3];
    char code3[SCE_NP_COMMERCE2_PRODUCT_CODE_BLOCK_LEN + 1];
    uint8_t padding3[3];
} SceNpCommerce2ProductCodeParam;

typedef struct SceNpCommerce2GetCategoryContentsResult {
    SceNpCommerce2CommonData commonData;
    SceNpCommerce2Range rangeOfContents;
} SceNpCommerce2GetCategoryContentsResult;

int sceNpCommerce2Init(void);
int sceNpCommerce2Term(void);
int sceNpCommerce2CreateCtx(uint32_t version, const SceNpId *npId, SceNpCommerce2Handler handler, void *arg, SceNpCommerce2CtxId *ctxId);
int sceNpCommerce2DestroyCtx(SceNpCommerce2CtxId ctxId);
int sceNpCommerce2CreateSessionStart(SceNpCommerce2CtxId ctxId);
int sceNpCommerce2CreateSessionAbort(SceNpCommerce2CtxId ctxId);
int sceNpCommerce2CreateSessionFinish(SceNpCommerce2CtxId ctxId, SceNpCommerce2SessionInfo *sessionInfo);
int sceNpCommerce2EmptyStoreCheckStart(SceNpCommerce2CtxId ctxId, int32_t storeCheckType, const char *targetId);
int sceNpCommerce2EmptyStoreCheckAbort(SceNpCommerce2CtxId ctxId);
int sceNpCommerce2EmptyStoreCheckFinish(SceNpCommerce2CtxId ctxId, int32_t *isEmpty);
int sceNpCommerce2GetCategoryContentsCreateReq(SceNpCommerce2CtxId ctxId, SceNpCommerce2ReqId *reqId);
int sceNpCommerce2GetCategoryContentsStart(SceNpCommerce2ReqId reqId, const char *categoryId, uint32_t startPosition, uint32_t maxCountOfResults);
int sceNpCommerce2GetCategoryContentsGetResult(SceNpCommerce2ReqId reqId, void *buf, uint32_t bufSize, uint32_t *fillSize);
int sceNpCommerce2InitGetCategoryContentsResult(SceNpCommerce2GetCategoryContentsResult *result, void *data, uint32_t dataSize);
int sceNpCommerce2DestroyGetCategoryContentsResult(SceNpCommerce2GetCategoryContentsResult *result);
int sceNpCommerce2GetCategoryInfo(const SceNpCommerce2GetCategoryContentsResult *result, SceNpCommerce2CategoryInfo *categoryInfo);
int sceNpCommerce2GetContentInfo(const SceNpCommerce2GetCategoryContentsResult *result, uint32_t index, SceNpCommerce2ContentInfo *contentInfo);
int sceNpCommerce2GetCategoryInfoFromContentInfo(const SceNpCommerce2ContentInfo *contentInfo, SceNpCommerce2CategoryInfo *categoryInfo);
int sceNpCommerce2GetGameProductInfoFromContentInfo(const SceNpCommerce2ContentInfo *contentInfo, SceNpCommerce2GameProductInfo *gameProductInfo);
int sceNpCommerce2GetProductInfoCreateReq(SceNpCommerce2CtxId ctxId, SceNpCommerce2ReqId *reqId);
int sceNpCommerce2GetProductInfoStart(SceNpCommerce2ReqId reqId, const char *categoryId, const char *productId);
int sceNpCommerce2GetProductInfoGetResult(SceNpCommerce2ReqId reqId, void *buf, uint32_t bufSize, uint32_t *fillSize);
int sceNpCommerce2InitGetProductInfoResult(SceNpCommerce2GetProductInfoResult *result, void *data, uint32_t dataSize);
int sceNpCommerce2GetGameProductInfo(const SceNpCommerce2GetProductInfoResult *result, SceNpCommerce2GameProductInfo *gameProductInfo);
int sceNpCommerce2DestroyGetProductInfoResult(SceNpCommerce2GetProductInfoResult *result);
int sceNpCommerce2GetProductInfoListCreateReq(SceNpCommerce2CtxId ctxId, SceNpCommerce2ReqId *reqId);
int sceNpCommerce2GetProductInfoListStart(SceNpCommerce2ReqId reqId, const char **productIds, uint32_t productNum);
int sceNpCommerce2GetProductInfoListGetResult(SceNpCommerce2ReqId reqId, void *buf, uint32_t bufSize, uint32_t *fillSize);
int sceNpCommerce2InitGetProductInfoListResult(SceNpCommerce2GetProductInfoListResult *result, void *data, uint32_t dataSize);
int sceNpCommerce2GetGameProductInfoFromGetProductInfoListResult(const SceNpCommerce2GetProductInfoListResult *result, uint32_t index, SceNpCommerce2GameProductInfo *gameProductInfo);
int sceNpCommerce2DestroyGetProductInfoListResult(SceNpCommerce2GetProductInfoListResult *result);
int sceNpCommerce2GetContentRatingInfoFromGameProductInfo(const SceNpCommerce2GameProductInfo *gameProductInfo, SceNpCommerce2ContentRatingInfo *contentRatingInfo);
int sceNpCommerce2GetContentRatingInfoFromCategoryInfo(const SceNpCommerce2CategoryInfo *categoryInfo, SceNpCommerce2ContentRatingInfo *contentRatingInfo);
int sceNpCommerce2GetContentRatingDescriptor(const SceNpCommerce2ContentRatingInfo *contentRatingInfo, uint32_t index, SceNpCommerce2ContentRatingDescriptor *contentRatingDescriptor);
int sceNpCommerce2GetGameSkuInfoFromGameProductInfo(const SceNpCommerce2GameProductInfo *gameProductInfo, uint32_t index, SceNpCommerce2GameSkuInfo *gameSkuInfo);
int sceNpCommerce2GetPrice(SceNpCommerce2CtxId ctxId, char *buf, uint32_t bufLen, uint32_t price);
int sceNpCommerce2DoCheckoutStartAsync(SceNpCommerce2CtxId ctxId, const char **skuIds, uint32_t skuNum, sys_memory_container_t container);
int sceNpCommerce2DoCheckoutFinishAsync(SceNpCommerce2CtxId ctxId);
int sceNpCommerce2DoDlListStartAsync(SceNpCommerce2CtxId ctxId, const char *serviceId, const char **skuIds, uint32_t skuNum, sys_memory_container_t container);
int sceNpCommerce2DoDlListFinishAsync(SceNpCommerce2CtxId ctxId);
int sceNpCommerce2DoProductBrowseStartAsync(SceNpCommerce2CtxId ctxId, const char *productId, sys_memory_container_t container, const SceNpCommerce2ProductBrowseParam *param);
int sceNpCommerce2DoProductBrowseFinishAsync(SceNpCommerce2CtxId ctxId);
int sceNpCommerce2DoProductCodeStartAsync(SceNpCommerce2CtxId ctxId, sys_memory_container_t container, const SceNpCommerce2ProductCodeParam *param);
int sceNpCommerce2DoProductCodeFinishAsync(SceNpCommerce2CtxId ctxId);
int sceNpCommerce2DoServiceListStartAsync(SceNpCommerce2CtxId ctxId, char *serviceId, sys_memory_container_t container, SceNpCommerce2ProductBrowseParam *param);
int sceNpCommerce2DoServiceListFinishAsync(SceNpCommerce2CtxId ctxId);
int sceNpCommerce2ExecuteStoreBrowse(int32_t targetType, const char *targetId, int32_t userdata);
int sceNpCommerce2GetStoreBrowseUserdata(int32_t *userdata);
int sceNpCommerce2GetBGDLAvailability(uint8_t *bgdlAvailability);
int sceNpCommerce2SetBGDLAvailability(uint8_t bgdlAvailability);
int sceNpCommerce2AbortReq(SceNpCommerce2ReqId reqId);
int sceNpCommerce2DestroyReq(SceNpCommerce2ReqId reqId);

#ifdef __cplusplus
}
#endif

#endif /* __PS3DK_CELL_NP_COMMERCE2_H__ */
