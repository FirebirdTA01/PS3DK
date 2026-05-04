/* cell/sysutil_search.h — cellSearch content-search API.
 *
 * Clean-room header.  Declares the 21 cellSearch* entry points for
 * save-data / video / music / photo content search across the
 * dev_hdd0 filesystem.  20 entries in 3.70 firmware; 21 in 475+
 * (cellSearchGetContentInfoSharable is the +1).
 */
#ifndef __PS3DK_CELL_SYSUTIL_SEARCH_H__
#define __PS3DK_CELL_SYSUTIL_SEARCH_H__

#include <stdint.h>
#include <sys/types.h>

#include <cell/sysutil_search_types.h>
#include <cell/sysutil_music.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- return / status codes -------------------------------------- */

#define CELL_SEARCH_OK            0
#define CELL_SEARCH_CANCELED      1

#define CELL_SEARCH_ERROR_PARAM                        0x8002c801
#define CELL_SEARCH_ERROR_BUSY                         0x8002c802
#define CELL_SEARCH_ERROR_NO_MEMORY                    0x8002c803
#define CELL_SEARCH_ERROR_UNKNOWN_MODE                 0x8002c804
#define CELL_SEARCH_ERROR_ALREADY_INITIALIZED          0x8002c805
#define CELL_SEARCH_ERROR_NOT_INITIALIZED              0x8002c806
#define CELL_SEARCH_ERROR_FINALIZING                   0x8002c807
#define CELL_SEARCH_ERROR_NOT_SUPPORTED_SEARCH         0x8002c808
#define CELL_SEARCH_ERROR_CONTENT_OBSOLETE             0x8002c809
#define CELL_SEARCH_ERROR_CONTENT_NOT_FOUND            0x8002c80a
#define CELL_SEARCH_ERROR_NOT_LIST                     0x8002c80b
#define CELL_SEARCH_ERROR_OUT_OF_RANGE                 0x8002c80c
#define CELL_SEARCH_ERROR_INVALID_SEARCHID             0x8002c80d
#define CELL_SEARCH_ERROR_ALREADY_GOT_RESULT           0x8002c80e
#define CELL_SEARCH_ERROR_NOT_SUPPORTED_CONTEXT        0x8002c80f
#define CELL_SEARCH_ERROR_INVALID_CONTENTTYPE          0x8002c810
#define CELL_SEARCH_ERROR_DRM                          0x8002c811
#define CELL_SEARCH_ERROR_TAG                          0x8002c812
#define CELL_SEARCH_ERROR_GENERIC                      0x8002c8ff

/* ---- API entry points (21) -------------------------------------- */

int cellSearchInitialize(
    CellSearchMode           mode,
    sys_memory_container_t   container,
    CellSearchSystemCallback func,
    void                    *userData
);

int cellSearchFinalize(void);

int cellSearchStartListSearch(
    CellSearchListSearchType searchType,
    CellSearchSortOrder      sortOrder,
    CellSearchId            *outSearchId
);

int cellSearchStartContentSearchInList(
    const CellSearchContentId *listId,
    CellSearchSortKey          sortKey,
    CellSearchSortOrder        sortOrder,
    CellSearchId              *outSearchId
);

int cellSearchStartContentSearch(
    CellSearchContentSearchType type,
    CellSearchSortKey           sortKey,
    CellSearchSortOrder         sortOrder,
    CellSearchId               *outSearchId
);

int cellSearchGetContentInfoByOffset(
    const CellSearchId    searchId,
    int32_t               offset,
    void                 *infoBuffer,
    CellSearchContentType *outContentType,
    CellSearchContentId   *outContentId
);

int cellSearchGetContentInfoByContentId(
    const CellSearchContentId *contentId,
    void                      *infoBuffer,
    CellSearchContentType     *outContentType
);

int cellSearchGetOffsetByContentId(
    const CellSearchId          searchId,
    const CellSearchContentId  *contentId,
    int32_t                    *outOffset
);

int cellSearchGetContentInfoPath(
    const CellSearchContentId  *contentId,
    CellSearchContentInfoPath  *infoPath
);

int cellSearchGetContentInfoPathMovieThumb(
    const CellSearchContentId            *contentId,
    CellSearchContentInfoPathMovieThumb  *infoMt
);

int cellSearchGetContentInfoGameComment(
    const CellSearchContentId *contentId,
    char                       gameComment[CELL_SEARCH_GAMECOMMENT_SIZE_MAX]
);

int cellSearchGetContentIdByOffset(
    CellSearchId            searchId,
    int32_t                 offset,
    CellSearchContentType  *outContentType,
    CellSearchContentId    *outContentId,
    CellSearchTimeInfo     *outTimeInfo
);

int cellSearchPrepareFile(const char *path);

int cellSearchCancel(const CellSearchId searchId);

int cellSearchEnd(const CellSearchId searchId);

int cellSearchGetMusicSelectionContext(
    const CellSearchId          searchId,
    const CellSearchContentId  *contentId,
    CellSearchRepeatMode        repeatMode,
    CellSearchContextOption     option,
    CellMusicSelectionContext  *outContext
);

int cellSearchGetMusicSelectionContextOfSingleTrack(
    const CellSearchContentId  *contentId,
    CellMusicSelectionContext  *outContext
);

int cellSearchStartSceneSearchInVideo(
    const CellSearchContentId  *videoId,
    CellSearchSceneSearchType   searchType,
    CellSearchSortOrder         sortOrder,
    CellSearchId               *outSearchId
);

int cellSearchStartSceneSearch(
    CellSearchSceneSearchType  searchType,
    const char                *gameTitle,
    const char               **tags,
    uint32_t                   tagNum,
    CellSearchSortKey          sortKey,
    CellSearchSortOrder        sortOrder,
    CellSearchId              *outSearchId
);

int cellSearchGetContentInfoDeveloperData(
    const CellSearchContentId *contentId,
    char                       developerData[CELL_SEARCH_DEVELOPERDATA_LEN_MAX]
);

int cellSearchGetContentInfoSharable(
    const CellSearchContentId *contentId,
    CellSearchSharableType    *sharable
);

#ifdef __cplusplus
}
#endif

#endif /* __PS3DK_CELL_SYSUTIL_SEARCH_H__ */
