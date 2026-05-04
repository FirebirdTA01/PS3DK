/* cell/vpost.h — cellVpost video post-processing.
 *
 * Clean-room header. 5 entry points: QueryAttr / Open / OpenEx /
 * Close / Exec.  Handles colour-space conversion (CSC), video
 * scaling (VSC), interlace-to-progressive conversion (IPC), and
 * entry-module (ENT) chains.
 *
 * Pointer fields crossing the SPRX boundary carry ATTRIBUTE_PRXPTR;
 * size_t fields are uint32_t (SPRX 32-bit-pointer ABI).
 */
#ifndef __PS3DK_CELL_VPOST_H__
#define __PS3DK_CELL_VPOST_H__

#include <stdint.h>
#include <stddef.h>
#include <cell/error.h>

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

/* ---- error codes ------------------------------------------------ */
#define CELL_VPOST_ERROR_Q_ARG_CFG_NULL     CELL_ERROR_CAST(0x80610410)
#define CELL_VPOST_ERROR_Q_ARG_CFG_INVALID  CELL_ERROR_CAST(0x80610411)
#define CELL_VPOST_ERROR_Q_ARG_ATTR_NULL    CELL_ERROR_CAST(0x80610412)
#define CELL_VPOST_ERROR_O_ARG_CFG_NULL     CELL_ERROR_CAST(0x80610440)
#define CELL_VPOST_ERROR_O_ARG_CFG_INVALID  CELL_ERROR_CAST(0x80610441)
#define CELL_VPOST_ERROR_O_ARG_RSRC_NULL    CELL_ERROR_CAST(0x80610442)
#define CELL_VPOST_ERROR_O_ARG_RSRC_INVALID CELL_ERROR_CAST(0x80610443)
#define CELL_VPOST_ERROR_O_ARG_HDL_NULL     CELL_ERROR_CAST(0x80610444)

/* ---- handle ----------------------------------------------------- */
typedef uint32_t CellVpostHandle;

/* ---- enums / structs (curated — full set in reference) ----------- */

typedef enum {
    CELL_VPOST_PIC_DEPTH_8,
} CellVpostPictureDepth;

typedef enum {
    CELL_VPOST_PIC_FMT_IN_YUV420_PLANAR,
} CellVpostPictureFormatIn;

typedef enum {
    CELL_VPOST_PIC_FMT_OUT_RGBA_ILV,
    CELL_VPOST_PIC_FMT_OUT_YUV420_PLANAR,
} CellVpostPictureFormatOut;

typedef enum {
    CELL_VPOST_PIC_STRUCT_PFRM,
    CELL_VPOST_PIC_STRUCT_IFRM,
    CELL_VPOST_PIC_STRUCT_ITOP,
    CELL_VPOST_PIC_STRUCT_IBTM,
} CellVpostPictureStructure;

typedef enum {
    CELL_VPOST_EXEC_TYPE_PFRM_PFRM,
    CELL_VPOST_EXEC_TYPE_PTOP_ITOP,
    CELL_VPOST_EXEC_TYPE_PBTM_IBTM,
    CELL_VPOST_EXEC_TYPE_ITOP_PFRM,
    CELL_VPOST_EXEC_TYPE_IBTM_PFRM,
    CELL_VPOST_EXEC_TYPE_IFRM_IFRM,
    CELL_VPOST_EXEC_TYPE_ITOP_ITOP,
    CELL_VPOST_EXEC_TYPE_IBTM_IBTM,
} CellVpostExecType;

typedef enum {
    CELL_VPOST_CHROMA_POS_TYPE_A,
    CELL_VPOST_CHROMA_POS_TYPE_B,
} CellVpostChromaPositionType;

typedef enum {
    CELL_VPOST_SCAN_TYPE_P,
    CELL_VPOST_SCAN_TYPE_I,
} CellVpostScanType;

typedef enum {
    CELL_VPOST_QUANT_RANGE_FULL,
    CELL_VPOST_QUANT_RANGE_BROADCAST,
} CellVpostQuantRange;

typedef enum {
    CELL_VPOST_COLOR_MATRIX_BT601,
    CELL_VPOST_COLOR_MATRIX_BT709,
} CellVpostColorMatrix;

typedef enum {
    CELL_VPOST_SCALER_TYPE_BILINEAR,
    CELL_VPOST_SCALER_TYPE_LINEAR_SHARP,
    CELL_VPOST_SCALER_TYPE_2X4TAP,
    CELL_VPOST_SCALER_TYPE_8X4TAP,
} CellVpostScalerType;

typedef enum {
    CELL_VPOST_IPC_TYPE_DOUBLING,
    CELL_VPOST_IPC_TYPE_LINEAR,
    CELL_VPOST_IPC_TYPE_MAVG,
} CellVpostIpcType;

typedef struct {
    uint32_t                 inMaxWidth, inMaxHeight;
    CellVpostPictureDepth    inDepth;
    CellVpostPictureFormatIn inPicFmt;
    uint32_t                 outMaxWidth, outMaxHeight;
    CellVpostPictureDepth    outDepth;
    CellVpostPictureFormatOut outPicFmt;
    uint32_t                 reserved1, reserved2;
} CellVpostCfgParam;

typedef struct {
    uint32_t memSize;
    uint8_t  delay;
    uint32_t vpostVerUpper;
    uint32_t vpostVerLower;
} CellVpostAttr;

typedef struct {
    void     *memAddr            ATTRIBUTE_PRXPTR;
    uint32_t  memSize;
    int32_t   ppuThreadPriority;
    uint32_t  ppuThreadStackSize;
    int32_t   spuThreadPriority;
    uint32_t  numOfSpus;
} CellVpostResource;

typedef struct {
    void      *memAddr           ATTRIBUTE_PRXPTR;
    uint32_t   memSize;
    CellSpurs *spurs             ATTRIBUTE_PRXPTR;
    uint8_t    priority[8];
    uint32_t   maxContention;
} CellVpostResourceEx;

typedef struct {
    uint32_t x, y, width, height;
} CellVpostWindow;

typedef struct {
    CellVpostExecType          execType;
    CellVpostScalerType        scalerType;
    CellVpostIpcType           ipcType;
    uint32_t                   inWidth, inHeight;
    CellVpostChromaPositionType inChromaPosType;
    CellVpostQuantRange        inQuantRange;
    CellVpostColorMatrix       inColorMatrix;
    CellVpostWindow            inWindow;
    uint32_t                   outWidth, outHeight;
    CellVpostWindow            outWindow;
    uint8_t                    outAlpha;
    uint64_t                   userData;
    uint32_t                   reserved1, reserved2;
} CellVpostCtrlParam;

typedef struct {
    uint32_t                   inWidth, inHeight;
    CellVpostPictureDepth      inDepth;
    CellVpostScanType          inScanType;
    CellVpostPictureFormatIn   inPicFmt;
    CellVpostChromaPositionType inChromaPosType;
    CellVpostPictureStructure  inPicStruct;
    CellVpostQuantRange        inQuantRange;
    CellVpostColorMatrix       inColorMatrix;
    uint32_t                   outWidth, outHeight;
    CellVpostPictureDepth      outDepth;
    CellVpostScanType          outScanType;
    CellVpostPictureFormatOut  outPicFmt;
    CellVpostChromaPositionType outChromaPosType;
    CellVpostPictureStructure  outPicStruct;
    CellVpostQuantRange        outQuantRange;
    CellVpostColorMatrix       outColorMatrix;
    uint64_t                   userData;
    uint32_t                   reserved1, reserved2;
} CellVpostPictureInfo;

/* ---- API (5 entry points) --------------------------------------- */

int32_t cellVpostQueryAttr(const CellVpostCfgParam *cfg, CellVpostAttr *attr);

int32_t cellVpostOpen(const CellVpostCfgParam *cfg, const CellVpostResource *res,
                       CellVpostHandle *handle);

int32_t cellVpostOpenEx(const CellVpostCfgParam *cfg, const CellVpostResourceEx *res,
                         CellVpostHandle *handle);

int32_t cellVpostClose(CellVpostHandle handle);

int32_t cellVpostExec(CellVpostHandle handle, const CellVpostCtrlParam *ctrl,
                       const CellVpostPictureInfo *inp, CellVpostPictureInfo *outp);

#ifdef __cplusplus
}
#endif

#endif /* __PS3DK_CELL_VPOST_H__ */
