/* cell/sail/source_check.h — cellSail source check diagnostic.
   Clean-room header. 1 entry point (cellSailSourceCheck). */
#ifndef __PS3DK_CELL_SAIL_SOURCE_CHECK_H__
#define __PS3DK_CELL_SAIL_SOURCE_CHECK_H__
#include <stdint.h>
#include <cell/sail/common.h>
#include <cell/sail/source.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef void (*CellSailSourceCheckFuncError)(void*,const char*,int);
typedef struct { int streamType; void *pMediaInfo; const char *pUri; } CellSailSourceCheckStream;
typedef struct { CellSailSourceCheckStream ok, readError, openError, startError, runningError; } CellSailSourceCheckResource;
int cellSailSourceCheck(CellSailSource*,const CellSailSourceFuncs*,void*,const char*,CellSailSourceCheckResource*,CellSailSourceCheckFuncError,void*);
#ifdef __cplusplus
}
#endif
#endif
