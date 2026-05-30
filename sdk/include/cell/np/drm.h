#ifndef __PS3DK_CELL_NP_DRM_H__
#define __PS3DK_CELL_NP_DRM_H__

#include <ppu-types.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SCE_NP_DRM_OPEN_FLAG 0x00000002
#define SCE_NP_DRM_EXITSPAWN2_EXIT_WO_FINI 0x4000000000000000ULL
#define SCE_NP_DRM_TIME_INFO_ENDLESS 0x7fffffffffffffffULL

typedef struct SceNpDrmKey {
    uint8_t keydata[16];
} SceNpDrmKey;

typedef struct SceNpDrmOpenArg {
    uint64_t flag;
} SceNpDrmOpenArg;

int sceNpDrmIsAvailable(const SceNpDrmKey *k_licensee, const char *path);
int sceNpDrmIsAvailable2(const SceNpDrmKey *k_licensee, const char *path);
int sceNpDrmVerifyUpgradeLicense(const char *content_id);
int sceNpDrmVerifyUpgradeLicense2(const char *content_id);
int sceNpDrmExecuteGamePurchase(void);
int sceNpDrmGetTimelimit(const char *path, uint64_t *time_remain);
int sceNpDrmProcessExitSpawn(const SceNpDrmKey *k_licensee, const char *path, char const *argv[], char const *envp[], sys_addr_t data, size_t data_size, int prio, uint64_t flags);
int sceNpDrmProcessExitSpawn2(const SceNpDrmKey *k_licensee, const char *path, char const *argv[], char const *envp[], sys_addr_t data, size_t data_size, int prio, uint64_t flags);

#ifdef __cplusplus
}
#endif

#endif /* __PS3DK_CELL_NP_DRM_H__ */
