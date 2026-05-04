/* cell/sail/profile.h — cellSail encoder profile configuration.
   Clean-room header. 4 entry points + inline helpers.
   size_t → uint32_t for caller-allocated struct fields. */
#ifndef __PS3DK_CELL_SAIL_PROFILE_H__
#define __PS3DK_CELL_SAIL_PROFILE_H__
#include <stdint.h>
#include <stddef.h>
#include <cell/sail/common.h>
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
/* encoder types */
enum { CELL_SAIL_ENCODER_AUDIO_UNSPECIFIED=-1, CELL_SAIL_ENCODER_VIDEO_UNSPECIFIED=-1, CELL_SAIL_ENCODER_AUDIO_M4AAC=0x01, CELL_SAIL_ENCODER_VIDEO_M4V=0x21, CELL_SAIL_ENCODER_VIDEO_AVC=0x22, CELL_SAIL_ENCODER_VIDEO_M4HD=0x24 };
/* M4V enc modes/presets */
enum { CELL_SAIL_M4V_ENC_MODE_UNSPECIFIED=-1, CELL_SAIL_M4V_ENC_MODE_NO_DELAY=0x101, CELL_SAIL_M4V_ENC_MODE_NO_DELAY_VBV_CONSTRAINT=0x102, CELL_SAIL_M4V_ENC_MODE_FAST=0x111, CELL_SAIL_M4V_ENC_MODE_FAST_VBV_CONSTRAINT=0x112, CELL_SAIL_M4V_ENC_MODE_LONG_DELAY=0x200 };
/* AVC enc modes/presets */
enum { CELL_SAIL_AVC_ENC_MODE_UNSPECIFIED=-1, CELL_SAIL_AVC_ENC_MODE_NO_DELAY=0x101, CELL_SAIL_AVC_ENC_MODE_NO_DELAY_HRD_CONSTRAINT=0x102, CELL_SAIL_AVC_ENC_MODE_NO_DELAY_CAVLC=0x111, CELL_SAIL_AVC_ENC_MODE_NO_DELAY_CAVLC_HRD_CONSTRAINT=0x112, CELL_SAIL_AVC_ENC_MODE_MIDDLE_DELAY=0x310, CELL_SAIL_AVC_ENC_MODE_LONG_DELAY=0x200 };
/* M4HD */
enum { CELL_SAIL_M4HD_ENC_MODE_UNSPECIFIED=-1 };

typedef struct { int index, value; } CellSailM4vEncExParam;
typedef struct { uint32_t thisSize; void *pReserved ATTRIBUTE_PRXPTR; CellSailM4vEncExParam *pExParams ATTRIBUTE_PRXPTR; uint32_t numExParams; } CellSailM4vEncConfiguration;
typedef struct { int index, value; } CellSailAvcEncExParam;
typedef struct { uint32_t thisSize; void *pReserved ATTRIBUTE_PRXPTR; CellSailAvcEncExParam *pExParams ATTRIBUTE_PRXPTR; unsigned numExParams; } CellSailAvcEncConfiguration;
typedef struct { uint32_t thisSize; int flags; uint32_t timeScale, majorBrand, compatibleBrandNum; uint32_t compatibleBrands[8]; uint32_t interleaveDuration; } CellSailMp4MovieParameter;
typedef struct { uint32_t thisSize, timeScale, interleaveDuration; } CellSailGeneralMovieParameter;
typedef struct { uint32_t thisSize; int heapSize; } CellSailStreamParameter;
typedef struct { uint32_t thisSize; int codecType, flags, reserved0, reserved1, maxBitrate, aveBitrate, sampleScale; int reserved2[3]; CellSailAudioFormat format; } CellSailBsAudioParameter;
typedef struct { uint32_t thisSize; int codecType, flags, majorMode, minorMode, maxBitrate, aveBitrate, sampleScale, gopDuration; int reserved[2]; CellSailVideoFormat format; } CellSailBsVideoParameter;
typedef struct { uint64_t internalData[512]; } CellSailProfile;

int cellSailProfileSetStreamParameter(CellSailProfile*,CellSailStreamParameter*,void*);
int cellSailProfileSetEsAudioParameter(CellSailProfile*,unsigned,CellSailBsAudioParameter*,void*);
int cellSailProfileSetEsVideoParameter(CellSailProfile*,unsigned,CellSailBsVideoParameter*,void*);

static inline void cellSailM4vEncConfigurationSet(CellSailM4vEncConfiguration *c, CellSailM4vEncExParam *p, unsigned n) { c->thisSize=sizeof(*c); c->pReserved=NULL; c->pExParams=p; c->numExParams=n; }
static inline void cellSailAvcEncConfigurationSet(CellSailAvcEncConfiguration *c, CellSailAvcEncExParam *p, unsigned n) { c->thisSize=sizeof(*c); c->pReserved=NULL; c->pExParams=p; c->numExParams=n; }
#ifdef __cplusplus
}
#endif
#endif
