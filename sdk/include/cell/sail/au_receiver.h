/* cell/sail/au_receiver.h — cellSail AU receiver (pull-style).
   Clean-room header. 3 entry points. */
#ifndef __PS3DK_CELL_SAIL_AU_RECEIVER_H__
#define __PS3DK_CELL_SAIL_AU_RECEIVER_H__
#include <stdint.h>
#include <cell/sail/common.h>
#ifdef __cplusplus
extern "C" {
#endif
enum { CELL_SAIL_SYNC_MODE_REPEAT=1<<0, CELL_SAIL_SYNC_MODE_SKIP=1<<1 };
typedef struct { uint64_t internalData[64]; } CellSailAuReceiver;
int cellSailAuReceiverInitialize(CellSailAuReceiver*,void*,void*,int);
int cellSailAuReceiverFinalize(CellSailAuReceiver*);
int cellSailAuReceiverGet(CellSailAuReceiver*,uint64_t,uint32_t,uint64_t,const CellSailAuInfo**);
#ifdef __cplusplus
}
#endif
#endif
