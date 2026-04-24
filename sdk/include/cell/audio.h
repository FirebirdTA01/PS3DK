/*! \file cell/audio.h
 \brief cellAudio API - PS3 output-audio mixer, 23 exports.

  The libaudio module owns the audio output mixer that downstream
  applications feed PCM into.  The workflow is:

    cellAudioInit()                     // once, at startup
    cellAudioPortOpen(&param, &port)    // reserve a mixer port
    cellAudioGetPortConfig(port, &cfg)  // read back buffer layout
    cellAudioPortStart(port)            // engage
    // fill cfg.portAddr with PCM, stride = nChannel * BLOCK_SAMPLES * sizeof(float)
    // advance by one block per ~5.3 ms; event queue fires per mix
    cellAudioPortStop(port)             // stop feeding mixer
    cellAudioPortClose(port)            // release port
    cellAudioQuit()                     // at shutdown

  Declarations are linked against libaudio_stub.a (built from
  tools/nidgen/nids/extracted/libaudio_stub.yaml by
  scripts/build-cell-stub-archives.sh).  Each export's FNID lands in
  .rodata.sceFNID; the module name "cellAudio" appears in
  .rodata.sceResident so the PRX loader pulls in the cellAudio SPRX
  at runtime.

  Callers generally do NOT need cellAudioSendAck -- it is only
  relevant for IPC-backed setups that acknowledge mixer heartbeats.
*/

#ifndef __PS3DK_CELL_AUDIO_H__
#define __PS3DK_CELL_AUDIO_H__

#include <stdint.h>
#include <ppu-types.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ *
 * Constants
 * ------------------------------------------------------------------ */

#define CELL_AUDIO_MAX_PORT                 4
#define CELL_AUDIO_MAX_PORT_2               8
#define CELL_AUDIO_BLOCK_SAMPLES            256

#define CELL_AUDIO_CREATEEVENTFLAG_SPU      0x00000001u

#define CELL_AUDIO_EVENTFLAG_BEFOREMIX      0x80000000u
#define CELL_AUDIO_EVENTFLAG_NOMIX          0x40000000u
#define CELL_AUDIO_EVENTFLAG_HEADPHONE      0x20000000u
#define CELL_AUDIO_EVENTFLAG_DECIMATE_4     0x10000000u
#define CELL_AUDIO_EVENTFLAG_DECIMATE_2     0x08000000u

#define CELL_AUDIO_EVENT_MIX                0
#define CELL_AUDIO_EVENT_HEADPHONE          1

#define CELL_AUDIO_STATUS_CLOSE             0x1010
#define CELL_AUDIO_STATUS_READY             1
#define CELL_AUDIO_STATUS_RUN               2

#define CELL_AUDIO_PORT_2CH                 2
#define CELL_AUDIO_PORT_8CH                 8

#define CELL_AUDIO_BLOCK_8                  8
#define CELL_AUDIO_BLOCK_16                 16
#define CELL_AUDIO_BLOCK_32                 32

#define CELL_AUDIO_PORTATTR_BGM             0x0000000000000010ULL
#define CELL_AUDIO_PORTATTR_INITLEVEL       0x0000000000001000ULL
#define CELL_AUDIO_PORTATTR_OUT_STREAM1     0x0000000000000001ULL
#define CELL_AUDIO_PORTATTR_OUT_SECONDARY   0x0000000000000001ULL
#define CELL_AUDIO_PORTATTR_OUT_NO_ROUTE    0x0000000000100000ULL
#define CELL_AUDIO_PORTATTR_OUT_PERSONAL_0  0x0000000001000000ULL
#define CELL_AUDIO_PORTATTR_OUT_PERSONAL_1  0x0000000002000000ULL
#define CELL_AUDIO_PORTATTR_OUT_PERSONAL_2  0x0000000004000000ULL
#define CELL_AUDIO_PORTATTR_OUT_PERSONAL_3  0x0000000008000000ULL

#define CELL_AUDIO_PERSONAL_DEVICE_PRIMARY  0x8000

#define CELL_AUDIO_MISC_ACCVOL_ALLDEVICE    0x0000ffffU

/* Error codes - facility 0x031, status 0x701..0x70f. */
#define CELL_AUDIO_ERROR_ALREADY_INIT           0x80310701
#define CELL_AUDIO_ERROR_AUDIOSYSTEM            0x80310702
#define CELL_AUDIO_ERROR_NOT_INIT               0x80310703
#define CELL_AUDIO_ERROR_PARAM                  0x80310704
#define CELL_AUDIO_ERROR_PORT_FULL              0x80310705
#define CELL_AUDIO_ERROR_PORT_ALREADY_RUN       0x80310706
#define CELL_AUDIO_ERROR_PORT_NOT_OPEN          0x80310707
#define CELL_AUDIO_ERROR_PORT_NOT_RUN           0x80310708
#define CELL_AUDIO_ERROR_TRANS_EVENT            0x80310709
#define CELL_AUDIO_ERROR_PORT_OPEN              0x8031070au
#define CELL_AUDIO_ERROR_SHAREDMEMORY           0x8031070bu
#define CELL_AUDIO_ERROR_MUTEX                  0x8031070cu
#define CELL_AUDIO_ERROR_EVENT_QUEUE            0x8031070du
#define CELL_AUDIO_ERROR_AUDIOSYSTEM_NOT_FOUND  0x8031070eu
#define CELL_AUDIO_ERROR_TAG_NOT_FOUND          0x8031070fu

/* ------------------------------------------------------------------ *
 * Structs
 * ------------------------------------------------------------------ */

typedef struct {
    uint64_t nChannel;
    uint64_t nBlock;
    uint64_t attr;
    float    level;
} CellAudioPortParam;

typedef struct {
    sys_addr_t readIndexAddr;
    uint32_t   status;
    uint64_t   nChannel;
    uint64_t   nBlock;
    uint32_t   portSize;
    sys_addr_t portAddr;
} CellAudioPortConfig;

/* ------------------------------------------------------------------ *
 * Library lifetime
 * ------------------------------------------------------------------ */

int cellAudioInit(void);
int cellAudioQuit(void);

/* ------------------------------------------------------------------ *
 * Port management
 * ------------------------------------------------------------------ */

int cellAudioPortOpen(CellAudioPortParam *audioParam, uint32_t *portNum);
int cellAudioPortClose(uint32_t portNum);
int cellAudioPortStart(uint32_t portNum);
int cellAudioPortStop(uint32_t portNum);
int cellAudioGetPortConfig(uint32_t portNum, CellAudioPortConfig *portConfig);
int cellAudioGetPortBlockTag(uint32_t portNum, uint64_t index, uint64_t *frameTag);
int cellAudioGetPortTimestamp(uint32_t portNum, uint64_t frameTag, uint64_t *timeStamp);
int cellAudioSetPortLevel(uint32_t portNum, float linearVol);

/* ------------------------------------------------------------------ *
 * Mixer-heartbeat notifications.  Typical pattern: call
 * cellAudioCreateNotifyEventQueue to get a queue + key, then
 * cellAudioSetNotifyEventQueue to register it for mix events.
 * ------------------------------------------------------------------ */

int cellAudioCreateNotifyEventQueue(sys_event_queue_t *id, sys_ipc_key_t *key);
int cellAudioCreateNotifyEventQueueEx(sys_event_queue_t *queue,
                                      sys_ipc_key_t *queueKey,
                                      uint32_t flags);

int cellAudioSetNotifyEventQueue(sys_ipc_key_t key);
int cellAudioSetNotifyEventQueueEx(sys_ipc_key_t queueKey, uint32_t flags);
int cellAudioRemoveNotifyEventQueue(sys_ipc_key_t key);
int cellAudioRemoveNotifyEventQueueEx(sys_ipc_key_t queueKey, uint32_t flags);

int cellAudioSendAck(uint64_t data3);

/* ------------------------------------------------------------------ *
 * Data path.  Feed PCM into the mixer port's ring buffer.  src is a
 * float32 array; cellAudioAddData handles arbitrary channel counts,
 * cellAudioAdd2chData / cellAudioAdd6chData are fast paths for
 * stereo / 5.1 when the source shape matches the port shape.
 * ------------------------------------------------------------------ */

int cellAudioAddData(uint32_t portNum, float *src, unsigned int samples, float volume);
int cellAudioAdd2chData(uint32_t portNum, float *src, unsigned int samples, float volume);
int cellAudioAdd6chData(uint32_t portNum, float *src, float volume);

/* ------------------------------------------------------------------ *
 * Misc - system-wide accessory volume, personal-stream routing.
 * ------------------------------------------------------------------ */

int cellAudioMiscSetAccessoryVolume(uint32_t device, float level);
int cellAudioSetPersonalDevice(int personalStream, int device);
int cellAudioUnsetPersonalDevice(int personalStream);

#ifdef __cplusplus
}
#endif

#endif /* __PS3DK_CELL_AUDIO_H__ */
