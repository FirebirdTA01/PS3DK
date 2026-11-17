/* Positioned FILE adapter for the private rolling store. Not runtime admission. */
#ifndef PS3DK_PROF_ROLL_FILE_H
#define PS3DK_PROF_ROLL_FILE_H
#include "roll_store.h"
#define pfr_file_bind ps3dk_prof__roll_file_bind
typedef struct {
    int fd;
    uint64_t extent;
    /* Local validation/host errno, or raw LV2 syscall error. Zero after
     * successful binding; diagnostic only, never a substitute for checking
     * each IO result. */
    uint32_t error;
} pfr_file;
/* Borrow an already exclusively opened read/write descriptor; never open or
 * close it. The sole output owner serializes all access, including size/sync.
 * LV2 positioning may change the descriptor cursor. Calls refuse any offset,
 * length or resize outside extent (which must fit INT64_MAX), before IO.
 * No EINTR retry: an interrupted operation is an IO failure for the store.
 * Positive short transfers are returned for the store to handle. A resize is
 * successful only if the observed length equals its request. Sparse resize
 * does not reserve physical storage; sync is not a console durability proof. */
int pfr_file_bind(pfr_file *, pfr_io *, int fd, uint64_t extent);
#endif
