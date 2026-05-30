#ifndef __PS3DK_CELL_NP_MANAGER_H__
#define __PS3DK_CELL_NP_MANAGER_H__

#include <cell/np/common.h>
#include <cell/rtc.h>
#include <stddef.h>
#include <stdint.h>
#include <sysutil/sysutil_common.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SCE_NP_MANAGER_SUB_SIGNIN_MAX 3
#define SCE_NP_MANAGER_SUB_SIGNIN_RESULT_OK 0
#define SCE_NP_MANAGER_SUB_SIGNIN_RESULT_CANCEL 1

#define SCE_NP_MANAGER_STATUS_OFFLINE -1
#define SCE_NP_MANAGER_STATUS_GETTING_TICKET 0
#define SCE_NP_MANAGER_STATUS_GETTING_PROFILE 1
#define SCE_NP_MANAGER_STATUS_LOGGING_IN 2
#define SCE_NP_MANAGER_STATUS_ONLINE 3

#define SCE_NP_MANAGER_EVENT_GOT_TICKET 255

typedef void (*SceNpManagerCallback)(int event, int result, void *arg);
typedef void (*SceNpManagerSubSigninCallback)(int result, SceNpId *npId, void *cb_arg);

static inline int sceNpManagerInit(void) { return 0; }
static inline int sceNpManagerTerm(void) { return 0; }

int sceNpManagerGetStatus(int *status);
int sceNpManagerGetOnlineId(SceNpOnlineId *onlineId);
int sceNpManagerGetNpId(SceNpId *npId);
int sceNpManagerRegisterCallback(SceNpManagerCallback callback, void *arg);
int sceNpManagerUnregisterCallback(void);
int sceNpManagerGetOnlineName(SceNpOnlineName *onlineName);
int sceNpManagerGetAvatarUrl(SceNpAvatarUrl *avatarUrl);
int sceNpManagerGetMyLanguages(SceNpMyLanguages *myLang);
int sceNpManagerGetNetworkTime(CellRtcTick *pTick);
int sceNpManagerGetAccountRegion(SceNpCountryCode *countryCode, int *languageCode);
int sceNpManagerGetAccountAge(int *age);
int sceNpManagerGetContentRatingFlag(int *isRestricted, int *age);
int sceNpManagerGetChatRestrictionFlag(int *isRestricted);
int sceNpManagerSubSignin(CellSysutilUserId userId, SceNpManagerSubSigninCallback cb_func, void *cb_arg, int flag);
int sceNpManagerSubSignout(SceNpId *npId);
int sceNpManagerSubSigninAbortGui(void);
int sceNpManagerGetCachedInfo(CellSysutilUserId userID, SceNpManagerCacheParam *param);
int sceNpManagerGetPsHandle(SceNpPsHandle *psHandle);
int sceNpManagerRequestTicket(const SceNpId *npId, const char *serviceId, const void *cookie, size_t cookieSize, const char *entitlementId, unsigned int consumedCount);
int sceNpManagerRequestTicket2(const SceNpId *npId, const SceNpTicketVersion *version, const char *serviceId, const void *cookie, size_t cookieSize, const char *entitlementId, unsigned int consumedCount);
int sceNpManagerGetTicket(void *buffer, size_t *bufferSize);
int sceNpManagerGetTicketParam(int paramId, SceNpTicketParam *param);
int sceNpManagerGetEntitlementIdList(SceNpEntitlementId *entIdList, size_t entIdListNum);
int sceNpManagerGetEntitlementById(const char *entId, SceNpEntitlement *ent);

#define sceNpManagerCheckEntitlementById(entId) sceNpManagerGetEntitlementById((entId), 0)

#ifdef __cplusplus
}
#endif

#endif /* __PS3DK_CELL_NP_MANAGER_H__ */
