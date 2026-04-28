/*! \file cell/sysmodule.h
 \brief Sony-SDK-source-compat cellSysmodule API.

  PSL1GHT ships a similar surface under <sysmodule/sysmodule.h> with
  PSL1GHT-style naming (sysModule*, SYSMODULE_*).  This header gives
  Sony-named entry points (cellSysmodule*, CELL_SYSMODULE_*) backed
  by libsysmodule_stub.a — the loader resolves the eight exports
  against the cellSysmodule SPRX module at runtime.

  FNIDs (verified against reference/sony-sdk/.../libsysmodule_stub.a):
    cellSysmoduleLoadModule     0x32267a31
    cellSysmoduleUnloadModule   0x112a5ee9
    cellSysmoduleIsLoaded       0x5a59e258
    cellSysmoduleInitialize     0x63ff6ff9
    cellSysmoduleFinalize       0x96c07adf
    cellSysmoduleSetMemcontainer 0xa193143c
    cellSysmoduleGetImagesize   0x1ef115ef
    cellSysmoduleFetchImage     0x3c92be09
*/

#ifndef __PSL1GHT_CELL_SYSMODULE_H__
#define __PSL1GHT_CELL_SYSMODULE_H__

#include <stdint.h>
#include <sys/memory.h>

#ifdef __cplusplus
extern "C" {
#endif

/* PRX module IDs (subset; full list lives in Sony's reference header). */
#define CELL_SYSMODULE_INVALID            0xffff
#define CELL_SYSMODULE_NET                0x0000
#define CELL_SYSMODULE_HTTP               0x0001
#define CELL_SYSMODULE_HTTP_UTIL          0x0002
#define CELL_SYSMODULE_SSL                0x0003
#define CELL_SYSMODULE_HTTPS              0x0004
#define CELL_SYSMODULE_VDEC               0x0005
#define CELL_SYSMODULE_ADEC               0x0006
#define CELL_SYSMODULE_DMUX               0x0007
#define CELL_SYSMODULE_VPOST              0x0008
#define CELL_SYSMODULE_RTC                0x0009
#define CELL_SYSMODULE_SPURS              0x000a
#define CELL_SYSMODULE_OVIS               0x000b
#define CELL_SYSMODULE_SHEAP              0x000c
#define CELL_SYSMODULE_SYNC               0x000d
#define CELL_SYSMODULE_SYNC2              0x0055
#define CELL_SYSMODULE_FS                 0x000e
#define CELL_SYSMODULE_JPGDEC             0x000f
#define CELL_SYSMODULE_GCM_SYS            0x0010
#define CELL_SYSMODULE_GCM                0x0010
#define CELL_SYSMODULE_AUDIO              0x0011
#define CELL_SYSMODULE_PAMF               0x0012
#define CELL_SYSMODULE_ATRAC3PLUS         0x0013
#define CELL_SYSMODULE_NETCTL             0x0014
#define CELL_SYSMODULE_SYSUTIL            0x0015
#define CELL_SYSMODULE_SYSUTIL_NP         0x0016
#define CELL_SYSMODULE_IO                 0x0017
#define CELL_SYSMODULE_PNGDEC             0x0018
#define CELL_SYSMODULE_FONT               0x0019
#define CELL_SYSMODULE_FONTFT             0x001a
#define CELL_SYSMODULE_FREETYPE           0x001b
#define CELL_SYSMODULE_USBD               0x001c
#define CELL_SYSMODULE_SAIL               0x001d
#define CELL_SYSMODULE_L10N               0x001e
#define CELL_SYSMODULE_RESC               0x001f
#define CELL_SYSMODULE_DAISY              0x0020
#define CELL_SYSMODULE_KEY2CHAR           0x0021
#define CELL_SYSMODULE_MIC                0x0022
#define CELL_SYSMODULE_CAMERA             0x0023
#define CELL_SYSMODULE_VDEC_MPEG2         0x0024
#define CELL_SYSMODULE_VDEC_AVC           0x0025
#define CELL_SYSMODULE_LV2DBG             0x002e
#define CELL_SYSMODULE_SYSUTIL_USERINFO   0x0032
#define CELL_SYSMODULE_SYSUTIL_SAVEDATA   0x0033
#define CELL_SYSMODULE_SUBDISPLAY         0x0034
#define CELL_SYSMODULE_SYSUTIL_REC        0x0035
#define CELL_SYSMODULE_VIDEO_EXPORT       0x0036
#define CELL_SYSMODULE_SYSUTIL_GAME_EXEC  0x0037
#define CELL_SYSMODULE_SYSUTIL_NP2        0x0038
#define CELL_SYSMODULE_SYSUTIL_AP         0x0039
#define CELL_SYSMODULE_SYSUTIL_NP_CLANS   0x003a
#define CELL_SYSMODULE_SYSUTIL_OSK_EXT    0x003b
#define CELL_SYSMODULE_VDEC_DIVX          0x003c
#define CELL_SYSMODULE_SPURS_JQ           0x0050

/* Return codes. */
#define CELL_SYSMODULE_LOADED                          0
#define CELL_SYSMODULE_ERROR_DUPLICATED                0x80012001
#define CELL_SYSMODULE_ERROR_UNKNOWN                   0x80012002
#define CELL_SYSMODULE_ERROR_UNLOADED                  0x80012003
#define CELL_SYSMODULE_ERROR_INVALID_MEMCONTAINER      0x80012004
#define CELL_SYSMODULE_ERROR_FATAL                     0x800120ff

/* Sony's `sys_memory_container_t` — in PSL1GHT this is
 * `sys_mem_container_t` from <sys/memory.h>. */
#ifndef _SYS_MEMORY_CONTAINER_T_DEFINED
#define _SYS_MEMORY_CONTAINER_T_DEFINED
typedef sys_mem_container_t sys_memory_container_t;
#endif

/* Resolved by libsysmodule_stub.a. */
int cellSysmoduleLoadModule(uint16_t id);
int cellSysmoduleUnloadModule(uint16_t id);
int cellSysmoduleIsLoaded(uint16_t id);

int cellSysmoduleInitialize(void);
int cellSysmoduleFinalize(void);
int cellSysmoduleSetMemcontainer(sys_memory_container_t ct);

#ifdef __cplusplus
}
#endif

#endif /* __PSL1GHT_CELL_SYSMODULE_H__ */
