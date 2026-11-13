#include "auto_commands.h"

uint64_t auto_command_words(const ps3tc_auto_call *c)
{
    uint64_t count,batches,mis,odd,loops,rest;
    if(!c) return 0;
    if(c->kind==3) return (c->clear_mask&~UINT32_C(0xf3)) ? 0:6;
    count=c->count;
    if(c->mode<1 || c->mode>10 || !count || count>UINT32_C(0xffffff)) return 0;
    if(c->kind==1) {
        if((uint64_t)c->start+count>UINT64_C(0x1000000)) return 0;
        batches=(count-1)/256;
        return 10+(batches/2047)*2048+(batches%2047 ? 1+batches%2047:0);
    }
    if(c->kind!=2 || c->index_type>1 || c->location>1) return 0;
    if((c->offset&(c->index_type ? 1u:3u)) ||
       (uint64_t)c->offset+count*(c->index_type ? 2u:4u)>UINT64_C(0x100000000)) return 0;
    mis=((128u-(c->offset&127u))&127u)>>(c->index_type ? 1:2);
    odd=mis && mis<count;
    if(odd) count-=mis;
    loops=(count-1)/UINT32_C(0x7ff00);
    rest=count-loops*UINT32_C(0x7ff00);
    return 10+odd*2+loops*2048+(rest+255)/256;
}

int auto_command_room(uintptr_t begin, uintptr_t current, uintptr_t end, uint64_t words)
{
    if(!begin || ((begin|current|end)&3u) || current<begin || end<current ||
       !words || words>UINT32_MAX-12u) return 0;
    return words+12u <= (uint64_t)(end-current)/4u;
}
