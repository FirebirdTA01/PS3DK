#ifndef PS3DK_SPU_PSGL_H
#define PS3DK_SPU_PSGL_H

/* SPU-side PSGL surface (independent, reference-faithful). */

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PSGL_READ_TAG  30u
#define PSGL_WRITE_TAG 31u

void psglSPUInit(unsigned long long initAddr);
void psglSPUWriteMappedBuffer(unsigned long long mappedAddress,
                              const void *localAddress, size_t size);
void psglSPUReadMappedBuffer(unsigned long long mappedAddress,
                              void *localAddress, size_t size);
void glSetMappedEventSCE(unsigned long long event);
void glSetMappedEventWithAddressTagSCE(unsigned long long event,
                                        unsigned int *zeroBuffer,
                                        unsigned int tag);
int psglFillCommandBufferHole(unsigned int mode, int count, unsigned int type,
                               int isIndexMainMemory, uint32_t indexOffset,
                               uint32_t holeSizeInWord, uint32_t *localHoleBuffer);

#ifdef __cplusplus
}
#endif

#endif
