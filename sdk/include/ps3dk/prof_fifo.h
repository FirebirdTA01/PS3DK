/* FIFO wrap phase events (docs/design/ps3dk-prof-fifo.md).  Only in libps3dkprof_ext_fifo.a, linked with
 * libgcm_cmd_prof_fifo.a in place of libgcm_cmd.a: the default archives do not define these functions.
 *
 * One instance per capture, on the GCM context owner: the thread that attached the GPU domain `gpu` for `ctx`, a
 * registered profiler thread, with the context quiescent.  The capture must run the background drain thread
 * (PS3DK_PROF_MODE_THREAD): an owner parked in a wrap could never drain its own events, so APP-mode draining is
 * refused (EPERM).  Events are PPU timebase wall time of the owner between wrap-protocol boundaries, including
 * scheduling and observer cost; they are never GPU busy or idle time. */
#ifndef PS3DK_PROF_FIFO_H
#define PS3DK_PROF_FIFO_H

#include <stdint.h>

#include <ps3dk/prof.h>
#include <ps3dk/prof_gpu.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ps3dk_prof_fifo_options {
    uint32_t size;          /* sizeof(ps3dk_prof_fifo_options) = 16, else EINVAL */
    uint32_t ring_records;  /* 0 = 1024; else 16..65536 events of 96 bytes */
    uint32_t max_bytes;     /* 0 = 1 MiB; else 5632..16 MiB: the ceiling for ring_records x 96 + 4096 */
    uint32_t reserved;      /* 0, else EINVAL */
} ps3dk_prof_fifo_options;

#define PS3DK_PROF_FIFO_OPTIONS_INIT {16u, 0u, 0u, 0u}

/* options NULL: the defaults.  On success *instance is the nonzero instance id; on every refusal it is 0 and
 * nothing is installed or allocated.  EBUSY inside a drain, sink or GPU call; ENOTINIT; ENOTSUP without GPU
 * domains; EPERM in APP mode or without the drain thread; EINVAL for bad options or ceiling (checked before any
 * allocation), a domain this thread did not attach, another ctx, or a ring the GCM bridge refuses; ESTOPPED after
 * the shutdown claim; ETHREAD for an unregistered thread; EFULL for a second attach in the capture; ENOMEM. */
int ps3dk_prof_fifo_attach(uint32_t gpu, ps3dk_prof_gcm_ctx *ctx, const ps3dk_prof_fifo_options *options,
                           uint32_t *instance);
/* On the owner: removes the observer and publishes the final owner counters; never waits (the drain thread writes
 * the remaining events and the FINAL status).  EBUSY inside the observer callback; EPERM from another thread;
 * EINVAL for another instance; ESTOPPED once the capture is sealed (its storage then stays allocated). */
int ps3dk_prof_fifo_detach(uint32_t instance);

#ifdef __cplusplus
}
#endif

#endif
