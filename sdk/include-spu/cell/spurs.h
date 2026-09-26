/* cell/spurs.h - SPU-side root SPURS include.
 *
 * SPU-only umbrella pulling in the shared type headers, the SPU task
 * runtime surface (task.h), the EA-based SPU forms of the task sync
 * primitives (barrier, event flag, queue, lock-free queue, semaphore)
 * and the SPU job-queue surface.  Excludes PPU syscall surfaces
 * (workload.h) and job_chain.h (include that directly from job code).
 */
#ifndef __PS3DK_CELL_SPURS_H_SPU__
#define __PS3DK_CELL_SPURS_H_SPU__

#include <stdint.h>
#include <cell/error.h>

#include <cell/spurs/types.h>
#include <cell/spurs/error.h>
#include <cell/spurs/common.h>
#include <cell/spurs/exception_types.h>
#include <cell/spurs/job_descriptor.h>
#include <cell/spurs/job_commands.h>
#include <cell/spurs/job_chain_types.h>
#include <cell/spurs/version.h>
#include <cell/spurs/job_guard.h>
#include <cell/spurs/task_types.h>
#include <cell/spurs/task.h>
#include <cell/spurs/task_exit_code.h>
#include <cell/spurs/barrier.h>
#include <cell/spurs/event_flag.h>
#include <cell/spurs/queue.h>
#include <cell/spurs/lfqueue.h>
#include <cell/spurs/semaphore.h>
#include <cell/spurs/policy_module.h>
#include <cell/spurs/job_queue.h>
#include <cell/spurs/job_queue_semaphore.h>
#include <cell/spurs/job_queue_port.h>
#include <cell/spurs/job_queue_port2.h>

/* CELL_SPURS_PPU_SYM(sym): the 32-bit PPU address of the PPU symbol
 * `sym`, for use in SPU code.  The SPU object records a `sym@ppu`
 * reference (R_SPU_PPU32) in a data word.
 *
 * Limitation: the SDK's embedding paths (ps3_add_spu_image, bin2s and
 * spu-elf-to-ppu-obj) do not yet carry that relocation into the PPU
 * object, so the PPU link does not fill the word in.  Code using this
 * macro compiles but reads an unresolved address until the embedding
 * tools promote R_SPU_PPU32. */
#define CELL_SPURS_PPU_SYM(sym)                                             \
    (__extension__({                                                        \
        extern char __ps3dk_ppu_sym_##sym[] __asm__(#sym "@ppu");           \
        static char *volatile __ps3dk_ppu_ref_##sym = __ps3dk_ppu_sym_##sym; \
        (unsigned int)(uintptr_t)__ps3dk_ppu_ref_##sym;                     \
    }))

#endif /* __PS3DK_CELL_SPURS_H_SPU__ */
