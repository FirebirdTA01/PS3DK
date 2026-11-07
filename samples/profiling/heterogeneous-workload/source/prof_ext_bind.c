/* SAMPLE_PROF_EXT target binding: prof_ext.c's port bound to the profiler API and GCM.  Compiled only with
 * SAMPLE_PROF_EXT (either PPU data model, with the uninstalled ext archive of the same data model). */
#include "prof_ext_rt.h"
#include "workload.h"
#include <cell/gcm.h>
#include <ps3dk/prof.h>
#include <stdio.h>
#include <string.h>
#include <sys/sys_time.h>
#include <unistd.h>

#include "prof_gpu.h"
#include "prof_spu.h"
#include "workload_spu_bin.h"

static pe_state g_pe;
static pe_fence_state g_fence;

static uint64_t now_us(void) { return (uint64_t)sys_time_get_system_time(); }

static void b_log(void *c, const char *what, int rc)
{
    (void)c;
    printf("PROF_EXT %s rc=%d\n", what, rc);
}

static int b_init(void *c, unsigned lanes)
{
    (void)c;
    ps3dk_prof_config cfg;
    ps3dk_prof_ext_config ext;
    ps3dk_prof_config_defaults(&cfg);
    cfg.app = "heterogeneous";
    cfg.build_id = SAMPLE_BUILD_ID;
    cfg.workload_tag = "ppu-spe-rsx-loop";
    cfg.mode = PS3DK_PROF_MODE_APP;
    cfg.max_threads = 1;
    cfg.max_descriptors = 64;
    cfg.ring_records = 512;
    memset(&ext, 0, sizeof ext);
    ext.size = PS3DK_PROF_EXT_CONFIG_SIZE;
    ext.spu_lanes_max = lanes ? lanes : 1;
    ext.spu_ring_slots = PE_RING_SLOTS;
    ext.spu_image_bytes_max = PE_IMAGE_BYTES;
    ext.spu_mem_max = PE_MEM_MAX;
    ext.gpu_domains_max = 1;
    ext.gpu_report_slots_max = PE_GPU_REPORT_COUNT;
    int rc = ps3dk_prof_set_sink_path("/dev_hdd0/tmp");
    return rc ? rc : ps3dk_prof_init_ex(&cfg, &ext);
}

static int b_thread_begin(void *c)
{
    (void)c;
    return ps3dk_prof_thread_begin("main");
}

static int b_register(void *c, uint32_t *image)
{
    (void)c;
    return ps3dk_prof_spu_image_register_elf(workload_spu_bin, (uint64_t)workload_spu_bin_size, image);
}

static int b_lane_create(void *c, uint32_t image, pe_lane *out)
{
    (void)c;
    ps3dk_prof_spu_lane l;
    int rc = ps3dk_prof_spu_lane_create("worker", image, 1, &l);
    if (rc == 0) {
        out->handle = l.handle;
        out->ring_slots = l.ring_slots;
        out->ea_ring = l.ea_ring;
        out->ea_ctl = l.ea_ctl;
    }
    return rc;
}

/* ETIMEDOUT keeps the lane charged: call again, a bounded number of times. */
static int b_lane_end(void *c, uint32_t handle)
{
    (void)c;
    int rc = 0;
    for (int k = 0; k < 3; ++k) {
        rc = ps3dk_prof_spu_lane_end(handle, PE_WAIT_US, NULL);
        if (rc != PS3DK_PROF_ETIMEDOUT)
            break;
    }
    return rc;
}

static int b_gpu_attach(void *c, uint32_t *gpu)
{
    (void)c;
    return ps3dk_prof_gpu_attach(gCellGcmCurrentContext, PE_GPU_REPORT_FIRST, PE_GPU_REPORT_COUNT, PE_GPU_LABEL, gpu);
}

static int b_gpu_calibrate(void *c, uint32_t gpu)
{
    (void)c;
    return ps3dk_prof_gpu_calibrate(gpu, gCellGcmCurrentContext, PE_WAIT_US);
}

static int b_drain(void *c)
{
    (void)c;
    return ps3dk_prof_drain();
}

/* The application's completion fence: pe_fence (host-tested) on GCM.  It writes only PE_FENCE_LABEL; label
 * PE_GPU_LABEL belongs to the profiler and is never written by the sample. */
static volatile uint32_t *f_label_addr(void *c, uint32_t label)
{
    (void)c;
    return cellGcmGetLabelAddress(label);
}
static void f_write_label(void *c, uint32_t label, uint32_t value)
{
    (void)c;
    cellGcmSetWriteBackEndLabel(gCellGcmCurrentContext, label, value);
}
static void f_flush(void *c)
{
    (void)c;
    cellGcmFlush(gCellGcmCurrentContext);
}
static void f_barrier(void *c)
{
    (void)c;
    __asm__ volatile("sync" ::: "memory");
}
static uint64_t f_now(void *c)
{
    (void)c;
    return now_us();
}
static void f_sleep(void *c, unsigned us)
{
    (void)c;
    usleep(us);
}
static const pe_fence_io g_fence_io = {NULL, f_label_addr, f_write_label, f_flush, f_barrier, f_now, f_sleep};
static int b_fence(void *c)
{
    (void)c;
    return pe_fence(&g_fence, &g_fence_io);
}

/* ETIMEDOUT while pending: call again, bounded. */
static int b_gpu_detach(void *c, uint32_t gpu)
{
    (void)c;
    int rc = 0;
    for (int k = 0; k < 3; ++k) {
        rc = ps3dk_prof_gpu_detach(gpu, gCellGcmCurrentContext, PE_WAIT_US);
        if (rc != PS3DK_PROF_ETIMEDOUT)
            break;
    }
    return rc;
}

static int b_gpu_release(void *c, uint32_t gpu)
{
    (void)c;
    uint32_t released = 0;
    int rc = ps3dk_prof_gpu_release(gpu, &released);
    return rc ? rc : (released ? 0 : -1);
}

static int b_thread_end(void *c)
{
    (void)c;
    ps3dk_prof_thread_end();
    return 0;
}

static int b_shutdown(void *c)
{
    (void)c;
    return ps3dk_prof_shutdown();
}

static const pe_port g_port = {NULL,        b_init,       b_thread_begin, b_register,   b_lane_create,
                               b_lane_end,  b_gpu_attach, b_gpu_calibrate, b_drain,     b_fence,
                               b_gpu_detach, b_gpu_release, b_thread_end,  b_shutdown,  b_log};

int pe_rt_start(unsigned workers) { return pe_start(&g_pe, &g_port, workers); }
const pe_lane *pe_rt_lane(unsigned i) { return g_pe.spu && i < g_pe.lanes ? &g_pe.lane[i] : NULL; }
int pe_rt_spu_on(void) { return g_pe.spu; }
void pe_rt_spu_off(const char *why) { pe_spu_off(&g_pe, &g_port, why); }
void pe_rt_gpu_start(void) { pe_gpu_start(&g_pe, &g_port); }
void pe_rt_gpu_frame(void)
{
    if (g_pe.gpu) {
        int rc = ps3dk_prof_gpu_frame(g_pe.gpu_id, ps3dk_prof_frame_token());
        if (rc)
            b_log(NULL, "gpu-frame", rc);
    }
}
void pe_rt_gpu_calibrate(void) { pe_gpu_calibrate(&g_pe, &g_port); }
uint32_t pe_rt_gpu(void) { return g_pe.gpu ? g_pe.gpu_id : 0; }
int pe_rt_stop(int join_ok) { return pe_stop(&g_pe, &g_port, join_ok); }
