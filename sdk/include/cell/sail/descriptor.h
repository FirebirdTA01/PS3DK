/* cell/sail/descriptor.h — cellSail descriptor (stream metadata).
   Clean-room header. 13 entry points. */
#ifndef __PS3DK_CELL_SAIL_DESCRIPTOR_H__
#define __PS3DK_CELL_SAIL_DESCRIPTOR_H__
#include <stdint.h>
#include <stddef.h>
#include <cell/sail/common.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef struct { uint64_t internalData[32]; } CellSailDescriptor;
int cellSailDescriptorGetStreamType(CellSailDescriptor*);
const char* cellSailDescriptorGetUri(CellSailDescriptor*);
void* cellSailDescriptorGetMediaInfo(CellSailDescriptor*);
int cellSailDescriptorSetAutoSelection(CellSailDescriptor*,bool);
int cellSailDescriptorIsAutoSelection(CellSailDescriptor*);
int cellSailDescriptorCreateDatabase(CellSailDescriptor*,void*,uint32_t,uint64_t);
int cellSailDescriptorDestroyDatabase(CellSailDescriptor*,void*);
int cellSailDescriptorOpen(CellSailDescriptor*);
int cellSailDescriptorClose(CellSailDescriptor*);
int cellSailDescriptorSetEs(CellSailDescriptor*,int,unsigned,bool,void*);
int cellSailDescriptorClearEs(CellSailDescriptor*,int,unsigned);
int cellSailDescriptorGetCapabilities(CellSailDescriptor*,uint32_t*,uint32_t*,uint32_t*,uint32_t*);
int cellSailDescriptorInquireCapability(CellSailDescriptor*,CellSailStartCommand*);
enum { CELL_SAIL_PERFORM_SEEK_AND_READ=0, CELL_SAIL_PERFORM_READ_ONLY=1 };
#ifdef __cplusplus
}
#endif
#endif
