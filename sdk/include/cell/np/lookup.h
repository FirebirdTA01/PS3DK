#ifndef __PS3DK_CELL_NP_LOOKUP_H__
#define __PS3DK_CELL_NP_LOOKUP_H__

#include <cell/np/common.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SCE_NP_LOOKUP_MAX_CTX_NUM 32

int sceNpLookupInit(void);
int sceNpLookupTerm(void);
int sceNpLookupCreateTitleCtx(const SceNpCommunicationId *titleId, const SceNpId *selfNpId);
int sceNpLookupDestroyTitleCtx(int32_t titleCtxId);
int sceNpLookupCreateTransactionCtx(int32_t titleCtxId);
int sceNpLookupDestroyTransactionCtx(int32_t transId);
int sceNpLookupAbortTransaction(int32_t transId);
int sceNpLookupSetTimeout(int32_t transId, usecond_t timeout);
int sceNpLookupWaitAsync(int32_t transId, int32_t *result);
int sceNpLookupPollAsync(int32_t transId, int32_t *result);
int sceNpLookupNpId(int32_t transId, const SceNpOnlineId *onlineId, SceNpId *npId, void *option);
int sceNpLookupNpIdAsync(int32_t transId, const SceNpOnlineId *onlineId, SceNpId *npId, int32_t prio, void *option);
int sceNpLookupUserProfile(int32_t transId, const SceNpId *npId, SceNpUserInfo *userInfo, SceNpAboutMe *aboutMe, SceNpMyLanguages *languages, SceNpCountryCode *countryCode, SceNpAvatarImage *avatarImage, void *option);
int sceNpLookupUserProfileAsync(int32_t transId, const SceNpId *npId, SceNpUserInfo *userInfo, SceNpAboutMe *aboutMe, SceNpMyLanguages *languages, SceNpCountryCode *countryCode, SceNpAvatarImage *avatarImage, int32_t prio, void *option);
int sceNpLookupUserProfileWithAvatarSize(int32_t transId, int32_t avatarSizeType, const SceNpId *npId, SceNpUserInfo *userInfo, SceNpAboutMe *aboutMe, SceNpMyLanguages *languages, SceNpCountryCode *countryCode, void *avatarImageData, size_t avatarImageDataMaxSize, size_t *avatarImageDataSize, void *option);
int sceNpLookupUserProfileWithAvatarSizeAsync(int32_t transId, int32_t avatarSizeType, const SceNpId *npId, SceNpUserInfo *userInfo, SceNpAboutMe *aboutMe, SceNpMyLanguages *languages, SceNpCountryCode *countryCode, void *avatarImageData, size_t avatarImageDataMaxSize, size_t *avatarImageDataSize, int32_t prio, void *option);
int sceNpLookupAvatarImage(int32_t transId, const SceNpAvatarUrl *avatarUrl, SceNpAvatarImage *avatarImage, void *option);
int sceNpLookupAvatarImageAsync(int32_t transId, const SceNpAvatarUrl *avatarUrl, SceNpAvatarImage *avatarImage, int32_t prio, void *option);
int sceNpLookupTitleStorage(void);
int sceNpLookupTitleStorageAsync(void);
int sceNpLookupTitleSmallStorage(int32_t transId, void *data, size_t maxSize, size_t *contentLength, void *option);
int sceNpLookupTitleSmallStorageAsync(int32_t transId, void *data, size_t maxSize, size_t *contentLength, int32_t prio, void *option);

#ifdef __cplusplus
}
#endif

#endif /* __PS3DK_CELL_NP_LOOKUP_H__ */
