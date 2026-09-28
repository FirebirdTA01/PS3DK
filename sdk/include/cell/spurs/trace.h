/* cell/spurs/trace.h - SPURS tracing.
 *
 * PPU: set up, start, stop and tear down the trace buffer of a SPURS
 * instance.  SPU: put packets into it - a policy module with
 * cellSpursModulePutTrace, anything running under SPURS with
 * cellSpursPutTrace (which rereads whether tracing is on) or
 * cellSpursPutUserTrace for a user-format packet.
 */
#ifndef __PS3DK_CELL_SPURS_TRACE_H__
#define __PS3DK_CELL_SPURS_TRACE_H__

#include <stdint.h>
#include <cell/trace/trace_types.h>
#include <cell/spurs/trace_types.h>

#ifndef __SPU__
#include <stddef.h>
#include <cell/spurs/types.h>
#endif

#ifdef __cplusplus
extern "C" {
#endif

#ifdef __SPU__

void cellSpursModulePutTrace(CellSpursTracePacket *packet, unsigned tag);
void cellSpursPutTrace(CellSpursTracePacket *packet, unsigned tag);
void cellSpursPutUserTrace(CellTraceHeader *packet, unsigned tag);

#else

int cellSpursTraceInitialize(CellSpurs *spurs, void *buffer, size_t size, uint32_t mode);
int cellSpursTraceStart(CellSpurs *spurs);
int cellSpursTraceStop(CellSpurs *spurs);
int cellSpursTraceFinalize(CellSpurs *spurs);

#endif

#ifdef __cplusplus
}   /* extern "C" */
#endif

#endif /* __PS3DK_CELL_SPURS_TRACE_H__ */
