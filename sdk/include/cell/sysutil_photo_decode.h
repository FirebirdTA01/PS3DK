/*! \file cell/sysutil_photo_decode.h
 \brief Decode a photo file into a caller buffer with the system's decoder
        (libsysutil_photo_decode_stub).

  Initialise (Initialize takes two memory containers, Initialize2 one),
  decode a file into dstBuffer at the requested size - the size actually
  produced comes back in CellPhotoDecodeReturnParam - then finalise.  The
  finish callbacks report CELL_PHOTO_DECODE_RET_* or an error.  Pointer
  members stay 32 bits wide in LP64 programs (ATTRIBUTE_PRXPTR), as the
  system module reads them.
*/

#ifndef PS3TC_CELL_SYSUTIL_PHOTO_DECODE_H
#define PS3TC_CELL_SYSUTIL_PHOTO_DECODE_H

#include <stdint.h>
#include <ppu-types.h>       /* ATTRIBUTE_PRXPTR */
#include <sys/memory.h>      /* sys_memory_container_t */
#include <sysutil/sysutil_common.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CELL_PHOTO_DECODE_RET_OK              0
#define CELL_PHOTO_DECODE_RET_CANCEL          1
#define CELL_PHOTO_DECODE_ERROR_BUSY          0x8002c901
#define CELL_PHOTO_DECODE_ERROR_INTERNAL      0x8002c902
#define CELL_PHOTO_DECODE_ERROR_PARAM         0x8002c903
#define CELL_PHOTO_DECODE_ERROR_ACCESS_ERROR  0x8002c904
#define CELL_PHOTO_DECODE_ERROR_INITIALIZE    0x8002c905
#define CELL_PHOTO_DECODE_ERROR_DECODE        0x8002c906

typedef enum {
	CELL_PHOTO_DECODE_VERSION_CURRENT = 0
} CellPhotoDecodeVersion;

typedef struct {
	void *dstBuffer ATTRIBUTE_PRXPTR;
	uint16_t width;
	uint16_t height;
	void *reserved1 ATTRIBUTE_PRXPTR;
	void *reserved2 ATTRIBUTE_PRXPTR;
} CellPhotoDecodeSetParam;

typedef struct {
	uint16_t width;
	uint16_t height;
	void *reserved1 ATTRIBUTE_PRXPTR;
	void *reserved2 ATTRIBUTE_PRXPTR;
} CellPhotoDecodeReturnParam;

typedef void (*CellPhotoDecodeFinishCallback)(int result, void *userdata);

int cellPhotoDecodeInitialize(unsigned int version, sys_memory_container_t container1,
                              sys_memory_container_t container2,
                              CellPhotoDecodeFinishCallback funcFinish, void *userdata);
int cellPhotoDecodeInitialize2(unsigned int version, sys_memory_container_t container2,
                               CellPhotoDecodeFinishCallback funcFinish, void *userdata);
int cellPhotoDecodeFromFile(const char *srcPath, const char *srcFilename,
                            const CellPhotoDecodeSetParam *set_param,
                            CellPhotoDecodeReturnParam *return_param);
int cellPhotoDecodeFinalize(CellPhotoDecodeFinishCallback funcFinish, void *userdata);

#ifdef __cplusplus
}
#endif

#endif /* PS3TC_CELL_SYSUTIL_PHOTO_DECODE_H */
