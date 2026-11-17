#include "auto_ring_layout.h"
#include <string.h>

int auto_ring_layout(uint32_t report_first, uint32_t report_count, uint32_t blocks, uint32_t bytes,
                     uint32_t records, uint32_t metadata_stride, uint32_t slot_stride, auto_ring_plan *plan)
{
    uint64_t pool,ring,at,total;
    auto_ring_plan p;
    if(!plan) return 0;
    memset(plan,0,sizeof *plan);
    if(!bytes) bytes=AUTO_RING_DEFAULT_BYTES;
    if(!records) records=AUTO_RING_DEFAULT_RECORDS;
    if(blocks<2 || blocks>4 || report_count<18 || report_count>1024 || (report_count&1u) ||
       report_first>2048u-report_count || !metadata_stride || !slot_stride ||
       (bytes%AUTO_RING_PAGE) || bytes>AUTO_RING_BYTES_MAX || records>AUTO_RING_RECORDS_MAX) return 0;
    pool=(report_count-AUTO_RING_CLOCK_REPORTS)/2u;
    ring=(uint64_t)blocks*AUTO_RING_BLOCK_PAIRS;
    if(pool<=ring) return 0; /* the legacy pool keeps segment, CALL, range and FRAME pairs */
    memset(&p,0,sizeof p);
    p.blocks=blocks;p.pairs=AUTO_RING_BLOCK_PAIRS;p.legacy_pairs=(uint32_t)(pool-ring);
    p.first=report_first+report_count-(uint32_t)(2u*ring);
    if(p.first&1u) return 0; /* M2MF report source is 32-byte aligned */
    p.bytes=bytes;p.stride=AUTO_RING_STRIDE;p.slots=bytes/AUTO_RING_STRIDE;
    p.records=records;p.metadata_stride=metadata_stride;p.slot_stride=slot_stride;
    at=bytes;p.metadata_at=(uint32_t)at;
    at+=(uint64_t)records*metadata_stride;
    at=(at+127u)&~UINT64_C(127);
    if(at>UINT32_MAX) return 0;
    p.slots_at=(uint32_t)at;
    at+=(uint64_t)p.slots*slot_stride;
    total=(at+AUTO_RING_PAGE-1u)/AUTO_RING_PAGE*AUTO_RING_PAGE;
    if(total>UINT32_MAX) return 0;
    p.total=(uint32_t)total;
    *plan=p;
    return 1;
}
