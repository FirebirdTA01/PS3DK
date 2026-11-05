#include "../source/spu_pool.h"
#include <spu_intrinsics.h>
#include <spu_mfcio.h>
#include <sys/spu_thread.h>
static work_slot local;
#if !SAMPLE_PROF_EXT
int main(uint64_t address,uint64_t lane,uint64_t lanes,uint64_t unused)
{
    (void)unused;
    for(;;) {
        uint32_t sequence=spu_read_in_mbox();
        if(!sequence)break;
        local.begin_dec=spu_readch(SPU_RdDec);
        work_result r=work_partition((uint32_t)lane,(uint32_t)lanes,WORK_ITEMS);
        local.end_dec=spu_readch(SPU_RdDec);
        local.digest=r.digest;local.count=r.count;
        mfc_put(&local,address,128,0,0,0);
        mfc_write_tag_mask(1);mfc_read_tag_status_all();
        local.sequence=sequence;
        mfc_put(&local.sequence,address+128,16,0,0,0);
        mfc_write_tag_mask(1);mfc_read_tag_status_all();
    }
    sys_spu_thread_exit(0);
    return 0;
}
#else
/* SAMPLE_PROF_EXT: the job loop is worker_proto.c (host-tested); this file binds its port to the mailbox, the lane
 * writer and MFC.  The writer's DMA uses tag WRITER_TAG, never the sample's tag 0. */
#include "../source/prof_ext.h"
#include "../source/worker_proto.h"
#include "prof_spu_lane.h"
#define LS_CAP 256u
#define WRITER_TAG 3u
_Static_assert(WP_EAGAIN==PS3DK_PROF_SPU_EAGAIN,"the port's EAGAIN is the writer's");
_Static_assert(WP_OK==PS3DK_PROF_SPU_OK,"the port's OK is the writer's");
static ps3dk_prof_spu_writer g_w __attribute__((aligned(PS3DK_PROF_SPU_WRITER_ALIGN)));
static uint8_t g_stage[PS3DK_PROF_SPU_STAGING_BYTES(LS_CAP)] __attribute__((aligned(16)));
static uint64_t g_address;
static pe_unpacked g_u;
static uint32_t w_read_mbox(void *c){(void)c;return spu_read_in_mbox();}
static int w_attach(void *c){(void)c;return ps3dk_prof_spu_attach(&g_w,g_u.ring_ea,g_u.ring_slots,g_u.ctl_ea,WRITER_TAG,g_stage,LS_CAP,PS3DK_PROF_SPU_OWN_DEC);}
static int w_flush(void *c){(void)c;return ps3dk_prof_spu_flush(&g_w);}
static int w_dec_write(void *c){(void)c;return ps3dk_prof_spu_dec_write(&g_w,0xFFFFFFFFu);}
static int w_fini(void *c){(void)c;return ps3dk_prof_spu_fini(&g_w);}
static uint32_t w_zone_begin(void *c){(void)c;return PS3DK_PROF_SPU_ZONE_BEGIN(&g_w,"spu.work");}
static void w_zone_end(void *c,uint32_t z){(void)c;PS3DK_PROF_SPU_ZONE_END(&g_w,z);}
static void w_work(void *c)
{
    (void)c;
    local.begin_dec=spu_readch(SPU_RdDec);
    work_result r=work_partition(g_u.lane,g_u.lanes,WORK_ITEMS);
    local.end_dec=spu_readch(SPU_RdDec);
    local.digest=r.digest;local.count=r.count;
}
static void w_no_work(void *c){(void)c;local.digest=0;local.count=0;}
static void w_publish(void *c,uint32_t status,uint32_t sequence)
{
    (void)c;
    local.reserved[0]=status;
    mfc_put(&local,g_address,128,0,0,0);
    mfc_write_tag_mask(1);mfc_read_tag_status_all();
    local.sequence=sequence;
    mfc_put(&local.sequence,g_address+128,16,0,0,0);
    mfc_write_tag_mask(1);mfc_read_tag_status_all();
}
int main(uint64_t address,uint64_t lanes_arg,uint64_t ring,uint64_t slots_ctl)
{
    static const worker_io io={0,w_read_mbox,w_attach,w_flush,w_dec_write,w_fini,w_zone_begin,w_zone_end,w_work,w_no_work,w_publish};
    pe_args a={address,lanes_arg,ring,slots_ctl};
    g_u=pe_unpack_ext(a);
    g_address=address;
    worker_run(&io,g_u.ring_ea!=0);
    sys_spu_thread_exit(0);
    return 0;
}
#endif
