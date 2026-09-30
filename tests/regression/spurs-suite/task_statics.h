/* spurs-task-cpp-statics row: values shared by the PPU and the SPU task. */
#ifndef SUITE_TASK_STATICS_H
#define SUITE_TASK_STATICS_H
#include <stdint.h>

#define TS_SEED  0x1111u   /* the static Counter is built from this: value = 3 * seed */

/* argTask u64[0] = box EA */
typedef struct ts_box {
    uint32_t state;       /* 2 when done */
    uint32_t value;       /* the static Counter's value, as its constructor left it */
    uint32_t marks;       /* how many Mark constructors ran */
    uint32_t order;       /* their ids, first in the high byte */
} __attribute__((aligned(16))) ts_box;

#endif
