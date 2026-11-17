/* Attach partition and budget layout of the owner report ring. Pure checks:
 * every refusal leaves a zeroed plan; accepted plans are disjoint from clocks
 * and legacy pairs, end the report range exactly and fit their allocation. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "auto_ring_layout.h"

static const auto_ring_plan zero;

static void refused(uint32_t first, uint32_t count, uint32_t blocks, uint32_t bytes, uint32_t records)
{
    auto_ring_plan p;
    memset(&p,0x5a,sizeof p);
    assert(!auto_ring_layout(first,count,blocks,bytes,records,184,40,&p));
    assert(!memcmp(&p,&zero,sizeof p));
}

static void disjoint(const auto_ring_plan *p, uint32_t first, uint32_t count)
{
    uint32_t legacy_end=first+16u+2u*p->legacy_pairs;
    assert(p->pairs==126 && p->legacy_pairs>0);
    assert(p->first==legacy_end && !(p->first&1u));
    assert(p->first+2u*p->blocks*p->pairs==first+count);
    assert(p->legacy_pairs+p->blocks*p->pairs==(count-16u)/2u);
    assert(p->stride==4096 && p->pairs*32u<=p->stride && p->slots==p->bytes/p->stride);
    assert(!(p->bytes%(1u<<20)) && p->metadata_at==p->bytes && !(p->slots_at&127u));
    assert(p->slots_at>=p->metadata_at+p->records*p->metadata_stride);
    assert(p->total>=p->slots_at+p->slots*p->slot_stride && !(p->total%(1u<<20)));
    assert(p->total-(p->slots_at+p->slots*p->slot_stride)<(1u<<20));
}

int main(void)
{
    auto_ring_plan p;
    /* defaults: clocks 1024..1039, 252 legacy pairs, two blocks 1544..2047 */
    assert(auto_ring_layout(1024,1024,2,0,0,184,40,&p));
    disjoint(&p,1024,1024);
    assert(p.blocks==2 && p.first==1544 && p.legacy_pairs==252);
    assert(p.bytes==16u<<20 && p.slots==4096 && p.records==16384);
    /* 16,384 x 184 = 3,014,656 metadata bytes and 4,096 x 40 = 163,840
     * descriptor bytes after the 16 MiB ring: 20 MiB of whole pages */
    assert(p.slots_at==(16u<<20)+3014656u && p.total==20u<<20);
    /* 3 blocks leave 126 legacy pairs; 4 blocks leave none with 504 pairs */
    assert(auto_ring_layout(1024,1024,3,0,0,184,40,&p));
    disjoint(&p,1024,1024);
    assert(p.first==1292 && p.legacy_pairs==126);
    refused(1024,1024,4,0,0);
    /* a smaller pool: two blocks need at least 253 pairs */
    assert(auto_ring_layout(0,522,2,1u<<20,1,184,40,&p));
    disjoint(&p,0,522);
    assert(p.legacy_pairs==1 && p.first==18 && p.slots==256 && p.total==2u<<20);
    refused(0,520,2,0,0);
    /* explicit budgets, an exact single page and the bounds */
    assert(auto_ring_layout(1024,1024,2,256u<<20,65536,184,40,&p));
    disjoint(&p,1024,1024);
    assert(p.slots==65536 && p.records==65536);
    refused(1024,1024,2,(256u<<20)+(1u<<20),0);
    refused(1024,1024,2,(1u<<20)+4096u,0);
    refused(1024,1024,2,0,65537);
    refused(1024,1024,1,0,0);
    refused(1024,1024,5,0,0);
    /* odd ring reports cannot be copied */
    refused(1023,1024,2,0,0);
    refused(1024,1026,2,0,0);
    refused(1025,1024,2,0,0);
    refused(1024,1023,2,0,0);
    refused(1024,16,2,0,0);
    assert(!auto_ring_layout(1024,1024,2,0,0,184,40,NULL));
    puts("AUTO ring partition and budget layout PASS");
    return 0;
}
