/* sync2-objects: values shared by the PPU program and the SPU task / job. */
#ifndef SYNC2_OBJECTS_COMMON_H
#define SYNC2_OBJECTS_COMMON_H
#include <stdint.h>

/* SPU task kinds (argTask u32[3]); argTask u64[0] = the task's box EA */
#define K_MUTEX_HOLD   1   /* lock, report 1, wait for go, unlock, report 2 */
#define K_MUTEX_LOCK   2   /* lock (may wait), report 1, unlock, report 2 */
#define K_MUTEX_TRY    3   /* try-lock, report 2 with rc (unlocks if it got it) */
#define K_SEM_ACQ      4   /* acquire n (may wait), report 1, wait for go, release n, report 2 */
#define K_SEM_TRY      5   /* try-acquire n, report 2 with rc */
#define K_SEM_COUNT    6   /* report 2 with the count in v0 */
#define K_COND_WAIT    7   /* lock, report 1, wait on the cond, unlock, report 2 */
#define K_COND_SIGNAL  8   /* lock, signal (n = 1: signal all), unlock, report 2 */
#define K_Q_POP        9   /* pop (may wait), report 2 with the element's first word in v0 */
#define K_Q_PUSH      10   /* push {n, n+1, n+2, n+3} (may wait), report 2 */
#define K_Q_TRYPOP    11   /* try-pop, report 2 with rc and the first word */
#define K_Q_INFO      12   /* report 2 with size in v0 and depth in v1 */

typedef struct box {
    uint32_t go;          /* PPU -> SPU */
    uint32_t n;
    uint64_t obj;         /* the sync2 object's EA */
    uint64_t mutex;       /* the cond's mutex EA */
    uint64_t pad0;
    uint32_t spare[8];
    uint32_t state;       /* SPU -> PPU, at +64: 1 / 2 steps, 0x8000000x error */
    uint32_t rc;
    uint32_t v0;
    uint32_t v1;
    uint32_t pad1[12];
} __attribute__((aligned(128))) box;

/* SPU job: workArea.userData[0] = semaphore EA, [1] = count to release,
 * [2] = EA of a 16-byte result slot {magic, rc, 0, 0} */
#define JOB_MAGIC 0x5e3a0b01u

#endif
