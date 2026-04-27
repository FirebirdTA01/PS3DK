/* cell/spurs/job_descriptor.h - SPURS job descriptor + DMA-list types.
 *
 * Public ABI for the structures the SPU job runtime DMAs in to set up
 * each job: a 64-byte CellSpursJobHeader followed by a 64/128/256-byte
 * payload that holds the input DMA list and (overlay) per-job user
 * data.  Sizes other than 64 must be a multiple of 128 and <= 1024.
 */
#ifndef __PS3DK_CELL_SPURS_JOB_DESCRIPTOR_H__
#define __PS3DK_CELL_SPURS_JOB_DESCRIPTOR_H__

#include <stdint.h>
#include <cell/spurs/error.h>

/* Total scratch+heap memory available to a job binary, before the
 * runtime starts overflowing to system memory. */
#define CELL_SPURS_MAX_SIZE_JOB_MEMORY            (234 * 1024)
#define CELL_SPURS_JOBQUEUE_MAX_SIZE_JOB_MEMORY   (221 * 1024)

#define CELL_SPURS_JOB_BINARY_CACHE_INHIBIT       1

/* CellSpursJobContext2 — passed to SPU-side cellSpursJobMain2 entry
 * point.  Describes the I/O buffers + DMA tag the job runtime set up
 * for the job before invoking it.  Layout matches the reference
 * SDK 475 cell/spurs/job_context_types.h exactly. */
typedef struct CellSpursJobContext2 {
    void *   ioBuffer;
    void *   cacheBuffer[4];
    uint32_t sizeJobDescriptor : 4;
    uint32_t pad               : 28;
    uint16_t numIoBuffer;
    uint16_t numCacheBuffer;
    void *   oBuffer;
    void *   sBuffer;
    unsigned int dmaTag;
    uint64_t eaJobDescriptor;
} CellSpursJobContext2;

typedef struct CellSpursJobInputDataElement {
    uint32_t size;
    void *   pointer;
} CellSpursJobInputDataElement;

/* CellSpursJobHeader.jobType bitset. */
#define CELL_SPURS_JOB_TYPE_STALL_SUCCESSOR       1
#define CELL_SPURS_JOB_TYPE_MEMORY_CHECK          2
#define CELL_SPURS_JOB_TYPE_BINARY2               4

#ifdef __cplusplus
extern "C" {
#endif

typedef struct CellSpursJobID {
    uint32_t notify : 2;   /* 0=none, 1=poll, 2=event, 3=reserved */
    uint32_t id     : 30;
} CellSpursJobID;

typedef union CellSpursJobInputList {
    struct {
        uint32_t size;     /* 0..0x4000 bytes */
        uint32_t eal;      /* low 32 bits of effective address */
    } asInputList;
    uint64_t asUint;
} CellSpursJobInputList;

typedef union CellSpursJobCacheList {
    struct {
        uint32_t size;
        uint32_t eal;
    } asCacheList;
    uint64_t asUint;
} CellSpursJobCacheList;

/* 64-byte job header.  __packed__ on the eaBinary/sizeBinary union
 * matches the SPRX wire layout - sizeBinary is at offset 8, sizeDmaList
 * at 10, eaHighInput at 12, etc. */
typedef struct CellSpursJobHeader {
    __extension__ union {
        __extension__ struct {
            uint64_t eaBinary;                                 /* offset 0 */
            uint16_t sizeBinary __attribute__((__packed__));   /* offset 8, units of 16 bytes */
        } __attribute__((__packed__));
        uint8_t  binaryInfo[10];   /* CELL_SPURS_JOB_TYPE_BINARY2 form */
    };
    uint16_t sizeDmaList     __attribute__((__packed__));   /* offset 10 */
    uint32_t eaHighInput     __attribute__((__packed__));   /* offset 12 */
    uint32_t useInOutBuffer;                                /* 0 = input, !=0 = in/out */
    uint32_t sizeInOrInOut;
    uint32_t sizeOut;
    uint16_t sizeStack;                                     /* qword units */
    __extension__ union {
        uint16_t sizeHeap __attribute__((deprecated));
        uint16_t sizeScratch;
    };
    uint32_t eaHighCache;
    uint32_t sizeCacheDmaList;
    __extension__ union {
        CellSpursJobID idJob __attribute__((deprecated));
        uint8_t        __reserved2__[4];
    };
    __extension__ union {
        uint32_t __reserved__ __attribute__((deprecated));
        __extension__ struct {
            uint8_t jobType;
            uint8_t __reserved3__[3];
        };
    };
} __attribute__((__aligned__(16))) CellSpursJobHeader;

/* Helper - shift a byte size into the 16-byte units sizeBinary stores. */
#define CELL_SPURS_GET_SIZE_BINARY(size_in_byte) \
    ((uint16_t)(((uintptr_t)(size_in_byte) + 15) >> 4))

/* Stamp out fixed-size job descriptors: header + workArea (DMA list
 * entries overlaid with user-data words). */
#define DEFINE_TYPE_CELL_SPURS_JOB(N, N_LIST)                                  \
typedef struct _CellSpursJob##N {                                              \
    CellSpursJobHeader header;                                                 \
    uint64_t           dmaList[N_LIST];                                        \
    uint64_t           userData[(N - sizeof(CellSpursJobHeader)) / sizeof(uint64_t) - N_LIST]; \
} CellSpursJob##N;

typedef struct CellSpursJob64 {
    CellSpursJobHeader header;
    union {
        uint64_t dmaList[2];
        uint64_t userData[2];
    } workArea;
} __attribute__((aligned(16))) CellSpursJob64;

typedef struct CellSpursJob128 {
    CellSpursJobHeader header;
    union {
        uint64_t dmaList[10];
        uint64_t userData[10];
    } workArea;
} __attribute__((aligned(128))) CellSpursJob128;

typedef struct CellSpursJob256 {
    CellSpursJobHeader header;
    union {
        uint64_t dmaList[26];
        uint64_t userData[26];
    } workArea;
} __attribute__((aligned(128))) CellSpursJob256;

#define CELL_SPURS_JOB_LIST_ALIGN  16
#define CELL_SPURS_JOB_LIST_SIZE   16

typedef struct CellSpursJobList {
    uint32_t numJobs;
    uint32_t sizeOfJob;
    uint64_t eaJobList;
} __attribute__((aligned(16))) CellSpursJobList;

#ifdef __PPU__
extern int cellSpursJobHeaderSetJobbin2Param(CellSpursJobHeader *header, const void *jobbin2);
#endif
#ifdef __SPU__
extern int cellSpursJobHeaderSetJobbin2Param(CellSpursJobHeader *header, uint64_t eaJobbin2);
#endif

#ifdef __cplusplus
}   /* extern "C" */
#endif

/* -- Inline helpers --------------------------------------------------
 *
 * Pack input/cache DMA-list entries into a single uint64_t with the
 * size in the high 32 bits and the EA low half in the low 32 bits.
 * Bounds match the constraints documented for the SPU job runtime:
 * size must be a power-of-two below 16, or a multiple-of-16 up to
 * 16K; eal must be aligned to min(size, 16).  */
static inline int cellSpursJobGetInputList(uint64_t *inputList,
                                           uint16_t size,
                                           uint32_t eal)
{
    if ((size > 0x4000) ||
        (size > 0x10 && (size & 0xf)) ||
        (size & (size - 1) & 0xf))
        return CELL_SPURS_JOB_ERROR_INVAL;
    if (size && (eal & ((size - 1) & 0xf)))
        return CELL_SPURS_JOB_ERROR_ALIGN;
    *inputList = ((uint64_t)size << 32) | (uint64_t)eal;
    return CELL_OK;
}

static inline int cellSpursJobGetCacheList(uint64_t *cacheList,
                                           uint32_t size,
                                           uint32_t eal)
{
    int ret = ((size & 0xf) | (eal & 0xf)) ? CELL_SPURS_JOB_ERROR_ALIGN : CELL_OK;
    *cacheList = ((uint64_t)size << 32) | (uint64_t)eal;
    return ret;
}

/* Front-end alignment / size sanity check.  Mirrors the documented
 * job-descriptor invariants: 16-byte EA alignment, sizeOut/sizeIn
 * multiples of 16, sizeDmaList/sizeCacheDmaList multiples of 8, and
 * sizeJob in {64, 128, 256, ..., 1024} bounded by maxSizeJob.  Real
 * memory-budget checks happen on the SPU side. */
static inline int cellSpursCheckJob(const CellSpursJob256 *pJob,
                                    unsigned int sizeJob,
                                    unsigned int maxSizeJob)
{
    uint32_t eaBin;
    if (maxSizeJob < 256 || maxSizeJob > 1024 || (maxSizeJob % 128))
        return CELL_SPURS_JOB_ERROR_INVAL;
    if (pJob == 0)
        return CELL_SPURS_JOB_ERROR_NULL_POINTER;
    eaBin = (uint32_t)(pJob->header.eaBinary & ~1ull);
    if (eaBin == 0)
        return CELL_SPURS_JOB_ERROR_NULL_POINTER;
    if (((uintptr_t)pJob % 16) ||
        (eaBin % 16) ||
        (pJob->header.sizeOut % 16) ||
        (pJob->header.sizeInOrInOut % 16) ||
        (pJob->header.sizeDmaList % 8) ||
        (pJob->header.sizeCacheDmaList % 8))
        return CELL_SPURS_JOB_ERROR_ALIGN;
    if ((pJob->header.sizeBinary == 0) ||
        (pJob->header.sizeDmaList + pJob->header.sizeCacheDmaList >
         sizeJob - sizeof(CellSpursJobHeader)) ||
        (pJob->header.sizeCacheDmaList / 8 > 4) ||
        (sizeJob != 64 &&
         ((sizeJob % 128) || (sizeJob > 1024) || (sizeJob < 64))) ||
        sizeJob > maxSizeJob)
        return CELL_SPURS_JOB_ERROR_INVAL;
    return CELL_OK;
}

#ifdef __cplusplus
namespace cell {
namespace Spurs {

/* Templated job descriptors.  tSize must be 64 or a multiple of 128
 * up to 1024; the static-assert dummy below fires if not. */
template <unsigned int tSize>
class Job : public CellSpursJobHeader {
private:
    int __dummyForAssert__[((tSize > 0 && tSize <= 1024 && (tSize % 128) == 0) || (tSize == 64)) ? 0 : -1];
public:
    static const int NUM_DATA = (tSize - sizeof(CellSpursJobHeader)) / 8;
    union {
        uint64_t dmaList[NUM_DATA];
        uint64_t userData[NUM_DATA];
    } workArea;

#ifndef __SPU__
    int setJobbin2Param(const void *jobbin2)
    { return cellSpursJobHeaderSetJobbin2Param(this, jobbin2); }
#else
    int setJobbin2Param(uint64_t eaJobbin2)
    { return cellSpursJobHeaderSetJobbin2Param(this, eaJobbin2); }
#endif

    int checkForJobChain(unsigned int maxSizeJob = 1024)
    {
        if (__builtin_expect(maxSizeJob < 256 || maxSizeJob > 1024 || (maxSizeJob % 128), 0))
            return CELL_SPURS_JOB_ERROR_INVAL;
        return cellSpursCheckJob((const CellSpursJob256 *)this, tSize, maxSizeJob);
    }
    int checkForJobQueue(unsigned int maxSizeJob = 896)
    {
        if (__builtin_expect(maxSizeJob < 256 || maxSizeJob >= 1024 || (maxSizeJob % 128), 0))
            return CELL_SPURS_JOB_ERROR_INVAL;
        return cellSpursCheckJob((const CellSpursJob256 *)this, tSize, maxSizeJob);
    }
};

template <typename tType, unsigned int tSize>
class JobTypeOf : public CellSpursJobHeader {
private:
    int __dummyForAssert__[((sizeof(tType) % 8) == 0 &&
                            ((tSize > 0 && tSize <= 1024 && (tSize % 128) == 0) || (tSize == 64))) ? 0 : -1];
public:
    static const int NUM_DMA = (tSize - sizeof(CellSpursJobHeader) - sizeof(tType)) / 8;
    CellSpursJobInputList dmaList[NUM_DMA];
    tType                 userData;

#ifndef __SPU__
    int setJobbin2Param(const void *jobbin2)
    { return cellSpursJobHeaderSetJobbin2Param(this, jobbin2); }
#else
    int setJobbin2Param(uint64_t eaJobbin2)
    { return cellSpursJobHeaderSetJobbin2Param(this, eaJobbin2); }
#endif

    int checkForJobChain(unsigned int maxSizeJob = 1024)
    {
        if (__builtin_expect(maxSizeJob < 256 || maxSizeJob > 1024 || (maxSizeJob % 128), 0))
            return CELL_SPURS_JOB_ERROR_INVAL;
        return cellSpursCheckJob((const CellSpursJob256 *)this, tSize, maxSizeJob);
    }
    int checkForJobQueue(unsigned int maxSizeJob = 896)
    {
        if (__builtin_expect(maxSizeJob < 256 || maxSizeJob >= 1024 || (maxSizeJob % 128), 0))
            return CELL_SPURS_JOB_ERROR_INVAL;
        return cellSpursCheckJob((const CellSpursJob256 *)this, tSize, maxSizeJob);
    }
};

}   /* namespace Spurs */
}   /* namespace cell */
#endif /* __cplusplus */

#endif /* __PS3DK_CELL_SPURS_JOB_DESCRIPTOR_H__ */
