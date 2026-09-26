/* Compile the four SPU reservation primitives in C and C++.  The header's
 * alignment predicate has type bool in C++, but the SPU halt intrinsic
 * accepts only signed or unsigned 32-bit integer operands. */
#include <cell/atomic.h>

#ifdef __cplusplus
extern "C" {
#endif

uint32_t atomic_load32(uint32_t *line, uint64_t address)
{
    return cellAtomicLockLine32(line, address);
}

int atomic_store32(uint32_t *line, uint64_t address, uint32_t value)
{
    return cellAtomicStoreConditional32(line, address, value);
}

uint64_t atomic_load64(uint64_t *line, uint64_t address)
{
    return cellAtomicLockLine64(line, address);
}

int atomic_store64(uint64_t *line, uint64_t address, uint64_t value)
{
    return cellAtomicStoreConditional64(line, address, value);
}

#ifdef __cplusplus
}
#endif
