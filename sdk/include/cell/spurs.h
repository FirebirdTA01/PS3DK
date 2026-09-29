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
#include <cell/spurs/trace_types.h>
#include <cell/spurs/common.h>
#include <cell/spurs/task_types.h>
#include <cell/spurs/task.h>
#include <cell/spurs/workload.h>
#include <cell/spurs/trace.h>
#include <cell/spurs/ready_count.h>
#include <cell/spurs/barrier.h>
#include <cell/spurs/event_flag.h>
#include <cell/spurs/queue.h>
#include <cell/spurs/semaphore.h>
#include <cell/spurs/exception_types.h>
#include <cell/spurs/exception.h>
#include <cell/spurs/version.h>
#include <cell/spurs/error.h>
#include <cell/spurs/job_commands.h>
#include <cell/spurs/job_descriptor.h>
#include <cell/spurs/job_guard.h>
#include <cell/spurs/job_chain_types.h>
#include <cell/spurs/job_chain.h>
#include <cell/spurs/control.h>
#include <cell/spurs/lv2_event_queue.h>
#include <cell/spurs/system_workload.h>
#include <cell/spurs/policy_module.h>
#include <cell/spurs/lfqueue.h>
#include <cell/spurs/task_exit_code.h>
#include <cell/spurs/job_queue.h>
#include <cell/spurs/job_queue_semaphore.h>
#include <cell/spurs/job_queue_port.h>
#include <cell/spurs/job_queue_port2.h>

#endif /* __PS3DK_CELL_SPURS_H__ */
