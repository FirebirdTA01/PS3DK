#include "ob_teardown.h"

#include <stdio.h>

int ob_td_gpu(const ob_td_port *p, uint32_t fresh_value)
{
    int rc = p->label_wait(p->ctx, fresh_value);
    if (rc) {
        p->log(p->ctx, "gpu: bench label wait failed: domain retained", rc);
        return OB_TD_RETAINED;
    }
    rc = p->gpu_detach(p->ctx);
    if (rc) {
        p->log(p->ctx, "gpu: detach failed: domain retained", rc);
        return OB_TD_RETAINED;
    }
    rc = p->gpu_release(p->ctx);
    if (rc) {
        p->log(p->ctx, "gpu: release failed", rc);
        return OB_TD_RETAINED;
    }
    return OB_TD_RELEASED;
}

int ob_td_spu(const ob_td_port *p)
{
    int rc = p->spu_join(p->ctx);
    if (rc) {
        p->log(p->ctx, "spu: group join failed: lane retained (no lane_end)", rc);
        return OB_TD_RETAINED;
    }
    rc = p->lane_end(p->ctx);
    if (rc) {
        p->log(p->ctx, "spu: lane_end failed", rc);
        return OB_TD_RETAINED;
    }
    return OB_TD_RELEASED;
}

static int overlap(uint32_t a, uint32_t na, uint32_t b, uint32_t nb) { return a < b + nb && b < a + na; }

int ob_owner_check(const ob_owner *o, char *why, unsigned long n)
{
    if (o->bench_report_count == 0 || o->dom_report_count == 0) {
        snprintf(why, n, "empty report range");
        return -1;
    }
    if (overlap(o->bench_report_first, o->bench_report_count, o->dom_report_first, o->dom_report_count)) {
        snprintf(why, n, "bench reports %u..%u overlap the domain's %u..%u", o->bench_report_first,
                 o->bench_report_first + o->bench_report_count - 1, o->dom_report_first,
                 o->dom_report_first + o->dom_report_count - 1);
        return -1;
    }
    if (o->bench_label == o->dom_label) {
        snprintf(why, n, "bench label %u is the domain's label", o->bench_label);
        return -1;
    }
    if (o->bench_tag == o->writer_tag || o->bench_tag > 31 || o->writer_tag > 31) {
        snprintf(why, n, "bench DMA tag %u vs writer tag %u", o->bench_tag, o->writer_tag);
        return -1;
    }
    if (n)
        why[0] = 0;
    return 0;
}
