/* cell/spurs.h - root SPURS include.
 *
 * Minimum surface needed to build reference SDK libspurs core samples.
 * Currently pulls in just types (opaque byte-array structs + C++ class
 * wrappers) - richer sub-surfaces (barrier, event_flag, queue, task,
 * job_chain, etc) land as more samples surface them.
 */
#ifndef __PS3DK_CELL_SPURS_H__
#define __PS3DK_CELL_SPURS_H__

#include <cell/spurs/types.h>
#include <cell/spurs/task_types.h>
#include <cell/spurs/task.h>
#include <cell/spurs/workload.h>
#include <cell/spurs/barrier.h>
#include <cell/spurs/event_flag.h>
#include <cell/spurs/queue.h>
#include <cell/spurs/semaphore.h>

#endif /* __PS3DK_CELL_SPURS_H__ */
