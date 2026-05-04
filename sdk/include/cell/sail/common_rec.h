/* cell/sail/common_rec.h — libsail_rec shared types. Clean-room. */
#ifndef __PS3DK_CELL_SAIL_COMMON_REC_H__
#define __PS3DK_CELL_SAIL_COMMON_REC_H__
#include <stdint.h>
#include <stddef.h>
#ifdef __PPU__
#include <ppu-types.h>
#else
#ifndef ATTRIBUTE_PRXPTR
#define ATTRIBUTE_PRXPTR
#endif
#endif
#ifdef __cplusplus
extern "C" {
#endif
enum { CELL_SAIL_EVENT_RECORDER_CALL_COMPLETED=2, CELL_SAIL_EVENT_RECORDER_STATE_CHANGED=3 };
enum { CELL_SAIL_VIDEO_FRAME_RATE_100HZ=8, CELL_SAIL_VIDEO_FRAME_RATE_120000_1001HZ=9, CELL_SAIL_VIDEO_FRAME_RATE_120HZ=10 };
typedef struct { uint32_t key; uintptr_t value; } CellSailCommand;
#define CELL_SAIL_MAX_NUM_FILTER_COMMANDS 16
typedef struct {
    void *pFrame ATTRIBUTE_PRXPTR; uint32_t size, sampleNum, reserved0;
    uint64_t timeStamp, reserved1; uint32_t commandNum;
    CellSailCommand *pCommands ATTRIBUTE_PRXPTR; uint32_t reserved2, reserved3;
} CellSailFeederFrameInfo;
typedef CellSailFeederFrameInfo CellSailCaptureFrameInfo;
#ifdef __cplusplus
}
#endif
#endif
