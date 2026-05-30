#ifndef __PS3DK_CELL_VOICE_H__
#define __PS3DK_CELL_VOICE_H__

#include <stdint.h>
#include <sys/event.h>
#include <cell/voiceTypes.h>

#ifdef __cplusplus
extern "C" {
#endif

int cellVoiceConnectIPortToOPort(uint32_t ips, uint32_t ops);
int cellVoiceCreateNotifyEventQueue(sys_event_queue_t *id, sys_ipc_key_t *key);
int cellVoiceCreatePort(uint32_t *portId, const CellVoicePortParam *pArg);
int cellVoiceDebugTopology(uint32_t *numPort, CellVoiceBasePortInfo *pInfo);
int cellVoiceDeletePort(uint32_t portId);
int cellVoiceDisconnectIPortFromOPort(uint32_t ips, uint32_t ops);
int cellVoiceEnd(void);
int cellVoiceGetBitRate(uint32_t portId, uint32_t *bitrate);
int cellVoiceGetMuteFlag(uint32_t portId, uint16_t *bMuted);
int cellVoiceGetPortAttr(uint32_t portId, CellVoicePortAttr attr, void *attrValue);
int cellVoiceGetPortInfo(uint32_t portId, CellVoiceBasePortInfo *pInfo);
int cellVoiceGetSignalState(uint32_t portId, CellVoicePortAttr attr, void *attrValue);
int cellVoiceGetVolume(uint32_t portId, float *volume);
int cellVoiceInit(CellVoiceInitParam *pArg);
int cellVoiceInitEx(CellVoiceInitParam *pArg);
int cellVoicePausePort(uint32_t portId);
int cellVoicePausePortAll(void);
int cellVoiceReadFromOPort(uint32_t ops, void *data, uint32_t *size);
int cellVoiceRemoveNotifyEventQueue(sys_ipc_key_t key);
int cellVoiceResetPort(uint32_t portId);
int cellVoiceResumePort(uint32_t portId);
int cellVoiceResumePortAll(void);
int cellVoiceSetBitRate(uint32_t portId, CellVoiceBitRate bitrate);
int cellVoiceSetMuteFlag(uint32_t portId, uint16_t bMuted);
int cellVoiceSetMuteFlagAll(uint16_t bMuted);
int cellVoiceSetNotifyEventQueue(sys_ipc_key_t key, uint64_t source);
int cellVoiceSetPortAttr(uint32_t portId, CellVoicePortAttr attr, void *attrValue);
int cellVoiceSetVolume(uint32_t portId, float volume);
int cellVoiceStart(void);
int cellVoiceStartEx(CellVoiceStartParam *pArg);
int cellVoiceStop(void);
int cellVoiceUpdatePort(uint32_t portId, const CellVoicePortParam *pArg);
int cellVoiceWriteToIPort(uint32_t ips, const void *data, uint32_t *size);
int cellVoiceWriteToIPortEx(uint32_t ips, const void *data, uint32_t *size,
                            uint32_t numFrameLost);
int cellVoiceWriteToIPortEx2(uint32_t ips, const void *data, uint32_t *size,
                             int16_t frameGaps);

#ifdef __cplusplus
}
#endif

#endif /* __PS3DK_CELL_VOICE_H__ */
