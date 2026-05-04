/* cell/codec/dmux.h - cellDmux demultiplexer API.
 *
 * Clean-room header.  cellDmux demuxes PAMF (and other container)
 * streams into elementary streams, delivering access units via
 * callback.  This header declares all 20 cellDmux* entry points plus
 * the structs, enums, and callback typedefs the cell SDK ABI requires.
 *
 * Handles are typed as uint32_t — the SPRX writes a 4-byte EA into
 * *handle during create, so LP64 callers must not interpret them as
 * native pointers.
 *
 * Pointer fields crossing the SPRX boundary carry ATTRIBUTE_PRXPTR
 * for 4-byte effective-address storage on LP64 hosts.
 */
#ifndef __PS3DK_CELL_CODEC_DMUX_H__
#define __PS3DK_CELL_CODEC_DMUX_H__

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#include <cell/error.h>
#include <cell/codec/types.h>

#ifdef __PPU__
#include <ppu-types.h>
/* Forward-declare CellSpurs to avoid pulling in the full
 * <cell/spurs/types.h> transitive include chain (which triggers
 * newlib stdlib.h __malloc_like issues on this host).  The SPRX
 * only needs the pointer width, not the full struct. */
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

/*
 * CELL_ERROR_FACILITY_CODEC = 0x061
 * libdmux range: 0x8061_0201 – 0x8061_02ff
 */

#define CELL_DMUX_ERROR_ARG      CELL_ERROR_CAST(0x80610201)
#define CELL_DMUX_ERROR_SEQ      CELL_ERROR_CAST(0x80610202)
#define CELL_DMUX_ERROR_BUSY     CELL_ERROR_CAST(0x80610203)
#define CELL_DMUX_ERROR_EMPTY    CELL_ERROR_CAST(0x80610204)
#define CELL_DMUX_ERROR_FATAL    CELL_ERROR_CAST(0x80610205)

/* ---- enums ------------------------------------------------------ */

typedef enum {
    CELL_DMUX_STREAM_TYPE_UNDEF = 0,
    CELL_DMUX_STREAM_TYPE_PAMF,
    CELL_DMUX_STREAM_TYPE_TERMINATOR,
} CellDmuxStreamType;

typedef enum {
    CELL_DMUX_MSG_TYPE_DEMUX_DONE = 0,
    CELL_DMUX_MSG_TYPE_FATAL_ERR,
    CELL_DMUX_MSG_TYPE_PROG_END_CODE,
} CellDmuxMsgType;

typedef enum {
    CELL_DMUX_ES_MSG_TYPE_AU_FOUND = 0,
    CELL_DMUX_ES_MSG_TYPE_FLUSH_DONE,
} CellDmuxEsMsgType;

/* ---- opaque handles --------------------------------------------- */

/*
 * Opaque handles are uint32_t-sized effective addresses.  The SPRX
 * writes 4 bytes into *handle at create time, and LP64 callers must
 * treat these as opaque tokens (never dereference).
 */
typedef uint32_t CellDmuxHandle;
typedef uint32_t CellDmuxEsHandle;

/* ---- callback message structs ----------------------------------- */

typedef struct {
    uint32_t msgType;
    uint64_t supplementalInfo;
} CellDmuxMsg;

typedef struct {
    uint32_t msgType;
    uint64_t supplementalInfo;
} CellDmuxEsMsg;

/* ---- callback typedefs ------------------------------------------ */

typedef uint32_t (*CellDmuxCbMsg)(
    CellDmuxHandle      demuxerHandle,
    const CellDmuxMsg  *demuxerMsg,
    void               *cbArg
);

typedef uint32_t (*CellDmuxCbEsMsg)(
    CellDmuxHandle       demuxerHandle,
    CellDmuxEsHandle     esHandle,
    const CellDmuxEsMsg *esMsg,
    void                *cbArg
);

/* ---- structs ---------------------------------------------------- */

typedef struct {
    CellDmuxStreamType streamType;
    int32_t            reserved1;
    int32_t            reserved2;
} CellDmuxType;

typedef struct {
    CellDmuxStreamType  streamType;
    void               *streamSpecificInfo ATTRIBUTE_PRXPTR;
} CellDmuxType2;

typedef struct {
    void    *memAddr           ATTRIBUTE_PRXPTR;
    uint32_t memSize;
    uint32_t ppuThreadPriority;
    uint32_t ppuThreadStackSize;
    uint32_t spuThreadPriority;
    uint32_t numOfSpus;
} CellDmuxResource;

typedef struct {
    void      *memAddr           ATTRIBUTE_PRXPTR;
    uint32_t   memSize;
    uint32_t   ppuThreadPriority;
    uint32_t   ppuThreadStackSize;
    CellSpurs *spurs             ATTRIBUTE_PRXPTR;
    uint8_t    priority[8];
    uint32_t   maxContention;
} CellDmuxResourceEx;

typedef struct {
    bool  isResourceEx;
    union {
        CellDmuxResource   resource;
        CellDmuxResourceEx resourceEx;
    } res;
} CellDmuxResource2;

typedef struct {
    CellDmuxCbMsg  cbFunc ATTRIBUTE_PRXPTR;
    void          *cbArg  ATTRIBUTE_PRXPTR;
} CellDmuxCb;

typedef struct {
    CellDmuxCbEsMsg  cbFunc ATTRIBUTE_PRXPTR;
    void            *cbArg  ATTRIBUTE_PRXPTR;
} CellDmuxEsCb;

typedef struct {
    uint32_t memSize;
    uint32_t demuxerVerUpper;
    uint32_t demuxerVerLower;
} CellDmuxAttr;

typedef struct {
    uint32_t memSize;
} CellDmuxEsAttr;

/*
 * ES filter ID — aliased from cell/codec/types.h.
 */
typedef CellCodecEsFilterId CellDmuxEsFilterId;

typedef struct {
    void  *memAddr ATTRIBUTE_PRXPTR;
    uint32_t memSize;
} CellDmuxEsResource;

typedef struct {
    void    *auAddr   ATTRIBUTE_PRXPTR;
    uint32_t auSize;
    uint32_t auMaxSize;
    uint64_t userData;
    uint32_t ptsUpper;
    uint32_t ptsLower;
    uint32_t dtsUpper;
    uint32_t dtsLower;
} CellDmuxAuInfo;

typedef struct {
    void              *auAddr  ATTRIBUTE_PRXPTR;
    uint32_t           auSize;
    uint32_t           reserved;
    bool               isRap;
    uint64_t           userData;
    CellCodecTimeStamp pts;
    CellCodecTimeStamp dts;
} CellDmuxAuInfoEx;

/* ---- API entry points ------------------------------------------- */

int32_t cellDmuxQueryAttr(
    const CellDmuxType *demuxerType,
    CellDmuxAttr       *demuxerAttr
);

int32_t cellDmuxQueryAttr2(
    const CellDmuxType2 *demuxerType2,
    CellDmuxAttr        *demuxerAttr
);

int32_t cellDmuxOpen(
    const CellDmuxType     *demuxerType,
    const CellDmuxResource *demuxerResource,
    const CellDmuxCb       *demuxerCb,
    CellDmuxHandle         *demuxerHandle
);

int32_t cellDmuxOpenEx(
    const CellDmuxType       *demuxerType,
    const CellDmuxResourceEx *demuxerResourceEx,
    const CellDmuxCb         *demuxerCb,
    CellDmuxHandle           *demuxerHandle
);

int32_t cellDmuxOpen2(
    const CellDmuxType2     *demuxerType2,
    const CellDmuxResource2 *demuxerResource2,
    const CellDmuxCb        *demuxerCb,
    CellDmuxHandle          *demuxerHandle
);

int32_t cellDmuxClose(CellDmuxHandle demuxerHandle);

int32_t cellDmuxSetStream(
    CellDmuxHandle demuxerHandle,
    const void    *streamAddress,
    size_t         streamSize,
    bool           discontinuity,
    uint64_t       userData
);

int32_t cellDmuxResetStream(CellDmuxHandle demuxerHandle);

int32_t cellDmuxResetStreamAndWaitDone(CellDmuxHandle demuxerHandle);

int32_t cellDmuxQueryEsAttr(
    const CellDmuxType        *demuxerType,
    const CellCodecEsFilterId *esFilterId,
    const void                *esSpecificInfo,
    CellDmuxEsAttr            *esAttr
);

int32_t cellDmuxQueryEsAttr2(
    const CellDmuxType2       *demuxerType2,
    const CellCodecEsFilterId *esFilterId,
    const void                *esSpecificInfo,
    CellDmuxEsAttr            *esAttr
);

int32_t cellDmuxEnableEs(
    CellDmuxHandle             demuxerHandle,
    const CellCodecEsFilterId *esFilterId,
    const CellDmuxEsResource  *esResource,
    const CellDmuxEsCb        *esCb,
    const void                *esSpecificInfo,
    CellDmuxEsHandle          *esHandle
);

int32_t cellDmuxDisableEs(CellDmuxEsHandle esHandle);

int32_t cellDmuxGetAu(
    CellDmuxEsHandle       esHandle,
    const CellDmuxAuInfo **auInfo,
    void                 **auSpecificInfo
);

int32_t cellDmuxPeekAu(
    CellDmuxEsHandle       esHandle,
    const CellDmuxAuInfo **auInfo,
    void                 **auSpecificInfo
);

int32_t cellDmuxGetAuEx(
    CellDmuxEsHandle         esHandle,
    const CellDmuxAuInfoEx **auInfoEx,
    void                   **auSpecificInfo
);

int32_t cellDmuxPeekAuEx(
    CellDmuxEsHandle         esHandle,
    const CellDmuxAuInfoEx **auInfoEx,
    void                   **auSpecificInfo
);

int32_t cellDmuxReleaseAu(CellDmuxEsHandle esHandle);

int32_t cellDmuxFlushEs(CellDmuxEsHandle esHandle);

int32_t cellDmuxResetEs(CellDmuxEsHandle esHandle);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* __PS3DK_CELL_CODEC_DMUX_H__ */
