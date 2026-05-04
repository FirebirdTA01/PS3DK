/* cell/sail/mp4.h — cellSail MP4 stream support. Clean-room. */
#ifndef __PS3DK_CELL_SAIL_MP4_H__
#define __PS3DK_CELL_SAIL_MP4_H__
#include <stdint.h>
#include <stdbool.h>
#include <cell/sail/common.h>
#ifdef __cplusplus
extern "C" {
#endif
enum { CELL_SAIL_MP4_MEDIA_SOUND=CELL_SAIL_4CC('s','o','u','n'), CELL_SAIL_MP4_MEDIA_AUDIO=CELL_SAIL_4CC('s','o','u','n'), CELL_SAIL_MP4_MEDIA_VIDEO=CELL_SAIL_4CC('v','i','d','e'), CELL_SAIL_MP4_MEDIA_OD=CELL_SAIL_4CC('o','d','s','m') };
enum { CELL_SAIL_MP4_AUDIO_SAMPLE_CODING_UNSPECIFIED=0, CELL_SAIL_MP4_AUDIO_SAMPLE_CODING_MP4A=CELL_SAIL_4CC('m','p','4','a') };
enum { CELL_SAIL_MP4_VIDEO_SAMPLE_CODING_UNSPECIFIED=0, CELL_SAIL_MP4_VIDEO_SAMPLE_CODING_MP4V=CELL_SAIL_4CC('m','p','4','v'), CELL_SAIL_MP4_VIDEO_SAMPLE_CODING_AVC1=CELL_SAIL_4CC('a','v','c','1') };
enum { CELL_SAIL_MP4_TRACK_REFERENCE_SYNC=CELL_SAIL_4CC('s','y','n','c') };
typedef struct { uint64_t internalData[16]; } CellSailMp4Movie;
typedef struct { uint64_t internalData[6]; } CellSailMp4Track;
typedef struct { uint16_t s,m,h,d,mo,y,res0,res1; } CellSailMp4DateTime;
typedef struct { CellSailMp4DateTime creationDateTime, modificationDateTime; uint32_t trackCount, movieTimeScale; uint64_t movieDuration; uint32_t reserved[16]; } CellSailMp4MovieInfo;
typedef struct { bool isTrackEnable; uint8_t res0[3]; uint32_t trackId; uint64_t trackDuration; int16_t layer, alternateGroup; uint16_t res1[2]; uint32_t trackWidth, trackHeight; uint16_t language, res2; uint32_t mediaType; uint32_t res3[6]; } CellSailMp4TrackInfo;
int cellSailMp4MovieGetBrand(CellSailMp4Movie*,uint32_t*,uint32_t*);
int cellSailMp4MovieIsCompatibleBrand(CellSailMp4Movie*,uint32_t);
int cellSailMp4MovieGetMovieInfo(CellSailMp4Movie*,CellSailMp4MovieInfo*);
int cellSailMp4MovieGetTrackByIndex(CellSailMp4Movie*,int,CellSailMp4Track*);
int cellSailMp4MovieGetTrackById(CellSailMp4Movie*,uint32_t,CellSailMp4Track*);
int cellSailMp4MovieGetTrackByTypeAndIndex(CellSailMp4Movie*,uint32_t,uint32_t,int,CellSailMp4Track*);
int cellSailMp4TrackGetTrackInfo(CellSailMp4Track*,CellSailMp4TrackInfo*);
int cellSailMp4TrackGetTrackReferenceCount(CellSailMp4Track*,uint32_t);
int cellSailMp4TrackGetTrackReference(CellSailMp4Track*,uint32_t,int,CellSailMp4Track*);
static inline int cellSailMp4ConvertTimeScale(uint64_t fd,uint32_t fts,uint32_t tts,uint64_t*ptd){if(0==fts)return-1;*ptd=(fd*tts)/fts;return 0;}
#ifdef __cplusplus
}
#endif
#endif
