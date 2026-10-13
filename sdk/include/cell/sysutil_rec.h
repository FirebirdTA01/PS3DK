/*! \file cell/sysutil_rec.h
 \brief In-game video recording (libsysutil_rec_stub).

  Describe the recording in a CellRecParam (video and audio formats plus
  CellRecOption entries), size the memory container with
  cellRecQueryMemSize, then cellRecOpen a file, cellRecStart / cellRecStop,
  and cellRecClose (keeping or discarding it).  Status changes arrive
  through the callback as CELL_REC_STATUS_*.  cellRecGetInfo reads the
  addresses of the video / audio input buffers and similar values;
  cellRecSetInfo sets movie times and metadata.

  The system module reads these structures with 32-bit pointers, so
  pointer members keep that width in LP64 programs (ATTRIBUTE_PRXPTR).
  CellRecSceneMetadata.tag points to an array of tag strings the module
  also reads as 32-bit addresses; its element type, ps3tc_rec_tag_t, is a
  32-bit pointer in both data models.
*/

#ifndef PS3TC_CELL_SYSUTIL_REC_H
#define PS3TC_CELL_SYSUTIL_REC_H

#include <stddef.h>
#include <stdint.h>
#include <ppu-types.h>       /* ATTRIBUTE_PRXPTR */
#include <sys/memory.h>      /* sys_memory_container_t */
#include <sdk_version.h>     /* CELL_SDK_VERSION */

#ifdef __cplusplus
extern "C" {
#endif

struct CellSpurs;

#define CELL_REC_MIN_MEMORY_CONTAINER_SIZE  0
#if CELL_SDK_VERSION < 0x300000
#define CELL_REC_MAX_MEMORY_CONTAINER_SIZE  (9 * 1024 * 1024)
#else
#define CELL_REC_MAX_MEMORY_CONTAINER_SIZE  (16 * 1024 * 1024)
#endif

#define CELL_REC_ERROR_OUT_OF_MEMORY   0x8002c501
#define CELL_REC_ERROR_FATAL           0x8002c502
#define CELL_REC_ERROR_INVALID_VALUE   0x8002c503
#define CELL_REC_ERROR_FILE_OPEN       0x8002c504
#define CELL_REC_ERROR_FILE_WRITE      0x8002c505
#define CELL_REC_ERROR_INVALID_STATE   0x8002c506
#define CELL_REC_ERROR_FILE_NO_DATA    0x8002c507

typedef struct {
	struct CellSpurs *pSpurs ATTRIBUTE_PRXPTR;
	int spu_usage_rate;
	uint8_t priority[8];
} CellRecSpursParam;

/* One option: `option` is a CELL_REC_OPTION_* and selects the union member. */
typedef struct {
	int option;
	union {
		int ppu_thread_priority;
		int spu_thread_priority;
		int capture_priority;
		int use_system_spu;
		int fit_to_youtube;
		int xmb_bgm;
		int mpeg4_fast_encode;
		uint32_t ring_sec;
		int video_input;
		int audio_input;
		int audio_input_mix_vol;
		int reduce_memsize;
		int show_xmb;
		char *metadata_filename ATTRIBUTE_PRXPTR;
		CellRecSpursParam *pSpursParam ATTRIBUTE_PRXPTR;
		uint64_t dummy;
	} value;
} CellRecOption;

typedef struct {
	int videoFmt;
	int audioFmt;
	int numOfOpt;
	CellRecOption *pOpt ATTRIBUTE_PRXPTR;
} CellRecParam;

#define CELL_REC_OPTION_PPU_THREAD_PRIORITY   1
#define CELL_REC_OPTION_SPU_THREAD_PRIORITY   2
#define CELL_REC_OPTION_CAPTURE_PRIORITY      3
#define CELL_REC_OPTION_USE_SYSTEM_SPU        4
#define CELL_REC_OPTION_FIT_TO_YOUTUBE        5
#define CELL_REC_OPTION_XMB_BGM               6
#define CELL_REC_OPTION_RING_SEC              7
#define CELL_REC_OPTION_MPEG4_FAST_ENCODE     8
#define CELL_REC_OPTION_VIDEO_INPUT           9
#define CELL_REC_OPTION_AUDIO_INPUT           10
#define CELL_REC_OPTION_AUDIO_INPUT_MIX_VOL   11
#define CELL_REC_OPTION_REDUCE_MEMSIZE        12
#define CELL_REC_OPTION_SHOW_XMB              13
#define CELL_REC_OPTION_METADATA_FILENAME     14
#define CELL_REC_OPTION_SPURS                 15

#define CELL_REC_PARAM_PPU_THREAD_PRIORITY_DEFAULT            400
#define CELL_REC_PARAM_SPU_THREAD_PRIORITY_DEFAULT            60
#define CELL_REC_PARAM_CAPTURE_PRIORITY_HIGHEST               0
#define CELL_REC_PARAM_CAPTURE_PRIORITY_EXCEPT_NOTIFICATION   1
#define CELL_REC_PARAM_CAPTURE_PRIORITY_GAME_SCREEN           2
#define CELL_REC_PARAM_USE_SYSTEM_SPU_DISABLE                 0
#define CELL_REC_PARAM_USE_SYSTEM_SPU_ENABLE                  1
#define CELL_REC_PARAM_USE_SYSTEM_SPU_ENALBE                  1   /* historical spelling, kept for source compatibility */
#define CELL_REC_PARAM_XMB_BGM_DISABLE                        0
#define CELL_REC_PARAM_XMB_BGM_ENABLE                         1
#define CELL_REC_PARAM_MPEG4_FAST_ENCODE_DISABLE              0
#define CELL_REC_PARAM_MPEG4_FAST_ENCODE_ENABLE               1
#define CELL_REC_PARAM_VIDEO_INPUT_DISABLE                    0
#define CELL_REC_PARAM_VIDEO_INPUT_ARGB_4_3                   1
#define CELL_REC_PARAM_VIDEO_INPUT_ARGB_16_9                  2
#define CELL_REC_PARAM_VIDEO_INPUT_RGBA_4_3                   3
#define CELL_REC_PARAM_VIDEO_INPUT_RGBA_16_9                  4
#define CELL_REC_PARAM_VIDEO_INPUT_YUV420PLANAR_16_9          5
#define CELL_REC_PARAM_AUDIO_INPUT_DISABLE                    0
#define CELL_REC_PARAM_AUDIO_INPUT_ENABLE                     1
#define CELL_REC_PARAM_AUDIO_INPUT_MIX_VOL_MIN                0
#define CELL_REC_PARAM_AUDIO_INPUT_MIX_VOL_MAX                100
#define CELL_REC_PARAM_REDUCE_MEMSIZE_DISABLE                 0
#define CELL_REC_PARAM_REDUCE_MEMSIZE_ENABLE                  1

/* Video formats: codec in the top nibble, size in the next, bit rate below. */
#define CELL_REC_PARAM_VIDEO_FMT_MPEG4_SMALL_512K_30FPS    0x0000
#define CELL_REC_PARAM_VIDEO_FMT_MPEG4_SMALL_768K_30FPS    0x0010
#define CELL_REC_PARAM_VIDEO_FMT_MPEG4_MIDDLE_512K_30FPS   0x0100
#define CELL_REC_PARAM_VIDEO_FMT_MPEG4_MIDDLE_768K_30FPS   0x0110
#define CELL_REC_PARAM_VIDEO_FMT_MPEG4_LARGE_512K_30FPS    0x0200
#define CELL_REC_PARAM_VIDEO_FMT_MPEG4_LARGE_768K_30FPS    0x0210
#define CELL_REC_PARAM_VIDEO_FMT_MPEG4_LARGE_1024K_30FPS   0x0220
#define CELL_REC_PARAM_VIDEO_FMT_MPEG4_LARGE_1536K_30FPS   0x0230
#define CELL_REC_PARAM_VIDEO_FMT_MPEG4_LARGE_2048K_30FPS   0x0240
#define CELL_REC_PARAM_VIDEO_FMT_AVC_MP_SMALL_512K_30FPS   0x1000
#define CELL_REC_PARAM_VIDEO_FMT_AVC_MP_SMALL_768K_30FPS   0x1010
#define CELL_REC_PARAM_VIDEO_FMT_AVC_MP_MIDDLE_512K_30FPS  0x1100
#define CELL_REC_PARAM_VIDEO_FMT_AVC_MP_MIDDLE_768K_30FPS  0x1110
#define CELL_REC_PARAM_VIDEO_FMT_AVC_MP_MIDDLE_1024K_30FPS 0x1120
#define CELL_REC_PARAM_VIDEO_FMT_AVC_MP_MIDDLE_1536K_30FPS 0x1130
#define CELL_REC_PARAM_VIDEO_FMT_AVC_BL_SMALL_512K_30FPS   0x2000
#define CELL_REC_PARAM_VIDEO_FMT_AVC_BL_SMALL_768K_30FPS   0x2010
#define CELL_REC_PARAM_VIDEO_FMT_AVC_BL_MIDDLE_512K_30FPS  0x2100
#define CELL_REC_PARAM_VIDEO_FMT_AVC_BL_MIDDLE_768K_30FPS  0x2110
#define CELL_REC_PARAM_VIDEO_FMT_AVC_BL_MIDDLE_1024K_30FPS 0x2120
#define CELL_REC_PARAM_VIDEO_FMT_AVC_BL_MIDDLE_1536K_30FPS 0x2130
#define CELL_REC_PARAM_VIDEO_FMT_MJPEG_SMALL_5000K_30FPS   0x3060
#define CELL_REC_PARAM_VIDEO_FMT_MJPEG_MIDDLE_5000K_30FPS  0x3160
#define CELL_REC_PARAM_VIDEO_FMT_MJPEG_LARGE_11000K_30FPS  0x3270
#define CELL_REC_PARAM_VIDEO_FMT_MJPEG_HD720_11000K_30FPS  0x3670
#define CELL_REC_PARAM_VIDEO_FMT_MJPEG_HD720_20000K_30FPS  0x3680
#define CELL_REC_PARAM_VIDEO_FMT_MJPEG_HD720_25000K_30FPS  0x3690
#define CELL_REC_PARAM_VIDEO_FMT_M4HD_SMALL_768K_30FPS     0x4010
#define CELL_REC_PARAM_VIDEO_FMT_M4HD_MIDDLE_768K_30FPS    0x4110
#define CELL_REC_PARAM_VIDEO_FMT_M4HD_LARGE_1536K_30FPS    0x4230
#define CELL_REC_PARAM_VIDEO_FMT_M4HD_LARGE_2048K_30FPS    0x4240
#define CELL_REC_PARAM_VIDEO_FMT_M4HD_HD720_2048K_30FPS    0x4640
#define CELL_REC_PARAM_VIDEO_FMT_M4HD_HD720_5000K_30FPS    0x4660
#define CELL_REC_PARAM_VIDEO_FMT_M4HD_HD720_11000K_30FPS   0x4670

#define CELL_REC_PARAM_AUDIO_FMT_AAC_64K     0x0002
#define CELL_REC_PARAM_AUDIO_FMT_AAC_96K     0x0000
#define CELL_REC_PARAM_AUDIO_FMT_AAC_128K    0x0001
#define CELL_REC_PARAM_AUDIO_FMT_ULAW_384K   0x1007
#define CELL_REC_PARAM_AUDIO_FMT_ULAW_768K   0x1008
#define CELL_REC_PARAM_AUDIO_FMT_PCM_384K    0x2007
#define CELL_REC_PARAM_AUDIO_FMT_PCM_768K    0x2008
#define CELL_REC_PARAM_AUDIO_FMT_PCM_1536K   0x2009

/* Presets for an upload to a video site. */
#define CELL_REC_PARAM_VIDEO_FMT_YOUTUBE        0x0310
#define CELL_REC_PARAM_VIDEO_FMT_YOUTUBE_LARGE  CELL_REC_PARAM_VIDEO_FMT_MPEG4_LARGE_2048K_30FPS
#define CELL_REC_PARAM_VIDEO_FMT_YOUTUBE_HD720  CELL_REC_PARAM_VIDEO_FMT_M4HD_HD720_5000K_30FPS
#define CELL_REC_PARAM_AUDIO_FMT_YOUTUBE        CELL_REC_PARAM_AUDIO_FMT_AAC_64K
#define CELL_REC_PARAM_VIDEO_FMT_YOUTUBE_MJPEG  CELL_REC_PARAM_VIDEO_FMT_MJPEG_HD720_11000K_30FPS
#define CELL_REC_PARAM_AUDIO_FMT_YOUTUBE_MJPEG  CELL_REC_PARAM_AUDIO_FMT_PCM_768K

#define CELL_REC_AUDIO_BLOCK_SAMPLES  256

typedef void (*CellRecCallback)(int recStatus, int recError, void *userdata);

#define CELL_REC_STATUS_UNLOAD  0
#define CELL_REC_STATUS_OPEN    1
#define CELL_REC_STATUS_START   2
#define CELL_REC_STATUS_STOP    3
#define CELL_REC_STATUS_CLOSE   4
#define CELL_REC_STATUS_ERR     10

#define CELL_REC_MAX_PATH_LEN   1023

size_t cellRecQueryMemSize(const CellRecParam *pParam);
int cellRecOpen(const char *pDirName, const char *pFileName, const CellRecParam *pParam,
                sys_memory_container_t container, CellRecCallback cb, void *cbUserData);
int cellRecStart(void);
int cellRecStop(void);
int cellRecClose(int isDiscard);

/* cellRecGetInfo selectors. */
#define CELL_REC_INFO_VIDEO_INPUT_ADDR          0
#define CELL_REC_INFO_VIDEO_INPUT_WIDTH         1
#define CELL_REC_INFO_VIDEO_INPUT_PITCH         2
#define CELL_REC_INFO_VIDEO_INPUT_HEIGHT        3
#define CELL_REC_INFO_AUDIO_INPUT_ADDR          4
#define CELL_REC_INFO_MOVIE_TIME_MSEC           5
#define CELL_REC_INFO_SPURS_SYSTEMWORKLOAD_ID   6

void cellRecGetInfo(int info, uint64_t *pValue);

/* cellRecSetInfo selectors; the META ones take the address of a
 * CellRecMovieMetadata / CellRecSceneMetadata as the value. */
#define CELL_REC_SETINFO_MOVIE_START_TIME_MSEC  100
#define CELL_REC_SETINFO_MOVIE_END_TIME_MSEC    101
#define CELL_REC_SETINFO_MOVIE_META             200
#define CELL_REC_SETINFO_SCEME_META             201

#define CELL_REC_MOVIE_META_GAME_TITLE_LEN      128
#define CELL_REC_MOVIE_META_MOVIE_TITLE_LEN     128
#define CELL_REC_MOVIE_META_DESCRIPTION_LEN     384
#define CELL_REC_MOVIE_META_USERDATA_LEN        64

#define CELL_REC_SCENE_META_TYPE_CHAPTER         0
#define CELL_REC_SCENE_META_TYPE_CLIP_HIGHLIGHT  1
#define CELL_REC_SCENE_META_TYPE_CLIP_USER       2
#define CELL_REC_SCENE_META_TITLE_LEN            128
#define CELL_REC_SCENE_META_TAG_NUM              6
#define CELL_REC_SCENE_META_TAG_LEN              64

typedef struct {
	char *gameTitle ATTRIBUTE_PRXPTR;
	char *movieTitle ATTRIBUTE_PRXPTR;
	char *description ATTRIBUTE_PRXPTR;
	char *userdata ATTRIBUTE_PRXPTR;
} CellRecMovieMetadata;

/* One scene tag: a 32-bit string pointer in both data models, because the
 * system module reads the tag array as four-byte elements.  Build the array
 * of this type (ps3tc_rec_tag_t tags[] = { "a", "b" };) and point tag at it. */
typedef char *ps3tc_rec_tag_t ATTRIBUTE_PRXPTR;

typedef struct {
	uint32_t type;
	uint64_t startTime;
	uint64_t endTime;
	char *title ATTRIBUTE_PRXPTR;
	uint32_t tagNum;
	ps3tc_rec_tag_t *tag ATTRIBUTE_PRXPTR;
} CellRecSceneMetadata;

int32_t cellRecSetInfo(int setInfo, uint64_t value);

#ifdef __cplusplus
}
#endif

#endif /* PS3TC_CELL_SYSUTIL_REC_H */
