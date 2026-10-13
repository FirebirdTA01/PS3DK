/*! \file cell/sysutil_remoteplay.h
 \brief Remote Play status and audio volume (libsysutil_remoteplay_stub).

  cellRemotePlayGetStatus reports where a Remote Play session is
  (CELL_REMOTEPLAY_STATUS_*); the comparative volume is the level of the
  Remote Play client's audio relative to the console's; cellRemotePlayBreak
  ends the session.  The *_MEMORY_CONTAINER_SIZE values are the container a
  title reserves for each Remote Play video mode.
*/

#ifndef PS3TC_CELL_SYSUTIL_REMOTEPLAY_H
#define PS3TC_CELL_SYSUTIL_REMOTEPLAY_H

#ifdef __cplusplus
extern "C" {
#endif

#define CELL_REMOTEPLAY_STATUS_LOADING      0x00000000
#define CELL_REMOTEPLAY_STATUS_WAIT         0x00000001
#define CELL_REMOTEPLAY_STATUS_RUNNING      0x00000002
#define CELL_REMOTEPLAY_STATUS_UNLOADING    0x00000003
#define CELL_REMOTEPLAY_STATUS_FATALERROR   0x00000004
#define CELL_REMOTEPLAY_STATUS_PREMOEND     0x00000005

#define CELL_REMOTEPLAY_M4V_MEMORY_CONTAINER_SIZE   (8 * 1024 * 1024)
#define CELL_REMOTEPLAY_AVC_MEMORY_CONTAINER_SIZE   (10 * 1024 * 1024)
#define CELL_REMOTEPLAY_480P_MEMORY_CONTAINER_SIZE  (10 * 1024 * 1024)

#define CELL_REMOTEPLAY_ERROR_INTERNAL      0x80029830

int cellRemotePlayGetStatus(void);
int cellRemotePlayGetComparativeVolume(float *pComparativeAudioVolume);
int cellRemotePlaySetComparativeVolume(float comparativeAudioVolume);
int cellRemotePlayBreak(void);

#ifdef __cplusplus
}
#endif

#endif /* PS3TC_CELL_SYSUTIL_REMOTEPLAY_H */
