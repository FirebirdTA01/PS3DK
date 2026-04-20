/*! \file cell/sysutil_screenshot.h
 \brief Sony-SDK-source-compat cellScreenShot API.

  PSL1GHT ships no wrappers for libsysutil_screenshot.  Phase 6.5
  closes the gap with a self-contained stub archive
  (libsysutil_screenshot_stub.a) emitted by `nidgen archive` —
  slots, FNIDs, .lib.stub header, sceResident name, .sceStub.text
  trampolines, and .opd descriptors all in one .a so user code links
  with `-lsysutil_screenshot_stub` and the loader resolves the four
  exports against the cellScreenShotUtility SPRX module at runtime.

  FNIDs (verified against reference/sony-sdk/.../libsysutil_screenshot_stub.a
  and tools/nidgen/nids/extracted/libsysutil_screenshot_stub.yaml):
    cellScreenShotEnable            0x9e33ab8f
    cellScreenShotDisable           0xfc6f4e74
    cellScreenShotSetParameter      0xd3ad63e4
    cellScreenShotSetOverlayImage   0x7a9c2243
*/

#ifndef __PSL1GHT_CELL_SYSUTIL_SCREENSHOT_H__
#define __PSL1GHT_CELL_SYSUTIL_SCREENSHOT_H__

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Return + error codes. */
#define CELL_SCREENSHOT_OK                              0
#define CELL_SCREENSHOT_ERROR_INTERNAL                  0x8002d101
#define CELL_SCREENSHOT_ERROR_PARAM                     0x8002d102
#define CELL_SCREENSHOT_ERROR_DECODE                    0x8002d103
#define CELL_SCREENSHOT_ERROR_NOSPACE                   0x8002d104
#define CELL_SCREENSHOT_ERROR_UNSUPPORTED_COLOR_FORMAT  0x8002d105

/* Sizes. */
enum {
	CELL_SCREENSHOT_PHOTO_TITLE_MAX_LENGTH  = 64,
	CELL_SCREENSHOT_GAME_TITLE_MAX_LENGTH   = 64,
	CELL_SCREENSHOT_GAME_COMMENT_MAX_SIZE   = 1024,
};

/* Parameters attached to subsequent screenshot captures. */
typedef struct CellScreenShotSetParam {
	const char *photo_title;
	const char *game_title;
	const char *game_comment;
	void *reserved;
} CellScreenShotSetParam;

/* Function declarations — resolved by libsysutil_screenshot_stub.a. */
int cellScreenShotEnable(void);
int cellScreenShotDisable(void);
int cellScreenShotSetParameter(const CellScreenShotSetParam *param);
int cellScreenShotSetOverlayImage(const char *srcDir, const char *srcFile,
                                  int32_t offset_x, int32_t offset_y);

#ifdef __cplusplus
}
#endif

#endif /* __PSL1GHT_CELL_SYSUTIL_SCREENSHOT_H__ */
