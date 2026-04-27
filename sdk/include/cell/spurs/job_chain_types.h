/* cell/spurs/job_chain_types.h - SPURS job-chain opaque container,
 * attribute container, info struct, and exception-handler typedef.
 *
 * Sizes match the SPRX wire layout.  Pointer fields in
 * CellSpursJobChainInfo cross the SPRX boundary, so they're tagged
 * ATTRIBUTE_PRXPTR to shrink to the 4-byte EAs the SPRX writes.
 */
#ifndef __PS3DK_CELL_SPURS_JOB_CHAIN_TYPES_H__
#define __PS3DK_CELL_SPURS_JOB_CHAIN_TYPES_H__

#include <stdint.h>
#include <stdbool.h>
#include <ppu-types.h>          /* ATTRIBUTE_PRXPTR */
#include <cell/spurs/types.h>
#include <cell/spurs/exception_types.h>
#include <cell/spurs/job_descriptor.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Library revision tag carried by JobChainAttributeInitialize. */
#define CELL_SPURS_JOB_REVISION                  3
#define CELL_SPURS_JOBCHAIN_CLASS_NAME           "JobChain"

#define CELL_SPURS_JOBCHAIN_ALIGN                128
#define CELL_SPURS_JOBCHAIN_SIZE                 272

typedef struct CellSpursJobChain {
    unsigned char skip[CELL_SPURS_JOBCHAIN_SIZE];
} __attribute__((aligned(CELL_SPURS_JOBCHAIN_ALIGN))) CellSpursJobChain;

/* For arrays of job chains: the bare CellSpursJobChain has 128-byte
 * alignment but only 272 bytes of storage, so consecutive entries
 * would land off-alignment.  Pad to 384 bytes (3 * align) so each
 * entry sits on a 128-byte boundary. */
typedef struct CellSpursJobChainForArray {
    CellSpursJobChain jobChain;
    uint8_t           padding[128 * 3 - CELL_SPURS_JOBCHAIN_SIZE];
} __attribute__((aligned(CELL_SPURS_JOBCHAIN_ALIGN))) CellSpursJobChainForArray;

#define CELL_SPURS_JOBCHAIN_ATTRIBUTE_ALIGN      8
#define CELL_SPURS_JOBCHAIN_ATTRIBUTE_SIZE       512

typedef struct CellSpursJobChainAttribute {
    unsigned char skip[CELL_SPURS_JOBCHAIN_ATTRIBUTE_SIZE];
} __attribute__((aligned(CELL_SPURS_JOBCHAIN_ATTRIBUTE_ALIGN))) CellSpursJobChainAttribute;

typedef void (*CellSpursJobChainExceptionEventHandler)(
    CellSpurs *,
    CellSpursJobChain *,
    const CellSpursExceptionInfo *,
    const void *,
    uint32_t,
    void *);

/* 264-byte info struct returned by cellSpursGetJobChainInfo.  Pointer
 * fields are SPRX-written and use ATTRIBUTE_PRXPTR. */
typedef struct CellSpursJobChainInfo {
    uint64_t                                urgentCommandSlot[4];
    const uint64_t                         *programCounter                ATTRIBUTE_PRXPTR;
    /* SPRX writes 32-bit EAs here.  Cast to (const uint64_t *) when
     * dereferencing - per-element ATTRIBUTE_PRXPTR can't be applied to
     * an array of pointers under LP64 PPU. */
    uint32_t                                linkRegister[3];
    const void                             *cause                         ATTRIBUTE_PRXPTR;
    uint32_t                                statusCode;
    uint32_t                                maxSizeJobDescriptor;
    CellSpursWorkloadId                     idWorkload;
    __extension__ union {
        bool autoReadyCount;
        bool autoRequestSpuCount;
    };
    bool                                    isHalted;
    bool                                    isFixedMemAlloc;
    uint8_t                                 padding8;
    uint16_t                                maxGrabbedJob;
    uint16_t                                padding16;
    const char                             *name                          ATTRIBUTE_PRXPTR;
    CellSpursJobChainExceptionEventHandler  exceptionEventHandler         ATTRIBUTE_PRXPTR;
    void                                   *exceptionEventHandlerArgument ATTRIBUTE_PRXPTR;
    uint8_t                                 reserved[264
                                                     - sizeof(uint64_t) * 4
                                                     - 4               /* PRXPTR programCounter */
                                                     - sizeof(uint32_t) * 3 /* linkRegister[3]   */
                                                     - 4               /* PRXPTR cause          */
                                                     - sizeof(uint32_t) * 2
                                                     - sizeof(CellSpursWorkloadId)
                                                     - sizeof(bool) * 3
                                                     - sizeof(uint8_t)
                                                     - sizeof(uint16_t) * 2
                                                     - 4               /* PRXPTR name           */
                                                     - 4 * 2           /* PRXPTR handler + arg  */];
} CellSpursJobChainInfo;

/* Per-stage job-pipeline snapshot returned by cellSpursGetJobPipelineInfo.
 * Each stage holds the full 1024-byte job descriptor (largest variant)
 * plus a GUID + reserved padding to 128 bytes.  No PRXPTR fields. */
typedef struct CellSpursJobPipelineInfo {
    struct {
        __extension__ union {
            uint8_t           jobDescriptorStorage[1024];
            CellSpursJob256   job;
        };
        uint8_t guid[8];
        uint8_t reserved[128 - sizeof(uint8_t) * 8];
    } fetchStage;
    struct {
        __extension__ union {
            uint8_t           jobDescriptorStorage[1024];
            CellSpursJob256   job;
        };
        uint8_t guid[8];
        uint8_t reserved[128 - sizeof(uint8_t) * 8];
    } inputStage;
    struct {
        __extension__ union {
            uint8_t           jobDescriptorStorage[1024];
            CellSpursJob256   job;
        };
        uint8_t guid[8];
        uint8_t reserved[128 - sizeof(uint8_t) * 8];
    } executeStage;
    struct {
        __extension__ union {
            uint8_t           jobDescriptorStorage[1024];
            CellSpursJob256   job;
        };
        uint8_t guid[8];
        uint8_t reserved[128 - sizeof(uint8_t) * 8];
    } outputStage;
} CellSpursJobPipelineInfo;

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* __PS3DK_CELL_SPURS_JOB_CHAIN_TYPES_H__ */
