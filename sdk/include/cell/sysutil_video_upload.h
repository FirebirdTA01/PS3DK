/*! \file cell/sysutil_video_upload.h
 \brief Upload a video file to a video site through the system
        (libsysutil_video_upload_stub).

  cellVideoUploadInitialize takes the file, the site (CELL_VIDEO_UPLOAD_
  SITE_ID_*) and that site's account and metadata strings plus optional
  CellVideoUploadOption entries, runs the upload and calls back with
  CELL_VIDEO_UPLOAD_STATUS_INITIALIZED / _FINALIZED, an error code and the
  resulting URL.  The system module reads the parameter structure with
  32-bit pointers, so pointer members keep that width in LP64 programs
  (ATTRIBUTE_PRXPTR): the structure is 48 bytes in both data models.
*/

#ifndef PS3TC_CELL_SYSUTIL_VIDEO_UPLOAD_H
#define PS3TC_CELL_SYSUTIL_VIDEO_UPLOAD_H

#include <stdint.h>
#include <ppu-types.h>   /* ATTRIBUTE_PRXPTR */

#ifdef __cplusplus
extern "C" {
#endif

#define CELL_VIDEO_UPLOAD_ERROR_CANCEL                 ((int)0x8002d000)
#define CELL_VIDEO_UPLOAD_ERROR_NETWORK                ((int)0x8002d001)
#define CELL_VIDEO_UPLOAD_ERROR_SERVICE_STOP           ((int)0x8002d002)
#define CELL_VIDEO_UPLOAD_ERROR_SERVICE_BUSY           ((int)0x8002d003)
#define CELL_VIDEO_UPLOAD_ERROR_SERVICE_UNAVAILABLE    ((int)0x8002d004)
#define CELL_VIDEO_UPLOAD_ERROR_SERVICE_QUOTA          ((int)0x8002d005)
#define CELL_VIDEO_UPLOAD_ERROR_ACCOUNT_STOP           ((int)0x8002d006)
#define CELL_VIDEO_UPLOAD_ERROR_OUT_OF_MEMORY          ((int)0x8002d020)
#define CELL_VIDEO_UPLOAD_ERROR_FATAL                  ((int)0x8002d021)
#define CELL_VIDEO_UPLOAD_ERROR_INVALID_VALUE          ((int)0x8002d022)
#define CELL_VIDEO_UPLOAD_ERROR_FILE_OPEN              ((int)0x8002d023)
#define CELL_VIDEO_UPLOAD_ERROR_INVALID_STATE          ((int)0x8002d024)
#define CELL_VIDEO_UPLOAD_ERROR_INTERNAL_INVALID_USER  ((int)0x8002d040)
#define CELL_VIDEO_UPLOAD_ERROR_INTERNAL_BOUNDARY      ((int)0x8002d041)

/* CellVideoUploadOption.type and the values that go with it. */
#define CELL_VIDEO_UPLOAD_OPTION_TYPE_TITLE_EDIT                0
#define CELL_VIDEO_UPLOAD_OPTION_TYPE_FORCE_USERNAME_FOR_DEBUG  1
#define CELL_VIDEO_UPLOAD_OPTION_TYPE_FORCE_PASSWORD_FOR_DEBUG  2
#define CELL_VIDEO_UPLOAD_OPTION_TYPE_YOUTUBE_DEVELOPER_TAG     3
#define CELL_VIDEO_UPLOAD_OPTION_TYPE_GET_TITLE                 4
#define CELL_VIDEO_UPLOAD_OPTION_VALUE_TITLE_EDIT_ENABLE        0
#define CELL_VIDEO_UPLOAD_OPTION_VALUE_TITLE_EDIT_DISABLE       1
#define CELL_VIDEO_UPLOAD_MAX_YOUTUBE_DEVELOPER_TAG_LEN         20

typedef struct {
	int32_t type;
	uint64_t value;
} CellVideoUploadOption;

#define CELL_VIDEO_UPLOAD_SITE_ID_YOUTUBE                0

#define CELL_VIDEO_UPLOAD_MAX_FILE_PATH_LEN              1023
#define CELL_VIDEO_UPLOAD_MAX_YOUTUBE_CLIENT_ID_LEN      64
#define CELL_VIDEO_UPLOAD_MAX_YOUTUBE_DEVELOPER_KEY_LEN  128
#define CELL_VIDEO_UPLOAD_MAX_YOUTUBE_TITLE_LEN          61
#define CELL_VIDEO_UPLOAD_MAX_YOUTUBE_DESCRIPTION_LEN    1024
#define CELL_VIDEO_UPLOAD_MAX_YOUTUBE_KEYWORD_LEN        25
#define CELL_VIDEO_UPLOAD_MAX_YOUTUBE_USERNAME_LEN       41
#define CELL_VIDEO_UPLOAD_MAX_YOUTUBE_PASSWORD_LEN       31

typedef struct {
	int32_t siteID;
	const char *pFilePath ATTRIBUTE_PRXPTR;
	union {
		struct {
			const char *pClientId ATTRIBUTE_PRXPTR;
			const char *pDeveloperKey ATTRIBUTE_PRXPTR;
			const char *pTitle_UTF8 ATTRIBUTE_PRXPTR;
			const char *pDescription_UTF8 ATTRIBUTE_PRXPTR;
			const char *pKeyword_1_UTF8 ATTRIBUTE_PRXPTR;
			const char *pKeyword_2_UTF8 ATTRIBUTE_PRXPTR;
			const char *pKeyword_3_UTF8 ATTRIBUTE_PRXPTR;
			uint8_t isPrivate;
			uint8_t rating;
		} youtube;
	} u;
	int numOfOption;
	CellVideoUploadOption *pOption ATTRIBUTE_PRXPTR;
} CellVideoUploadParam;

#define CELL_VIDEO_UPLOAD_STATUS_INITIALIZED  1
#define CELL_VIDEO_UPLOAD_STATUS_FINALIZED    2

typedef void (*CellVideoUploadCallback)(int32_t status, int32_t errorCode,
                                        const char *pResultURL, void *userdata);

int32_t cellVideoUploadInitialize(const CellVideoUploadParam *pParam,
                                  CellVideoUploadCallback cb, void *userdata);

#ifdef __cplusplus
}
#endif

#endif /* PS3TC_CELL_SYSUTIL_VIDEO_UPLOAD_H */
