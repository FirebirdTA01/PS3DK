#include "ob_rsx.h"

unsigned ob_rsx_variant(const ob_rsx_port *p, int variant, uint32_t report_first, uint32_t label_value)
{
    unsigned bad = 0;
    p->report(p->ctx, report_first);
    for (uint32_t i = 0; i < OB_DRAWS; ++i) {
        if (variant == OB_G1)
            bad += p->zone_begin(p->ctx, i) != 0;
        else if (variant == OB_G2)
            p->nop(p->ctx, OB_ZONE_ENDPOINT_WORDS);
        p->draw(p->ctx, i);
        if (variant == OB_G1)
            bad += p->zone_end(p->ctx, i) != 0;
        else if (variant == OB_G2)
            p->nop(p->ctx, OB_ZONE_ENDPOINT_WORDS);
    }
    p->report(p->ctx, report_first + 1u);
    p->label(p->ctx, label_value);
    p->flush(p->ctx);
    return bad;
}
