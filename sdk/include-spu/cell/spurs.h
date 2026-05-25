/* cell/spurs.h - SPU-side root SPURS include.
 *
 * Slim SPU-only umbrella.  Excludes PPU syscall surfaces (task.h,
 * workload.h, barrier.h, event_flag.h, queue.h, job_chain.h) that
 * carry extern function declarations and C++ class wrappers which
 * do not exist on SPU.  SPU code that needs those types pulls the
 * individual sub-headers directly.
 *
 * The PPU variant at sdk/include/cell/spurs.h includes the full
 * 17-header surface including the PPU syscall entry points.
 */
#ifndef __PS3DK_CELL_SPURS_H_SPU__
#define __PS3DK_CELL_SPURS_H_SPU__

#include <cell/error.h>

#include <cell/spurs/types.h>
#include <cell/spurs/common.h>
#include <cell/spurs/exception_types.h>
#include <cell/spurs/job_descriptor.h>
#include <cell/spurs/job_commands.h>
#include <cell/spurs/job_chain_types.h>
#include <cell/spurs/version.h>
#include <cell/spurs/job_guard.h>
#include <cell/spurs/task_types.h>
#include <cell/spurs/task_exit_code.h>
#include <cell/spurs/semaphore.h>
#include <cell/spurs/policy_module.h>
#include <cell/spurs/lfqueue.h>

#endif /* __PS3DK_CELL_SPURS_H_SPU__ */
