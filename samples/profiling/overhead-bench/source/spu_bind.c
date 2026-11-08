/* overhead-bench target SPU binding: common/ob_spu_life.c's port bound to LV2 and the profiler, and the profiler's
 * init_ex (one SPU lane, one GPU domain).  Every call's result is returned to the lifecycle, which owns the state
 * rules (ob_spu_life.h).  A1: compiled for the audit only; nothing here is launched before the A2 gates. */
#include <string.h>
#include <sys/spu.h>
#include <unistd.h>

#include <ps3dk/prof.h>

#include "../common/ob_cases.h"
#include "ob_bind.h"
#include "ob_spu_bin.h"
#include "prof_spu.h"
#include "../common/ob_timer.h"

static ob_spu_res g_res; /* the reply block: static, never released (the SPU may DMA into it while retained) */
static sysSpuImage g_image;
static sys_spu_group_t g_group;
static sys_spu_thread_t g_thread;

int ob_prof_init(void)
{
    ps3dk_prof_config cfg;
    ps3dk_prof_ext_config ext;
    ps3dk_prof_config_defaults(&cfg);
    cfg.mode = PS3DK_PROF_MODE_APP;
    cfg.ring_records = OB_RING_RECORDS;
    cfg.max_threads = 16;
    cfg.app = "overhead-bench";
    memset(&ext, 0, sizeof ext);
    ext.size = PS3DK_PROF_EXT_CONFIG_SIZE;
    ext.spu_lanes_max = 1;
    ext.spu_ring_slots = OB_SPU_RING_SLOTS;
    ext.spu_image_bytes_max = OB_SPU_IMAGE_BYTES;
    ext.spu_mem_max = OB_EXT_MEM_MAX;
    ext.gpu_domains_max = 1;
    ext.gpu_report_slots_max = OB_GPU_REPORT_COUNT;
    int rc = ps3dk_prof_set_sink_path("/dev_hdd0/tmp");
    return rc ? rc : ps3dk_prof_init_ex(&cfg, &ext);
}

static int s_init(void *c)
{
    (void)c;
    return sysSpuInitialize(1, 0);
}
static int s_import(void *c)
{
    (void)c;
    return sysSpuImageImport(&g_image, ob_spu_bin, 0);
}
static int s_register(void *c, uint32_t *image)
{
    (void)c;
    return ps3dk_prof_spu_image_register_elf(ob_spu_bin, (uint64_t)ob_spu_bin_size, image);
}
static int s_lane(void *c, uint32_t image, ob_lane *out)
{
    (void)c;
    ps3dk_prof_spu_lane l;
    int rc = ps3dk_prof_spu_lane_create("ob-spu", image, 1, &l);
    if (rc == 0) {
        out->handle = l.handle;
        out->ring_slots = l.ring_slots;
        out->ea_ring = l.ea_ring;
        out->ea_ctl = l.ea_ctl;
    }
    return rc;
}
static int s_group(void *c)
{
    (void)c;
    sysSpuThreadGroupAttribute ga = {0};
    ga.name = "ob-spu";
    ga.nsize = 7;
    return sysSpuThreadGroupCreate(&g_group, 1, 100, &ga);
}
static int s_thread(void *c, const ob_lane *l, uint64_t reply_ea)
{
    (void)c;
    sysSpuThreadAttribute ta = {0};
    ta.name = "ob-spu";
    ta.nsize = 7;
    sysSpuThreadArgument args = {0};
    args.arg0 = l->ea_ring;
    args.arg1 = l->ring_slots;
    args.arg2 = l->ea_ctl;
    args.arg3 = reply_ea;
    memset(&g_res, 0, sizeof g_res);
    __asm__ volatile("sync" ::: "memory");
    return sysSpuThreadInitialize(&g_thread, g_group, 0, &g_image, &ta, &args);
}
/* No sysSpuThreadGroupStart error is documented to prove that nothing started, so this binding never returns
 * OB_SPU_START_NOT_STARTED: every nonzero result is UNCERTAIN and settled by terminate + join (ob_spu_life.h). */
static int s_start(void *c)
{
    (void)c;
    int rc = sysSpuThreadGroupStart(g_group);
    return rc == OB_SPU_START_NOT_STARTED ? -1 : rc; /* never the proven-non-start value */
}
static int s_mb(void *c, uint32_t w)
{
    (void)c;
    return sysSpuThreadWriteMb(g_thread, w);
}
static uint32_t s_seq(void *c)
{
    (void)c;
    return *(volatile uint32_t *)&g_res.seq;
}
static void s_words(void *c, uint32_t out[16])
{
    (void)c;
    __asm__ volatile("lwsync" ::: "memory"); /* the seq read before the word reads (ob_spu_res.h) */
    for (unsigned k = 0; k < 16; ++k)
        out[k] = ((volatile uint32_t *)g_res.w)[k];
}
static void s_pause(void *c)
{
    (void)c;
    usleep(10);
}
static int s_terminate(void *c)
{
    (void)c;
    return sysSpuThreadGroupTerminate(g_group, 0);
}
/* LV2's join has no time bound (ob_spu_life.h residual) */
static int s_join(void *c)
{
    (void)c;
    u32 cause = 0, status = 0;
    return sysSpuThreadGroupJoin(g_group, &cause, &status);
}
/* ETIMEDOUT keeps the lane charged: called again, a bounded number of times */
static int s_lane_end(void *c, uint32_t handle)
{
    (void)c;
    int rc = 0;
    for (int k = 0; k < 3; ++k) {
        rc = ps3dk_prof_spu_lane_end(handle, OB_WAIT_US, NULL);
        if (rc != PS3DK_PROF_ETIMEDOUT)
            break;
    }
    return rc;
}
static int s_destroy(void *c)
{
    (void)c;
    return sysSpuThreadGroupDestroy(g_group);
}
static int s_image_close(void *c)
{
    (void)c;
    return sysSpuImageClose(&g_image);
}
static uint64_t s_tb(void *c)
{
    (void)c;
    return ob_tb();
}

static const ob_spu_port g_port = {NULL,    s_init,  s_import, s_register,  s_lane,    s_group,       s_thread,
                                   s_start, s_mb,    s_seq,    s_words,     s_pause,   s_terminate,   s_join,
                                   s_lane_end, s_destroy, s_image_close, s_tb, NULL};

const ob_spu_port *ob_spu_port_lv2(void) { return &g_port; }
uint64_t ob_spu_reply_ea(void) { return (uint64_t)(uintptr_t)&g_res; }
