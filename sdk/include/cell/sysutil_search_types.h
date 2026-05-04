/* cell/sysutil_search_types.h — cellSearch content-search types.
 *
 * Clean-room header.  CellSearchContentId is a 16-byte opaque
 * identifier used across the cellSearch API.  This header also
 * provides the enums, callback typedefs, and result structs the
 * cell SDK ABI requires.
 *
 * Width-sensitive integer fields (time_t, size_t) are declared as
 * uint32_t in caller-allocated structs crossing the SPRX boundary —
 * the SPRX uses a 32-bit-pointer ABI where these are 4 bytes wide.
 */
#ifndef __PS3DK_CELL_SYSUTIL_SEARCH_TYPES_H__
#define __PS3DK_CELL_SYSUTIL_SEARCH_TYPES_H__

#include <stdint.h>

#include <cell/sysutil_music.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- opaque content ID (16 bytes) -------------------------------
 *
 * CellSearchContentId is forward-declared in <cell/sysutil_music.h>.
 * The struct body lives here so both headers can use the complete
 * type without a circular dependency. */

#define CELL_SEARCH_CONTENT_ID_SIZE  16

struct CellSearchContentId {
    char data[CELL_SEARCH_CONTENT_ID_SIZE];
};

/* ---- search handle ---------------------------------------------- */

typedef int32_t CellSearchId;
#define CELL_SEARCH_ID_INVALID  ((CellSearchId)0)

/* ---- string-length limits --------------------------------------- */

#define CELL_SEARCH_GAMECOMMENT_SIZE_MAX    1024
#define CELL_SEARCH_CONTENT_BUFFER_SIZE_MAX 2048
#define CELL_SEARCH_TITLE_LEN_MAX           384
#define CELL_SEARCH_PATH_LEN_MAX            63
#define CELL_SEARCH_MTOPTION_LEN_MAX        63
#define CELL_SEARCH_TAG_LEN_MAX             63
#define CELL_SEARCH_TAG_NUM_MAX             6
#define CELL_SEARCH_DEVELOPERDATA_LEN_MAX   64

/* ---- enums ------------------------------------------------------ */

typedef enum {
    CELL_SEARCH_MODE_NORMAL = 0,
} CellSearchMode;

typedef enum {
    CELL_SEARCH_EVENT_NOTIFICATION = 0,
    CELL_SEARCH_EVENT_INITIALIZE_RESULT,
    CELL_SEARCH_EVENT_FINALIZE_RESULT,
    CELL_SEARCH_EVENT_LISTSEARCH_RESULT,
    CELL_SEARCH_EVENT_CONTENTSEARCH_INLIST_RESULT,
    CELL_SEARCH_EVENT_CONTENTSEARCH_RESULT,
    CELL_SEARCH_EVENT_SCENESEARCH_INVIDEO_RESULT,
    CELL_SEARCH_EVENT_SCENESEARCH_RESULT,
} CellSearchEvent;

typedef enum {
    CELL_SEARCH_LISTSEARCHTYPE_NONE = 0,
    CELL_SEARCH_LISTSEARCHTYPE_MUSIC_ALBUM,
    CELL_SEARCH_LISTSEARCHTYPE_MUSIC_GENRE,
    CELL_SEARCH_LISTSEARCHTYPE_MUSIC_ARTIST,
    CELL_SEARCH_LISTSEARCHTYPE_PHOTO_YEAR,
    CELL_SEARCH_LISTSEARCHTYPE_PHOTO_MONTH,
    CELL_SEARCH_LISTSEARCHTYPE_PHOTO_ALBUM,
    CELL_SEARCH_LISTSEARCHTYPE_PHOTO_PLAYLIST,
    CELL_SEARCH_LISTSEARCHTYPE_VIDEO_ALBUM,
    CELL_SEARCH_LISTSEARCHTYPE_MUSIC_PLAYLIST,
} CellSearchListSearchType;

typedef enum {
    CELL_SEARCH_CONTENTSEARCHTYPE_NONE = 0,
    CELL_SEARCH_CONTENTSEARCHTYPE_MUSIC_ALL,
    CELL_SEARCH_CONTENTSEARCHTYPE_PHOTO_ALL,
    CELL_SEARCH_CONTENTSEARCHTYPE_VIDEO_ALL,
} CellSearchContentSearchType;

typedef enum {
    CELL_SEARCH_SCENESEARCHTYPE_NONE = 0,
    CELL_SEARCH_SCENESEARCHTYPE_CHAPTER,
    CELL_SEARCH_SCENESEARCHTYPE_CLIP_HIGHLIGHT,
    CELL_SEARCH_SCENESEARCHTYPE_CLIP_USER,
    CELL_SEARCH_SCENESEARCHTYPE_CLIP,
    CELL_SEARCH_SCENESEARCHTYPE_ALL,
} CellSearchSceneSearchType;

typedef enum {
    CELL_SEARCH_SORTKEY_NONE = 0,
    CELL_SEARCH_SORTKEY_DEFAULT,
    CELL_SEARCH_SORTKEY_TITLE,
    CELL_SEARCH_SORTKEY_ALBUMTITLE,
    CELL_SEARCH_SORTKEY_GENRENAME,
    CELL_SEARCH_SORTKEY_ARTISTNAME,
    CELL_SEARCH_SORTKEY_IMPORTEDDATE,
    CELL_SEARCH_SORTKEY_TRACKNUMBER,
    CELL_SEARCH_SORTKEY_TAKENDATE,
    CELL_SEARCH_SORTKEY_USERDEFINED,
    CELL_SEARCH_SORTKEY_MODIFIEDDATE,
} CellSearchSortKey;

typedef enum {
    CELL_SEARCH_SORTORDER_NONE = 0,
    CELL_SEARCH_SORTORDER_ASCENDING,
    CELL_SEARCH_SORTORDER_DESCENDING,
} CellSearchSortOrder;

typedef enum {
    CELL_SEARCH_CONTENTTYPE_NONE = 0,
    CELL_SEARCH_CONTENTTYPE_MUSIC,
    CELL_SEARCH_CONTENTTYPE_MUSICLIST,
    CELL_SEARCH_CONTENTTYPE_PHOTO,
    CELL_SEARCH_CONTENTTYPE_PHOTOLIST,
    CELL_SEARCH_CONTENTTYPE_VIDEO,
    CELL_SEARCH_CONTENTTYPE_VIDEOLIST,
    CELL_SEARCH_CONTENTTYPE_SCENE,
} CellSearchContentType;

typedef enum {
    CELL_SEARCH_SCENETYPE_NONE = 0,
    CELL_SEARCH_SCENETYPE_CHAPTER,
    CELL_SEARCH_SCENETYPE_CLIP_HIGHLIGHT,
    CELL_SEARCH_SCENETYPE_CLIP_USER,
} CellSearchSceneType;

typedef enum {
    CELL_SEARCH_LISTTYPE_NONE = 0,
    CELL_SEARCH_LISTTYPE_MUSIC_ALBUM,
    CELL_SEARCH_LISTTYPE_MUSIC_GENRE,
    CELL_SEARCH_LISTTYPE_MUSIC_ARTIST,
    CELL_SEARCH_LISTTYPE_PHOTO_YEAR,
    CELL_SEARCH_LISTTYPE_PHOTO_MONTH,
    CELL_SEARCH_LISTTYPE_PHOTO_ALBUM,
    CELL_SEARCH_LISTTYPE_PHOTO_PLAYLIST,
    CELL_SEARCH_LISTTYPE_VIDEO_ALBUM,
    CELL_SEARCH_LISTTYPE_MUSIC_PLAYLIST,
} CellSearchListType;

typedef enum {
    CELL_SEARCH_CODEC_UNKNOWN = 0,
    CELL_SEARCH_CODEC_MPEG2,
    CELL_SEARCH_CODEC_MPEG4,
    CELL_SEARCH_CODEC_AVC,
    CELL_SEARCH_CODEC_MPEG1,
    CELL_SEARCH_CODEC_AT3,
    CELL_SEARCH_CODEC_AT3P,
    CELL_SEARCH_CODEC_ATALL,
    CELL_SEARCH_CODEC_MP3,
    CELL_SEARCH_CODEC_AAC,
    CELL_SEARCH_CODEC_LPCM,
    CELL_SEARCH_CODEC_WAV,
    CELL_SEARCH_CODEC_WMA,
    CELL_SEARCH_CODEC_JPEG,
    CELL_SEARCH_CODEC_PNG,
    CELL_SEARCH_CODEC_TIFF,
    CELL_SEARCH_CODEC_BMP,
    CELL_SEARCH_CODEC_GIF,
    CELL_SEARCH_CODEC_MPEG2_TS,
    CELL_SEARCH_CODEC_DSD,
    CELL_SEARCH_CODEC_AC3,
    CELL_SEARCH_CODEC_MPEG1_LAYER1,
    CELL_SEARCH_CODEC_MPEG1_LAYER2,
    CELL_SEARCH_CODEC_MPEG1_LAYER3,
    CELL_SEARCH_CODEC_MPEG2_LAYER1,
    CELL_SEARCH_CODEC_MPEG2_LAYER2,
    CELL_SEARCH_CODEC_MPEG2_LAYER3,
    CELL_SEARCH_CODEC_MOTIONJPEG,
    CELL_SEARCH_CODEC_MPO,
} CellSearchCodec;

typedef enum {
    CELL_SEARCH_ORIENTATION_UNKNOWN = 0,
    CELL_SEARCH_ORIENTATION_TOP_LEFT,
    CELL_SEARCH_ORIENTATION_TOP_RIGHT,
    CELL_SEARCH_ORIENTATION_BOTTOM_RIGHT,
    CELL_SEARCH_ORIENTATION_BOTTOM_LEFT,
} CellSearchOrientation;

typedef enum {
    CELL_SEARCH_REPEATMODE_NONE = 0,
    CELL_SEARCH_REPEATMODE_REPEAT1,
    CELL_SEARCH_REPEATMODE_ALL,
    CELL_SEARCH_REPEATMODE_NOREPEAT1,
} CellSearchRepeatMode;

typedef enum {
    CELL_SEARCH_CONTEXTOPTION_NONE = 0,
    CELL_SEARCH_CONTEXTOPTION_SHUFFLE,
} CellSearchContextOption;

typedef enum {
    CELL_SEARCH_CONTENTSTATUS_NONE,
    CELL_SEARCH_CONTENTSTATUS_AVILABLE,
    CELL_SEARCH_CONTENTSTATUS_NOT_SUPPORTED,
    CELL_SEARCH_CONTENTSTATUS_BROKEN,
} CellSearchContentStatus;

typedef enum {
    CELL_SEARCH_SHARABLETYPE_PROHIBITED = 0,
    CELL_SEARCH_SHARABLETYPE_PERMITTED,
} CellSearchSharableType;

/* ---- result structs --------------------------------------------- */

typedef struct {
    CellSearchId searchId;
    uint32_t     resultNum;
} CellSearchResultParam;

/*
 * NOTE: time_t fields (importedDate, lastPlayedDate, takenDate,
 * modifiedDate) are declared as uint32_t because the SPRX uses a
 * 32-bit-pointer ABI where time_t is 4 bytes.  Our LP64 PPU
 * toolchain would otherwise emit 8-byte time_t, misaligning every
 * subsequent field.
 */

typedef struct {
    uint32_t   importedDate;
    uint32_t   takenDate;
    uint32_t   modifiedDate;
} CellSearchTimeInfo;

typedef struct {
    CellSearchListType listType;
    uint32_t           numOfItems;
    int64_t            duration;
    char               title[CELL_SEARCH_TITLE_LEN_MAX + 1];
    char               reserved[3];
    char               artistName[CELL_SEARCH_TITLE_LEN_MAX + 1];
    char               reserved2[3];
} CellSearchMusicListInfo;

typedef struct {
    CellSearchListType listType;
    uint32_t           numOfItems;
    char               title[CELL_SEARCH_TITLE_LEN_MAX + 1];
    char               reserved[3];
} CellSearchPhotoListInfo;

typedef struct {
    CellSearchListType listType;
    uint32_t           numOfItems;
    int64_t            duration;
    char               title[CELL_SEARCH_TITLE_LEN_MAX + 1];
    char               reserved[3];
} CellSearchVideoListInfo;

typedef struct {
    int64_t                 duration;
    int64_t                 size;
    uint32_t                importedDate;
    uint32_t                lastPlayedDate;
    int32_t                 releasedYear;
    int32_t                 trackNumber;
    int32_t                 bitrate;
    int32_t                 samplingRate;
    int32_t                 quantizationBitrate;
    int32_t                 playCount;
    int32_t                 drmEncrypted;
    CellSearchCodec         codec;
    CellSearchContentStatus status;
    char                    diskNumber[8];
    char                    title[CELL_SEARCH_TITLE_LEN_MAX + 1];
    char                    reserved[3];
    char                    albumTitle[CELL_SEARCH_TITLE_LEN_MAX + 1];
    char                    reserved2[3];
    char                    artistName[CELL_SEARCH_TITLE_LEN_MAX + 1];
    char                    reserved3[3];
    char                    genreName[CELL_SEARCH_TITLE_LEN_MAX + 1];
    char                    reserved4[3];
} CellSearchMusicInfo;

typedef struct {
    int64_t                 size;
    uint32_t                importedDate;
    uint32_t                takenDate;
    int32_t                 width;
    int32_t                 height;
    CellSearchOrientation   orientation;
    CellSearchCodec         codec;
    CellSearchContentStatus status;
    char                    title[CELL_SEARCH_TITLE_LEN_MAX + 1];
    char                    reserved[3];
    char                    albumTitle[CELL_SEARCH_TITLE_LEN_MAX + 1];
    char                    reserved2[3];
} CellSearchPhotoInfo;

typedef struct {
    int64_t                 duration;
    int64_t                 size;
    uint32_t                importedDate;
    uint32_t                takenDate;
    int32_t                 videoBitrate;
    int32_t                 audioBitrate;
    int32_t                 drmEncrypted;
    CellSearchCodec         videoCodec;
    CellSearchCodec         audioCodec;
    CellSearchContentStatus status;
    char                    title[CELL_SEARCH_TITLE_LEN_MAX + 1];
    char                    reserved[3];
    char                    albumTitle[CELL_SEARCH_TITLE_LEN_MAX + 1];
    char                    reserved2[3];
} CellSearchVideoInfo;

typedef struct {
    CellSearchSceneType   sceneType;
    int64_t               startTime_ms;
    int64_t               endTime_ms;
    CellSearchContentId   videoId;
    char                  title[CELL_SEARCH_TITLE_LEN_MAX + 1];
    char                  reserved[3];
    char                  tags[CELL_SEARCH_TAG_NUM_MAX][CELL_SEARCH_TAG_LEN_MAX];
} CellSearchSceneInfo;

typedef struct {
    char contentPath[CELL_SEARCH_PATH_LEN_MAX + 1];
    char thumbnailPath[CELL_SEARCH_PATH_LEN_MAX + 1];
} CellSearchContentInfoPath;

typedef struct {
    char movieThumbnailPath[CELL_SEARCH_PATH_LEN_MAX + 1];
    char movieThumbnailOption[CELL_SEARCH_MTOPTION_LEN_MAX + 1];
} CellSearchContentInfoPathMovieThumb;

/* ---- callback typedef ------------------------------------------- */

typedef void (*CellSearchSystemCallback)(
    CellSearchEvent event, int result, const void *param, void *userData);

#ifdef __cplusplus
}
#endif

#endif /* __PS3DK_CELL_SYSUTIL_SEARCH_TYPES_H__ */
