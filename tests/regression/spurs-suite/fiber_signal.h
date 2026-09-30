/* spu-fiber-signal row: values shared by the PPU and the SPU task. */
#ifndef SUITE_FIBER_SIGNAL_H
#define SUITE_FIBER_SIGNAL_H
#include <stdint.h>

#define FS_UTIL_SIGNAL   1   /* signal fiber A through its worker control */
#define FS_PLAIN_SIGNAL  2   /* signal fiber B, then wake a worker */

/* argTask u64[0] = box EA, u32[3] = kind */
typedef struct fs_box {
    uint32_t fiber;       /* PPU -> SPU: the fiber's EA */
    uint32_t runtime;     /* the fiber runtime (worker control) EA */
    uint32_t pad[14];
    uint32_t state;       /* SPU -> PPU at +64: 2 when done */
    uint32_t rc;          /* first nonzero rc */
    uint32_t numWorker;
    uint32_t scheduler;   /* cellFiberPpuGetScheduler's answer */
    uint32_t pad1[12];
} __attribute__((aligned(128))) fs_box;

#endif
