/* overhead-bench: ownership and teardown (Plan A r2 3.1e, the accepted sample's discipline).  Portable C; the target
 * binds the port to GCM / LV2 / the profiler, the host tests (test_ob_units.c) to a mock with a failure at every step.
 *
 * GPU: wait for the bench label with a FRESH value (the queue drained past the bench's commands) -> gpu_detach with
 *      its wait -> gpu_release only after both succeeded.  Any failure retains the domain (no release) and is logged.
 * SPU: the bench group's join must succeed before lane_end; a failed join retains the lane (no lane_end).
 * Ownership (checked by ob_owner_check before any bench command): the bench's report indices and label lie outside
 * the profiler domain's report range and label; the bench's SPU DMA tag differs from the writer's; the ring belongs
 * to its lane only. */
#ifndef OB_TEARDOWN_H
#define OB_TEARDOWN_H

#include <stdint.h>

typedef struct ob_td_port {
    void *ctx;
    int (*label_wait)(void *ctx, uint32_t value); /* 0: the label reached value within its bound */
    int (*gpu_detach)(void *ctx);                 /* 0: detached (with its wait) */
    int (*gpu_release)(void *ctx);
    int (*spu_join)(void *ctx);
    int (*lane_end)(void *ctx);
    void (*log)(void *ctx, const char *what, int rc);
} ob_td_port;

enum { OB_TD_RELEASED = 0, OB_TD_RETAINED = 1 };

int ob_td_gpu(const ob_td_port *p, uint32_t fresh_value);
int ob_td_spu(const ob_td_port *p);

typedef struct ob_owner {
    uint32_t dom_report_first, dom_report_count, dom_label; /* the profiler GPU domain */
    uint32_t bench_report_first, bench_report_count, bench_label;
    uint32_t writer_tag, bench_tag;
} ob_owner;
/* 0 when nothing overlaps; else -1 and the first overlap in why */
int ob_owner_check(const ob_owner *o, char *why, unsigned long n);

#endif
