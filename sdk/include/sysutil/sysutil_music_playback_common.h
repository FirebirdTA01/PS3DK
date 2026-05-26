/*
 * PS3 Custom Toolchain - sysutil/sysutil_music_playback_common.h
 *
 * CellMusicSelectionContext: 2048-byte opaque selection context
 * shared across cellMusicDecode / cellMusicPlayback v1 and v2 APIs.
 */
#ifndef _PS3DK_SYSUTIL_MUSIC_PLAYBACK_COMMON_H_
#define _PS3DK_SYSUTIL_MUSIC_PLAYBACK_COMMON_H_

#ifdef __cplusplus
extern "C" {
#endif

#define CELL_MUSIC_SELECTION_CONTEXT_SIZE 2048

typedef struct CellMusicSelectionContext {
	char data[CELL_MUSIC_SELECTION_CONTEXT_SIZE];
} CellMusicSelectionContext;

#ifdef __cplusplus
}
#endif

#endif /* _PS3DK_SYSUTIL_MUSIC_PLAYBACK_COMMON_H_ */
