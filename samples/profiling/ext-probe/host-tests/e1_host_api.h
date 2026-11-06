/* The host harness entry points main.c's fakes call (e1_host.c).  HOST ONLY. */
#ifndef E1_HOST_API_H
#define E1_HOST_API_H

#include <stddef.h>
#include <stdint.h>

#include "../source/e1_report.h"
#include "prof_gpu.h"

/* the fault script (env E1_FAULTS) and the trace (env E1_TRACE); thread-safe */
const char *e1h_f(const char *key);              /* the value of key ("" for a bare key), or NULL */
int e1h_fval(const char *key, uint32_t *v);      /* 1 when key=<hex> is set */
void e1h_trace(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
const char *e1h_base(void);                      /* env E1_BASE_DIR */

/* the host SPU: the real e1_spu_core + writer on a thread */
void e1h_spu_args(uint64_t ea_ring, uint32_t ring_slots, uint64_t ea_ctl, uint64_t report_eas);
void e1h_spu_start(e1_line *payload, e1_line *commit);
int e1h_spu_write_mb(uint32_t v);
void e1h_spu_terminate(void);
void e1h_spu_join(void);
void e1h_after_copy(uint32_t s);
int e1h_register_image(uint32_t *out);

/* the GCM device (R4a Mode S) */
ps3dk_prof_gcm_ctx *e1h_gcm_ctx(void);
void e1h_gcm_flush(ps3dk_prof_gcm_ctx *ctx);
void e1h_gcm_finish(ps3dk_prof_gcm_ctx *ctx, uint32_t ref);

/* file seams */
int e1h_mk_open(const char *path, unsigned seq);
long e1h_mk_write(int fd, const void *b, size_t n, unsigned seq);
int e1h_mk_fsync(int fd, unsigned seq);
int e1h_mk_close(int fd, unsigned seq);
int e1h_mkdir(const char *path);
int e1h_cap_open(const char *path);
long e1h_cap_read(int fd, void *b, size_t n);

#endif
