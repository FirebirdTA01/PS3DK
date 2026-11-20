/* Portable predicate coverage; compile/run separately when authorized.
 * This tests the actual PPU retirement predicate, not firmware completion. */
#include <assert.h>
#include "../common/protocol.h"
int main(void)
{
    sj_receipt r={0};
    r.magic=SJ_MAGIC; r.version=SJ_VERSION; r.job=2; r.lane=7;
    r.cookie=123; r.ring=4096; r.ctl=8192; r.post_fini=1;
#define CAN() sj_can_retire(1,&r,2,7,123,4096,8192)
    assert(CAN());
    r.fini_rc=-21; assert(!CAN()); r.fini_rc=0;
    r.magic=0; assert(!CAN()); r.magic=SJ_MAGIC;
    r.post_fini=0; assert(!CAN()); r.post_fini=1;
    r.lane=8; assert(!CAN()); r.lane=7;
    r.cookie=124; assert(!CAN()); r.cookie=123;
    r.ring=12288; assert(!CAN()); r.ring=4096;
    r.ctl=12288; assert(!CAN()); r.ctl=8192;
    r.job=3; assert(!CAN()); r.job=2;
    r.attach_rc=-2; assert(!CAN()); r.attach_rc=0;
    assert(!sj_can_retire(0,&r,2,7,123,4096,8192));
    assert(!sj_can_retire(1,0,2,7,123,4096,8192));
    assert(!sj_can_retire(1,&r,2,0,123,4096,8192));
    assert(CAN());
    /* Reuse a previously successful result object: stale post_fini/magic must
     * not authorize this invocation before its own attach/fini/receipt. */
    sj_receipt_reset(&r);
    assert(!CAN());
    assert(r.magic==0 && r.post_fini==0 && r.status==0);
    assert(r.attach_rc==SJ_NOT_CALLED && r.flush_rc==SJ_NOT_CALLED && r.fini_rc==SJ_NOT_CALLED);
    r.magic=SJ_MAGIC; r.version=SJ_VERSION; r.job=2; r.lane=7;
    r.cookie=123; r.ring=4096; r.ctl=8192;
    assert(!CAN()); /* matching identity is insufficient after reset */
    r.attach_rc=0; r.fini_rc=0; r.post_fini=1;
    assert(CAN());
    assert(sj_checksum(0,SJ_ITERATIONS)==4232289247u);
    assert(sj_checksum(5,SJ_ITERATIONS)==2655427546u);
    return 0;
}
