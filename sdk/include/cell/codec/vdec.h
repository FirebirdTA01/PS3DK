/* cell/codec/vdec.h - cellVdec video decoder framework (MPEG-2, AVC, DivX).
 *
 * Independent header.  11 entry points: QueryAttr(Ex) / Open(Ex) / Close /
 * StartSeq / EndSeq / DecodeAu / GetPicture / GetPicItem / SetFrameRate,
 * imported through libvdec_stub.
 *
 * Same conventions as cell/codec/adec.h: pointer fields crossing the SPRX
 * boundary carry ATTRIBUTE_PRXPTR, size_t fields in caller-filled structs
 * are uint32_t (the module uses 32-bit pointers), handles are uint32_t. */
#ifndef PS3TC_CELL_CODEC_VDEC_H
#define PS3TC_CELL_CODEC_VDEC_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <cell/error.h>
#include <cell/codec/types.h>

#ifdef __PPU__
#include <ppu-types.h>
typedef struct CellSpurs CellSpurs;
#else
#ifndef ATTRIBUTE_PRXPTR
#define ATTRIBUTE_PRXPTR
#endif
#endif

#ifdef __cplusplus
extern "C" {
#endif

#define CELL_VDEC_ERROR_ARG    CELL_ERROR_CAST(0x80610101)
#define CELL_VDEC_ERROR_SEQ    CELL_ERROR_CAST(0x80610102)
#define CELL_VDEC_ERROR_BUSY   CELL_ERROR_CAST(0x80610103)
#define CELL_VDEC_ERROR_EMPTY  CELL_ERROR_CAST(0x80610104)
#define CELL_VDEC_ERROR_AU     CELL_ERROR_CAST(0x80610105)
#define CELL_VDEC_ERROR_PIC    CELL_ERROR_CAST(0x80610106)
#define CELL_VDEC_ERROR_FATAL  CELL_ERROR_CAST(0x80610180)

#define CELL_VDEC_PTS_INVALID  CELL_CODEC_PTS_INVALID
#define CELL_VDEC_DTS_INVALID  CELL_CODEC_DTS_INVALID

typedef enum {
	CELL_VDEC_CODEC_TYPE_MPEG2 = 0,
	CELL_VDEC_CODEC_TYPE_AVC   = 1,
	CELL_VDEC_CODEC_TYPE_DIVX  = 5,
	CELL_VDEC_CODEC_TYPE_MAX
} CellVdecCodecType;

/* what a callback reports */
typedef enum {
	CELL_VDEC_MSG_TYPE_AUDONE,    /* an access unit has been consumed */
	CELL_VDEC_MSG_TYPE_PICOUT,    /* a decoded picture is available */
	CELL_VDEC_MSG_TYPE_SEQDONE,   /* cellVdecEndSeq finished */
	CELL_VDEC_MSG_TYPE_ERROR
} CellVdecMsgType;

typedef enum {
	CELL_VDEC_DEC_MODE_NORMAL,
	CELL_VDEC_DEC_MODE_B_SKIP,    /* skip non-reference B pictures */
	CELL_VDEC_DEC_MODE_PB_SKIP    /* skip non-reference P and B pictures */
} CellVdecDecodeMode;

typedef enum {
	CELL_VDEC_PICFMT_ARGB32_ILV,
	CELL_VDEC_PICFMT_RGBA32_ILV,
	CELL_VDEC_PICFMT_UYVY422_ILV,
	CELL_VDEC_PICFMT_YUV420_PLANAR
} CellVdecPicFormatType;

typedef enum {
	CELL_VDEC_COLOR_MATRIX_TYPE_BT601,
	CELL_VDEC_COLOR_MATRIX_TYPE_BT709
} CellVdecColorMatrixType;

typedef enum {
	CELL_VDEC_PICITEM_ATTR_NORMAL,
	CELL_VDEC_PICITEM_ATTR_SKIPPED
} CellVdecPicAttr;

/* frame-rate codes for cellVdecSetFrameRate */
typedef enum {
	CELL_VDEC_FRC_24000DIV1001 = 0x80,
	CELL_VDEC_FRC_24           = 0x81,
	CELL_VDEC_FRC_25           = 0x82,
	CELL_VDEC_FRC_30000DIV1001 = 0x83,
	CELL_VDEC_FRC_30           = 0x84,
	CELL_VDEC_FRC_50           = 0x85,
	CELL_VDEC_FRC_60000DIV1001 = 0x86,
	CELL_VDEC_FRC_60           = 0x87
} CellVdecFrameRate;

/* opaque: the module writes a 4-byte handle */
typedef uint32_t CellVdecHandle;

typedef struct CellVdecType {
	CellVdecCodecType codecType;
	uint32_t          profileLevel;
} CellVdecType;

typedef struct CellVdecTypeEx {
	CellVdecCodecType codecType;
	uint32_t          profileLevel;
	void             *codecSpecificInfo ATTRIBUTE_PRXPTR;
} CellVdecTypeEx;

typedef struct CellVdecAttr {
	uint32_t memSize;          /* bytes the decoder needs */
	uint8_t  cmdDepth;         /* access units that can be queued */
	uint32_t decoderVerUpper;
	uint32_t decoderVerLower;
} CellVdecAttr;

typedef struct CellVdecResource {
	void    *memAddr ATTRIBUTE_PRXPTR;
	uint32_t memSize;
	int32_t  ppuThreadPriority;
	uint32_t ppuThreadStackSize;
	int32_t  spuThreadPriority;
	uint32_t numOfSpus;
} CellVdecResource;

typedef struct CellVdecResourceSpurs {
	CellSpurs *spursAddr ATTRIBUTE_PRXPTR;
	uint8_t    tasksetPriority[8];
	uint32_t   tasksetMaxContention;
} CellVdecResourceSpurs;

typedef struct CellVdecResourceEx {
	void                  *memAddr ATTRIBUTE_PRXPTR;
	uint32_t               memSize;
	int32_t                ppuThreadPriority;
	uint32_t               ppuThreadStackSize;
	int32_t                spuThreadPriority;
	uint32_t               numOfSpus;
	CellVdecResourceSpurs *spursResource ATTRIBUTE_PRXPTR;
} CellVdecResourceEx;

typedef CellCodecTimeStamp CellVdecTimeStamp;

/* one access unit handed to cellVdecDecodeAu */
typedef struct CellVdecAuInfo {
	void              *startAddr ATTRIBUTE_PRXPTR;
	uint32_t           size;
	CellCodecTimeStamp pts;
	CellCodecTimeStamp dts;
	uint64_t           userData;
	uint64_t           codecSpecificData;
} CellVdecAuInfo;

/* a decoded picture, from cellVdecGetPicItem */
typedef struct CellVdecPicItem {
	CellVdecCodecType  codecType;
	void              *startAddr ATTRIBUTE_PRXPTR;
	uint32_t           size;
	uint8_t            auNum;
	CellCodecTimeStamp auPts[2];
	CellCodecTimeStamp auDts[2];
	uint64_t           auUserData[2];
	int32_t            status;
	CellVdecPicAttr    attr;
	void              *picInfo ATTRIBUTE_PRXPTR;  /* codec-specific picture info */
} CellVdecPicItem;

typedef struct CellVdecPicFormat {
	CellVdecPicFormatType   formatType;
	CellVdecColorMatrixType colorMatrixType;
	uint8_t                 alpha;
} CellVdecPicFormat;

typedef uint32_t (*CellVdecCbMsg)(CellVdecHandle handle, CellVdecMsgType msgType,
                                  int32_t msgData, void *cbArg);

typedef struct CellVdecCb {
	CellVdecCbMsg cbFunc ATTRIBUTE_PRXPTR;
	void         *cbArg  ATTRIBUTE_PRXPTR;
} CellVdecCb;

int32_t cellVdecQueryAttr(const CellVdecType *type, CellVdecAttr *attr);
int32_t cellVdecQueryAttrEx(const CellVdecTypeEx *type, CellVdecAttr *attr);
int32_t cellVdecOpen(const CellVdecType *type, const CellVdecResource *resource,
                     const CellVdecCb *callback, CellVdecHandle *handle);
int32_t cellVdecOpenEx(const CellVdecTypeEx *type, const CellVdecResourceEx *resource,
                       const CellVdecCb *callback, CellVdecHandle *handle);
int32_t cellVdecClose(CellVdecHandle handle);
int32_t cellVdecStartSeq(CellVdecHandle handle);
int32_t cellVdecEndSeq(CellVdecHandle handle);
int32_t cellVdecDecodeAu(CellVdecHandle handle, CellVdecDecodeMode mode,
                         const CellVdecAuInfo *auInfo);
int32_t cellVdecGetPicture(CellVdecHandle handle, const CellVdecPicFormat *format,
                           void *outBuff);
int32_t cellVdecGetPicItem(CellVdecHandle handle, const CellVdecPicItem **picItem);
int32_t cellVdecSetFrameRate(CellVdecHandle handle, CellVdecFrameRate frameRate);

#ifdef __cplusplus
}
#endif

#endif /* PS3TC_CELL_CODEC_VDEC_H */
