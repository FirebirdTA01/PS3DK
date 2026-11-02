/* P3.3 R3 target LINK fixture (never run): references every device op of the GCM port through the bound prg_port.
 * A link witness only; not a functional test.  R3_LINK_CONTROL: the negative control references nothing of the
 * port, so the archive member must not be pulled in. */
#include <stdint.h>

#include "gpu_port_gcm.h"

volatile uint64_t r3_sink;

#ifdef R3_LINK_CONTROL
int main(int argc, char **argv)
{
    (void)argv;
    r3_sink += (uint64_t)argc;
    return (int)(r3_sink & 1);
}
#else
static gpp_port pc;

int main(int argc, char **argv)
{
    prg_port p = {0};
    (void)argv;
    if (gpp_prepare(&pc, &gpp_dev_gcm, gCellGcmCurrentContext, 0, 4, 64) != PRG_OK)
        return 1;
    gpp_bind(&p, &pc);
    r3_sink += (uint64_t)p.reserve_pair(p.ctx, 0, 64, (uint32_t)argc);
    p.flush(p.ctx);
    r3_sink += p.read_label(p.ctx, 64);
    p.write_label(p.ctx, 64, (uint32_t)argc);
    r3_sink += p.read_timer(p.ctx, 1);
    p.write_timer(p.ctx, 1, PRG_POISON);
    p.sync(p.ctx);
    gpp_release(&pc);
    return (int)(r3_sink & 1);
}
#endif
