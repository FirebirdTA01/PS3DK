/* cell/sail/converter.h — cellSail video converter.
   Clean-room header. 4 entry points. */
#ifndef __PS3DK_CELL_SAIL_CONVERTER_H__
#define __PS3DK_CELL_SAIL_CONVERTER_H__
#include <stdint.h>
#include <stddef.h>
#include <sys/types.h>
#include <sys/sys_time.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef void (*CellSailVideoConverterFuncProcessDone)(void*);
typedef struct { uint64_t internalData[32]; } CellSailVideoConverter;
int cellSailVideoConverterCanProcess(CellSailVideoConverter*);
int cellSailVideoConverterProcess(CellSailVideoConverter*,void*,size_t,void*,void*,usecond_t,uint32_t);
int cellSailVideoConverterCanGetResult(CellSailVideoConverter*);
int cellSailVideoConverterGetResult(CellSailVideoConverter*,usecond_t,int*,uint32_t*);
#ifdef __cplusplus
}
#endif
#endif
