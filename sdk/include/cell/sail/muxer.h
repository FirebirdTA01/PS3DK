/* cell/sail/muxer.h — cellSailComposer (user-defined muxer). Clean-room. */
#ifndef __PS3DK_CELL_SAIL_MUXER_H__
#define __PS3DK_CELL_SAIL_MUXER_H__
#include <stdint.h>
#include <stddef.h>
#include <cell/sail/common.h>
#include <cell/sail/common_rec.h>
#include <cell/sail/profile.h>
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
typedef struct { int codecType, flags, res0, res1, maxBitrate, aveBitrate, sampleScale; int res2[3]; CellSailAudioFormat format; void *pSpecificInfo ATTRIBUTE_PRXPTR; } CellSailComposerEsAudioParameter;
typedef struct { int codecType, flags, majorMode, minorMode, maxBitrate, aveBitrate, sampleScale, gopDuration; int res[2]; CellSailVideoFormat format; void *pSpecificInfo ATTRIBUTE_PRXPTR; } CellSailComposerEsVideoParameter;
typedef struct { uint8_t *pAu ATTRIBUTE_PRXPTR; uint32_t auSize; int64_t pts; uint64_t reserved; uint32_t sessionId; void *specificInfo ATTRIBUTE_PRXPTR; } CellSailComposerAudioAuInfo;
typedef struct { uint8_t *pAu ATTRIBUTE_PRXPTR; uint32_t auSize; int64_t pts, dts; uint64_t reserved; uint32_t sessionId; void *specificInfo ATTRIBUTE_PRXPTR; } CellSailComposerVideoAuInfo;
typedef struct { uint8_t *pAu ATTRIBUTE_PRXPTR; uint32_t auSize; int64_t pts, dts; uint64_t reserved; uint32_t sessionId; void *specificInfo ATTRIBUTE_PRXPTR; } CellSailComposerUserAuInfo;
typedef struct { int command; int value; } CellSailComposerCommand;
typedef struct { uint64_t internalData[32]; } CellSailComposer;
int cellSailComposerInitialize(CellSailComposer*,CellSailMp4MovieParameter*,CellSailStreamParameter*,unsigned);
int cellSailComposerFinalize(CellSailComposer*);
int cellSailComposerGetStreamParameter(CellSailComposer*,unsigned,CellSailStreamParameter*,void**);
int cellSailComposerSetEsAudioParameter(CellSailComposer*,unsigned,const CellSailComposerEsAudioParameter*);
int cellSailComposerSetEsVideoParameter(CellSailComposer*,unsigned,const CellSailComposerEsVideoParameter*);
int cellSailComposerSetEsUserParameter(CellSailComposer*,unsigned,CellSailStreamParameter*,void*);
int cellSailComposerGetEsAudioAu(CellSailComposer*,unsigned,CellSailComposerAudioAuInfo*,CellSailCommand*);
int cellSailComposerTryGetEsAudioAu(CellSailComposer*,unsigned,CellSailComposerAudioAuInfo*,CellSailCommand*);
int cellSailComposerGetEsVideoAu(CellSailComposer*,unsigned,CellSailComposerVideoAuInfo*,CellSailCommand*);
int cellSailComposerTryGetEsVideoAu(CellSailComposer*,unsigned,CellSailComposerVideoAuInfo*,CellSailCommand*);
int cellSailComposerGetEsUserAu(CellSailComposer*,unsigned,CellSailComposerUserAuInfo*,CellSailCommand*);
int cellSailComposerTryGetEsUserAu(CellSailComposer*,unsigned,CellSailComposerUserAuInfo*,CellSailCommand*);
int cellSailComposerGetEsAudioParameter(CellSailComposer*,unsigned,CellSailComposerEsAudioParameter*);
int cellSailComposerGetEsVideoParameter(CellSailComposer*,unsigned,CellSailComposerEsVideoParameter*);
int cellSailComposerGetEsUserParameter(CellSailComposer*,unsigned,CellSailStreamParameter*,void**);
int cellSailComposerRegisterEncoderCallbacks(CellSailComposer*,unsigned,void*);
int cellSailComposerUnregisterEncoderCallbacks(CellSailComposer*,unsigned);
int cellSailComposerNotifyCallCompleted(CellSailComposer*,int);
#ifdef __cplusplus
}
#endif
#endif
