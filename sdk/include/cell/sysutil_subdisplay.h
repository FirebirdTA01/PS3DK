/*! \file cell/sysutil_subdisplay.h
 \brief Sony-SDK-source-compat libsysutil_subdisplay (PSP-as-secondary-display) API.

  Backed by libsysutil_subdisplay_stub.a — the loader resolves the
  eleven exports against the cellSubDisplay SPRX module at runtime.
  No PSL1GHT runtime wrapper.
*/

#ifndef __PSL1GHT_CELL_SYSUTIL_SUBDISPLAY_H__
#define __PSL1GHT_CELL_SYSUTIL_SUBDISPLAY_H__

#include <stdint.h>
#include <stddef.h>
#include <sys/memory.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Error codes. */
#define CELL_SUBDISPLAY_ERROR_OUT_OF_MEMORY     0x80029851
#define CELL_SUBDISPLAY_ERROR_FATAL             0x80029852
#define CELL_SUBDISPLAY_ERROR_NOT_FOUND         0x80029853
#define CELL_SUBDISPLAY_ERROR_INVALID_VALUE     0x80029854
#define CELL_SUBDISPLAY_ERROR_NOT_INITIALIZED   0x80029855
#define CELL_SUBDISPLAY_ERROR_NOT_SUPPORTED     0x80029856
#define CELL_SUBDISPLAY_ERROR_SET_SAMPLE        0x80029860
#define CELL_SUBDISPLAY_ERROR_AUDIOOUT_IS_BUSY  0x80029861
#define CELL_SUBDISPLAY_ERROR_ZERO_REGISTERED   0x80029813

/* Status codes (delivered via CellSubDisplayHandler). */
#define CELL_SUBDISPLAY_STATUS_JOIN          0x00000001
#define CELL_SUBDISPLAY_STATUS_LEAVE         0x00000002
#define CELL_SUBDISPLAY_STATUS_FATALERROR    0x00000003

/* CellSubDisplayParam.version. */
#define CELL_SUBDISPLAY_VERSION_0001         0x00000001
#define CELL_SUBDISPLAY_VERSION_0002         0x00000002
#define CELL_SUBDISPLAY_VERSION_0003         0x00000003

/* CellSubDisplayParam.mode. */
#define CELL_SUBDISPLAY_MODE_REMOTEPLAY      0x00000001

/* CellSubDisplayVideoParam.format. */
#define CELL_SUBDISPLAY_VIDEO_FORMAT_A8R8G8B8  0x00000001
#define CELL_SUBDISPLAY_VIDEO_FORMAT_R8G8B8A8  0x00000002
#define CELL_SUBDISPLAY_VIDEO_FORMAT_YUV420    0x00000003

#define CELL_SUBDISPLAY_VIDEO_ASPECT_RATIO_16_9  0x0000
#define CELL_SUBDISPLAY_VIDEO_ASPECT_RATIO_4_3   0x0001

#define CELL_SUBDISPLAY_VIDEO_MODE_SETDATA   0x0000
#define CELL_SUBDISPLAY_VIDEO_MODE_CAPTURE   0x0001

#define CELL_SUBDISPLAY_AUDIO_MODE_SETDATA   0x0000
#define CELL_SUBDISPLAY_AUDIO_MODE_CAPTURE   0x0001

#define CELL_SUBDISPLAY_TOUCH_MAX_TOUCH_INFO 6

#define CELL_SUBDISPLAY_0001_MEMORY_CONTAINER_SIZE  (8  * 1024 * 1024)
#define CELL_SUBDISPLAY_0002_MEMORY_CONTAINER_SIZE  (10 * 1024 * 1024)
#define CELL_SUBDISPLAY_0003_MEMORY_CONTAINER_SIZE  (10 * 1024 * 1024)

#define CELL_SUBDISPLAY_0003_WIDTH  864
#define CELL_SUBDISPLAY_0003_PITCH  864
#define CELL_SUBDISPLAY_0003_HEIGHT 480

#define CELL_SUBDISPLAY_NICKNAME_LEN  256
#define CELL_SUBDISPLAY_PSPID_SIZE    16

#define CELL_SUBDISPLAY_TOUCH_STATUS_NONE     0
#define CELL_SUBDISPLAY_TOUCH_STATUS_PRESS    1
#define CELL_SUBDISPLAY_TOUCH_STATUS_RELEASE  2
#define CELL_SUBDISPLAY_TOUCH_STATUS_MOVE     3
#define CELL_SUBDISPLAY_TOUCH_STATUS_ABORT    4

/* sys_memory_container_t bridge (shared with other Phase 6.5 headers). */
#ifndef _SYS_MEMORY_CONTAINER_T_DEFINED
#define _SYS_MEMORY_CONTAINER_T_DEFINED
typedef sys_mem_container_t sys_memory_container_t;
#endif

typedef void (*CellSubDisplayHandler)(int cbMsg, uint64_t cbParam, void *userdata);

typedef struct CellSubDisplayVideoParam {
	int format;
	int width;
	int height;
	int pitch;
	int aspectRatio;
	int videoMode;
} CellSubDisplayVideoParam;

typedef struct CellSubDisplayAudioParam {
	int ch;
	int audioMode;
} CellSubDisplayAudioParam;

typedef struct CellSubDisplayParam {
	int version;
	int mode;
	int nGroup;
	int nPeer;
	CellSubDisplayVideoParam videoParam;
	CellSubDisplayAudioParam audioParam;
} CellSubDisplayParam;

typedef struct CellSubDisplayNickname {
	char data[CELL_SUBDISPLAY_NICKNAME_LEN];
} CellSubDisplayNickname;

typedef struct CellSubDisplayPSPId {
	char data[CELL_SUBDISPLAY_PSPID_SIZE];
} CellSubDisplayPSPId;

typedef struct CellSubDisplayPeerInfo {
	uint64_t                sessionId;
	uint32_t                portNo;
	CellSubDisplayPSPId     pspId;
	CellSubDisplayNickname  pspNickname;
} CellSubDisplayPeerInfo;

typedef struct CellSubDisplayTouchInfo {
	uint8_t  status;
	uint8_t  force;
	uint16_t x;
	uint16_t y;
} CellSubDisplayTouchInfo;

/* Resolved by libsysutil_subdisplay_stub.a. */
int cellSubDisplayGetRequiredMemory(CellSubDisplayParam *pParam);
int cellSubDisplayInit(CellSubDisplayParam *pParam, CellSubDisplayHandler func,
                       void *userdata, sys_memory_container_t container);
int cellSubDisplayEnd(void);
int cellSubDisplayStart(void);
int cellSubDisplayStop(void);
int cellSubDisplayGetVideoBuffer(int groupId, void **ppVideoBuf, size_t *pSize);
int cellSubDisplayAudioOutBlocking(int groupId, void *pvData, int samples);
int cellSubDisplayAudioOutNonBlocking(int groupId, void *pvData, int samples);
int cellSubDisplayGetPeerNum(int groupId);
int cellSubDisplayGetPeerList(int groupId, CellSubDisplayPeerInfo *pInfo, int *pNum);
int cellSubDisplayGetTouchInfo(int groupId, CellSubDisplayTouchInfo *pTouchInfo,
                               int *numTouchInfo);

#ifdef __cplusplus
}
#endif

#endif /* __PSL1GHT_CELL_SYSUTIL_SUBDISPLAY_H__ */
