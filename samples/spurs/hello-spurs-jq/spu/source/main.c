/*
 * hello-spurs-jq SPU side - skeleton-link validation.
 *
 * The JQ scheduler binary's entry point is cellSpursJobMain2 from
 * libspurs_jq.a (NOT user-provided).  The user-defined entry below
 * is unreachable in a JQ workload - it's here so the SPU build has
 * SOMETHING to compile + the linker can confirm libspurs_jq.a's
 * symbols resolve.
 *
 * Once libspurs_jq's runtime is ported beyond stubs, the JQ
 * dispatcher will route incoming jobs to user-provided entries
 * resolved at run time via per-job-descriptor function pointers,
 * and this file's contents will be replaced by an actual job-body
 * for the validation test.
 */

#include <stdint.h>
#include <spu_intrinsics.h>
#include <spu_mfcio.h>

#include <cell/spurs/job_chain.h>
#include <cell/spurs/job_context.h>

/* Force libspurs_jq.a to be pulled in by referencing one of its
 * entry points from a no-op stub.  The actual JQ dispatcher loop
 * lives in libspurs_jq's cellSpursJobMain2 and overrides this
 * symbol via the link order. */
extern int cellSpursJobQueueClose(uint64_t eaJobQueue, int handle);

void hello_spurs_jq_unused_link_anchor(void)
{
    (void)cellSpursJobQueueClose(0, 0);
}
