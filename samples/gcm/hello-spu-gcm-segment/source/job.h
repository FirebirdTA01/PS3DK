/* The records the PPU and the SPU job exchange (same layout on both). */
#ifndef HELLO_SPU_GCM_SEGMENT_JOB_H
#define HELLO_SPU_GCM_SEGMENT_JOB_H

#include <stdint.h>

#define JOB_EXIT_FRAME 0xffffffffu

typedef struct {
    uint64_t segment_ea;       /* main-memory segment the SPU fills          */
    uint32_t frame;            /* 1-based; the SPU echoes it when done       */
    uint32_t slot;             /* which double-buffer slot (progress label)  */
    uint32_t width, height;
    uint32_t segment_bytes;
    uint32_t progress_label;
} __attribute__((aligned(16))) SegJob;

typedef struct {
    uint32_t frame;            /* job.frame once the segment is in memory;
                                  JOB_EXIT_FRAME just before the SPU exits   */
    uint32_t used_bytes;       /* bytes written including the RETURN         */
    uint32_t measured_bytes;   /* size the measuring context predicted, +4   */
    uint32_t status;           /* 0 ok, 1 too big, 2 end refused, 3 DMA      */
    uint32_t overflow_refused; /* one-shot check: 1 if End refused overflow  */
    uint32_t overflow_words;   /* words the one-shot check refused           */
    uint32_t pad[2];
} __attribute__((aligned(16))) SegDone;

_Static_assert(sizeof(SegJob) == 32, "SegJob layout");
_Static_assert(sizeof(SegDone) == 32, "SegDone layout");

#endif
