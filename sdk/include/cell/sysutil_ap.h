/*! \file cell/sysutil_ap.h
 \brief Sony-SDK-source-compat libsysutil_ap (software access-point) API.

  PSL1GHT ships no wrappers for libsysutil_ap.  Phase 6.5 backs the
  three exports with a self-contained stub archive
  (libsysutil_ap_stub.a) emitted by `nidgen archive`; user code links
  with `-lsysutil_ap_stub` and the loader resolves against the
  cellSysutilAp SPRX module at runtime.

  FNIDs (verified against reference/sony-sdk/.../libsysutil_ap_stub.a
  and tools/nidgen/nids/extracted/libsysutil_ap_stub.yaml):
    cellSysutilApGetRequiredMemSize  0x9e67e0dd
    cellSysutilApOn                  0x3343824c
    cellSysutilApOff                 0x90c2bb19
*/

#ifndef __PSL1GHT_CELL_SYSUTIL_AP_H__
#define __PSL1GHT_CELL_SYSUTIL_AP_H__

#include <sys/memory.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Return + error codes. */
#define CELL_SYSUTIL_AP_ERROR_OUT_OF_MEMORY          0x8002cd00
#define CELL_SYSUTIL_AP_ERROR_FATAL                  0x8002cd01
#define CELL_SYSUTIL_AP_ERROR_INVALID_VALUE          0x8002cd02
#define CELL_SYSUTIL_AP_ERROR_NOT_INITIALIZED        0x8002cd03
#define CELL_SYSUTIL_AP_ERROR_ZERO_REGISTERED        0x8002cd13
#define CELL_SYSUTIL_AP_ERROR_NETIF_DISABLED         0x8002cd14
#define CELL_SYSUTIL_AP_ERROR_NETIF_NO_CABLE         0x8002cd15
#define CELL_SYSUTIL_AP_ERROR_NETIF_CANNOT_CONNECT   0x8002cd16

/* type values for cellSysutilApParam.type. */
#define CELL_SYSUTIL_AP_TYPE_USE_GAME_SETTING        0x00000001
#define CELL_SYSUTIL_AP_TYPE_USE_SYSTEM_SETTING      0x00000002
#define CELL_SYSUTIL_AP_TYPE_USE_REMOTEPLAY_SETTING  0x00000003

/* Sizes. */
#define CELL_SYSUTIL_AP_TITLE_ID_LEN                 9
#define CELL_SYSUTIL_AP_SSID_LEN                     32
#define CELL_SYSUTIL_AP_WPA_KEY_LEN                  64

/* wlanFlag values. */
#define CELL_SYSUTIL_AP_DISABLE                      0x00
#define CELL_SYSUTIL_AP_AVAILABLE                    0x01

/* Memory container size cellSysutilApOn requires (1 MiB). */
#define CELL_SYSUTIL_AP_MEMORY_CONTAINER_SIZE        (1 * 1024 * 1024)

/* Sony's `sys_memory_container_t` — in PSL1GHT this is
 * `sys_mem_container_t` from <sys/memory.h>. */
typedef sys_mem_container_t sys_memory_container_t;

typedef struct CellSysutilApTitleId {
	char data[CELL_SYSUTIL_AP_TITLE_ID_LEN];
	char padding[3];
} CellSysutilApTitleId;

typedef struct CellSysutilApSsid {
	char data[CELL_SYSUTIL_AP_SSID_LEN + 1];
	char padding[3];
} CellSysutilApSsid;

typedef struct CellSysutilApWpaKey {
	char data[CELL_SYSUTIL_AP_WPA_KEY_LEN + 1];
	char padding[3];
} CellSysutilApWpaKey;

typedef struct CellSysutilApParam {
	int                   type;
	int                   wlanFlag;
	CellSysutilApTitleId  titleId;
	CellSysutilApSsid     ssid;
	CellSysutilApWpaKey   wpakey;
} CellSysutilApParam;

/* Function declarations — resolved by libsysutil_ap_stub.a. */
int cellSysutilApGetRequiredMemSize(void);
int cellSysutilApOn(CellSysutilApParam *pParam, sys_memory_container_t container);
int cellSysutilApOff(void);

#ifdef __cplusplus
}
#endif

#endif /* __PSL1GHT_CELL_SYSUTIL_AP_H__ */
