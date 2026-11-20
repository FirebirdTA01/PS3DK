/* SPDX-License-Identifier: BSD-2-Clause
 * The producer's memset calls bind to this PIC job-owned routine, not an
 * ordinary SPU libc archive built for fixed addresses. Volatile byte stores
 * prevent loop-to-memset replacement. No allocator or other libc is needed. */
#include <stddef.h>
void *sj_memset(void *dst, int value, size_t n)
{
    volatile unsigned char *p = (volatile unsigned char *)dst;
    while (n--) *p++ = (unsigned char)value;
    return dst;
}
