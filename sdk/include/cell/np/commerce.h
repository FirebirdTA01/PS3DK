#ifndef __PS3DK_CELL_NP_COMMERCE_H__
#define __PS3DK_CELL_NP_COMMERCE_H__

#include <cell/np/common.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/memory.h>

#ifndef ATTRIBUTE_PRXPTR
#define ATTRIBUTE_PRXPTR
#endif

#ifdef __cplusplus
extern "C" {
#endif

#define SCE_NP_COMMERCE_VERSION 1

#define SCE_NP_COMMERCE_EVENT_PRODUCT_CATEGORY_DONE 1
#define SCE_NP_COMMERCE_EVENT_PRODUCT_CATEGORY_ABORTED 2
#define SCE_NP_COMMERCE_EVENT_CHECKOUT_DONE 3
#define SCE_NP_COMMERCE_EVENT_CHECKOUT_ABORTED 4
#define SCE_NP_COMMERCE_EVENT_DATA_FLAG_DONE 5
#define SCE_NP_COMMERCE_EVENT_DATA_FLAG_ABORTED 6

#define SCE_NP_COMMERCE_DATA_FLAG_DISABLE 0
#define SCE_NP_COMMERCE_DATA_FLAG_ENABLE 1

typedef uint32_t SceNpCommerceCtxId;
typedef uint32_t SceNpCommerceRequestId;

typedef void (*SceNpCommerceHandler)(uint32_t ctxId, uint32_t subjectId, int32_t event, int32_t errorCode, void *arg);

typedef struct SceNpCommerceProductCategory {
    uint32_t version;
    const void *data ATTRIBUTE_PRXPTR;
    uint32_t dataSize;
    uint32_t dval;
    uint8_t reserved[16];
} SceNpCommerceProductCategory;

typedef struct SceNpCommerceProductSkuInfo {
    SceNpCommerceProductCategory *pc ATTRIBUTE_PRXPTR;
    const void *data ATTRIBUTE_PRXPTR;
    uint8_t reserved[8];
} SceNpCommerceProductSkuInfo;

typedef struct SceNpCommerceCategoryInfo {
    SceNpCommerceProductCategory *pc ATTRIBUTE_PRXPTR;
    const void *data ATTRIBUTE_PRXPTR;
    uint8_t reserved[8];
} SceNpCommerceCategoryInfo;

typedef struct SceNpCommerceCurrencyInfo {
    SceNpCommerceProductCategory *pc ATTRIBUTE_PRXPTR;
    const void *data ATTRIBUTE_PRXPTR;
    uint8_t reserved[8];
} SceNpCommerceCurrencyInfo;

typedef struct SceNpCommercePrice {
    uint32_t integer;
    uint32_t fractional;
} SceNpCommercePrice;

int sceNpCommerceCreateCtx(uint32_t version, const SceNpId *npId, SceNpCommerceHandler handler, void *arg, SceNpCommerceCtxId *ctxId);
int sceNpCommerceDestroyCtx(SceNpCommerceCtxId ctxId);
int sceNpCommerceInitProductCategory(SceNpCommerceProductCategory *pc, const void *data, uint32_t dataSize);
void sceNpCommerceDestroyProductCategory(SceNpCommerceProductCategory *pc);
int sceNpCommerceGetProductCategoryStart(SceNpCommerceCtxId ctxId, const char *categoryId, int32_t langCode, SceNpCommerceRequestId *reqId);
int sceNpCommerceGetProductCategoryFinish(SceNpCommerceRequestId reqId);
int sceNpCommerceGetProductCategoryResult(SceNpCommerceRequestId reqId, void *buf, uint32_t bufSize, uint32_t *fillSize);
int sceNpCommerceGetProductCategoryAbort(SceNpCommerceRequestId reqId);
const char *sceNpCommerceGetProductId(SceNpCommerceProductSkuInfo *info);
const char *sceNpCommerceGetProductName(SceNpCommerceProductSkuInfo *info);
const char *sceNpCommerceGetCategoryDescription(SceNpCommerceCategoryInfo *info);
const char *sceNpCommerceGetCategoryId(SceNpCommerceCategoryInfo *info);
const char *sceNpCommerceGetCategoryImageURL(SceNpCommerceCategoryInfo *info);
int sceNpCommerceGetCategoryInfo(SceNpCommerceProductCategory *pc, SceNpCommerceCategoryInfo *info);
const char *sceNpCommerceGetCategoryName(SceNpCommerceCategoryInfo *info);
const char *sceNpCommerceGetCurrencyCode(SceNpCommerceCurrencyInfo *info);
uint32_t sceNpCommerceGetCurrencyDecimals(SceNpCommerceCurrencyInfo *info);
int sceNpCommerceGetCurrencyInfo(SceNpCommerceProductCategory *pc, SceNpCommerceCurrencyInfo *info);
int sceNpCommerceGetNumOfChildCategory(SceNpCommerceProductCategory *pc, uint32_t *num);
int sceNpCommerceGetNumOfChildProductSku(SceNpCommerceProductCategory *pc, uint32_t *num);
const char *sceNpCommerceGetSkuDescription(SceNpCommerceProductSkuInfo *info);
const char *sceNpCommerceGetSkuId(SceNpCommerceProductSkuInfo *info);
const char *sceNpCommerceGetSkuImageURL(SceNpCommerceProductSkuInfo *info);
const char *sceNpCommerceGetSkuName(SceNpCommerceProductSkuInfo *info);
void sceNpCommerceGetSkuPrice(SceNpCommerceProductSkuInfo *info, SceNpCommercePrice *price);
const char *sceNpCommerceGetSkuUserData(SceNpCommerceProductSkuInfo *info);
int sceNpCommerceSetDataFlagStart(SceNpCommerceCtxId ctxId, uint32_t dataFlag, uint32_t subjectId, uint32_t value, SceNpCommerceRequestId *reqId);
int sceNpCommerceGetDataFlagStart(SceNpCommerceCtxId ctxId, uint32_t dataFlag, uint32_t subjectId, uint32_t reserved, SceNpCommerceRequestId *reqId);
int sceNpCommerceSetDataFlagFinish(void);
int sceNpCommerceGetDataFlagFinish(void);
int sceNpCommerceGetDataFlagState(uint32_t subjectId, uint32_t dataFlag, uint32_t *state);
int sceNpCommerceGetDataFlagAbort(void);
int sceNpCommerceGetChildCategoryInfo(SceNpCommerceProductCategory *pc, uint32_t childIndex, SceNpCommerceCategoryInfo *info);
int sceNpCommerceGetChildProductSkuInfo(SceNpCommerceProductCategory *pc, uint32_t childIndex, SceNpCommerceProductSkuInfo *info);
int sceNpCommerceDoCheckoutStartAsync(SceNpCommerceCtxId ctxId, const char **skuIds, uint32_t skuNum, sys_memory_container_t container, SceNpCommerceRequestId *reqId);
int sceNpCommerceDoCheckoutFinishAsync(SceNpCommerceRequestId reqId);

static inline int sceNpCommerceInit(void) { return 0; }
static inline int sceNpCommerceTerm(void) { return 0; }

#ifdef __cplusplus
}
#endif

#endif /* __PS3DK_CELL_NP_COMMERCE_H__ */
