/* overhead-bench target RSX binding: common/ob_gpu_run.c's port bound to GCM and the profiler's GPU calls.  The
 * section's rules (capacity, block discipline, failure flags, close) live in ob_gpu_run.h.  A1: compiled for the
 * audit only; nothing here is launched before the A2 gates.
 * A "small draw" is one 8x8 scissored colour clear on a 64x64 offscreen surface (no shader program in A1). */
#include <cell/gcm.h>
#include <rsx/rsx.h>
#include <string.h>
#include <unistd.h>

#include <ps3dk/prof.h>

#include "../common/ob_timer.h"
#include "gcm_host_buffer.h"
#include "ob_bind.h"
#include "prof_gpu.h"

static uint32_t g_gpu;
static ps3dk_prof_gpu_zone g_zone[OB_DRAWS];

static gcmContextData *ctx(void) { return gCellGcmCurrentContext; }

/* ---- the ob_rsx port ---- */
static void r_report(void *c, uint32_t i)
{
    (void)c;
    cellGcmSetTimeStamp(ctx(), i);
}
static void r_label(void *c, uint32_t v)
{
    (void)c;
    cellGcmSetWriteBackEndLabel(ctx(), OB_BENCH_LABEL, v);
}
static void r_draw(void *c, uint32_t i)
{
    (void)c;
    cellGcmSetScissor(ctx(), (i % 8u) * 8u, (i / 8u) * 8u, 8, 8);
    cellGcmSetClearColor(ctx(), 0xFF000000u | i);
    cellGcmSetClearSurface(ctx(), CELL_GCM_CLEAR_R | CELL_GCM_CLEAR_G | CELL_GCM_CLEAR_B | CELL_GCM_CLEAR_A);
}
static int r_zone_begin(void *c, uint32_t i)
{
    (void)c;
    return PS3DK_PROF_GPU_ZONE_BEGIN(g_gpu, ctx(), "ob.g1.draw", g_zone[i]);
}
static int r_zone_end(void *c, uint32_t i)
{
    (void)c;
    return PS3DK_PROF_GPU_ZONE_END(g_gpu, ctx(), g_zone[i]);
}
static void r_nop(void *c, uint32_t words)
{
    (void)c;
    cellGcmSetNopCommand(ctx(), words);
}
static void r_flush(void *c)
{
    (void)c;
    cellGcmFlush(ctx());
}
static const ob_rsx_port g_rsx = {NULL, r_report, r_label, r_draw, r_zone_begin, r_zone_end, r_nop, r_flush};

/* ---- the ob_gpu port ---- */
static int g_init(void *c)
{
    (void)c;
    sys_addr_t host = 0;
    uint32_t off = 0;
    if (gcm_host_buffer_alloc(1u << 20, &host) || cellGcmInit(512u << 10, 1u << 20, (void *)(uintptr_t)host))
        return -1;
    void *cb = rsxMemalign(64, 64u * 64u * 4u);
    if (!cb || cellGcmAddressToOffset(cb, &off))
        return -1;
    CellGcmSurface sf;
    memset(&sf, 0, sizeof sf);
    sf.colorFormat = GCM_SURFACE_X8R8G8B8;
    sf.colorTarget = GCM_SURFACE_TARGET_0;
    for (unsigned i = 0; i < 4; ++i) {
        sf.colorLocation[i] = GCM_LOCATION_RSX;
        sf.colorPitch[i] = 64;
    }
    sf.colorOffset[0] = off;
    sf.colorPitch[0] = 64u * 4u;
    sf.depthFormat = GCM_SURFACE_ZETA_Z16;
    sf.depthLocation = GCM_LOCATION_RSX;
    sf.depthOffset = off; /* no depth test or depth clear is ever issued */
    sf.depthPitch = 64u * 4u;
    sf.type = GCM_SURFACE_TYPE_LINEAR;
    sf.antiAlias = GCM_SURFACE_CENTER_1;
    sf.width = 64;
    sf.height = 64;
    cellGcmSetSurface(ctx(), &sf);
    return 0;
}
static void g_label_store(void *c, uint32_t v)
{
    (void)c;
    *(volatile uint32_t *)cellGcmGetLabelAddress(OB_BENCH_LABEL) = v;
    __asm__ volatile("sync" ::: "memory");
}
static uint32_t g_label_read(void *c)
{
    (void)c;
    return *(volatile uint32_t *)cellGcmGetLabelAddress(OB_BENCH_LABEL);
}
static void g_pause(void *c)
{
    (void)c;
    usleep(10);
}
static int g_attach(void *c, uint32_t first, uint32_t count, uint32_t label, uint32_t *gpu)
{
    (void)c;
    int rc = ps3dk_prof_gpu_attach(ctx(), first, count, label, gpu);
    if (rc == 0)
        g_gpu = *gpu;
    return rc;
}
static int g_calibrate(void *c, uint32_t gpu)
{
    (void)c;
    return ps3dk_prof_gpu_calibrate(gpu, ctx(), OB_WAIT_US);
}
static int g_detach(void *c, uint32_t gpu)
{
    (void)c;
    int rc = 0;
    for (int k = 0; k < 3; ++k) { /* ETIMEDOUT while pending: call again, bounded */
        rc = ps3dk_prof_gpu_detach(gpu, ctx(), OB_WAIT_US);
        if (rc != PS3DK_PROF_ETIMEDOUT)
            break;
    }
    return rc;
}
static int g_release(void *c, uint32_t gpu)
{
    (void)c;
    uint32_t released = 0;
    int rc = ps3dk_prof_gpu_release(gpu, &released);
    return rc ? rc : (released ? 0 : -1);
}
static int g_drain(void *c)
{
    (void)c;
    return ps3dk_prof_drain();
}
static uint64_t g_tb(void *c)
{
    (void)c;
    return ob_tb();
}
static uint32_t *g_current(void *c)
{
    (void)c;
    return ctx()->current;
}
static uint64_t g_report_time(void *c, uint32_t index)
{
    (void)c;
    return cellGcmGetTimeStamp(index);
}

static const ob_gpu_port g_port = {NULL,    g_init,    g_label_store, g_label_read, g_pause,     g_attach,
                                   g_calibrate, g_detach, g_release,  g_drain,      g_tb,        g_current,
                                   g_report_time, NULL,  &g_rsx};

const ob_gpu_port *ob_gpu_port_gcm(void) { return &g_port; }
