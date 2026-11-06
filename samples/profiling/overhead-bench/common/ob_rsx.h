/* overhead-bench RSX section (Plan A r2 3.1c): a fixed sequence of 64 small draws in variants g0 (no zones), g1 (one
 * GPU zone per draw) and g2 (no zones, the same number of command words as g1's zones written as NOP methods: g2
 * controls command-buffer size only, it is not equivalent GPU work, and g1 - g2 is qualified that way).  Every variant
 * is bracketed by the bench's own timestamp reports (begin, end) and its own label.  Portable C: the target binds the
 * port to GCM and the profiler's GPU zone calls; the host tests count the command words.
 * A GPU zone is two endpoints of OB_ZONE_ENDPOINT_WORDS words each (src/rsx/gpu_port_gcm.c: 6-word endpoints), so a
 * zone adds 48 bytes; g2 writes exactly those 12 words per draw as NOPs. */
#ifndef OB_RSX_H
#define OB_RSX_H

#include <stdint.h>

#define OB_DRAWS 64u
#define OB_ZONE_ENDPOINT_WORDS 6u
#define OB_ZONE_BYTES (2u * OB_ZONE_ENDPOINT_WORDS * 4u)

enum { OB_G0 = 0, OB_G1 = 1, OB_G2 = 2 };

typedef struct ob_rsx_port {
    void *ctx;
    void (*report)(void *ctx, uint32_t index);           /* the bench's timestamp report */
    void (*label)(void *ctx, uint32_t value);            /* the bench's back-end label */
    void (*draw)(void *ctx, uint32_t i);                 /* one small draw */
    int (*zone_begin)(void *ctx, uint32_t i);            /* the profiler's GPU zone endpoints (g1 only): 0 or rc */
    int (*zone_end)(void *ctx, uint32_t i);
    void (*nop)(void *ctx, uint32_t words);              /* NOP methods (g2 only) */
    void (*flush)(void *ctx);
} ob_rsx_port;

/* one variant: report(first), 64 draws (with zones or NOPs), report(first + 1), label(value), flush.  Returns the
 * number of zone calls that returned nonzero (0 for g0/g2).  Every call is made whatever an earlier one returned, so
 * the command stream of a variant does not depend on a refusal. */
unsigned ob_rsx_variant(const ob_rsx_port *p, int variant, uint32_t report_first, uint32_t label_value);

#endif
