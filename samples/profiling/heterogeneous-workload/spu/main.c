#include "../source/spu_pool.h"
#include <spu_intrinsics.h>
#include <spu_mfcio.h>
#include <sys/spu_thread.h>
static work_slot local;
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
