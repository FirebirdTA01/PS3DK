#include "auto_ring_commands.h"

static const uint32_t registers[9]={0x184,0x188,0x30c,0x310,0x314,0x318,0x31c,0x320,0x324};
#define METHOD(sub,m,n) (((uint32_t)(n)<<18)|((uint32_t)(sub)<<13)|(m))

int auto_ring_shadow_scan(auto_ring_shadow *s,const uint32_t *p,size_t n)
{
    size_t at=0;
    if(s->invalid || (!p && n)) return 0;
    if(UINT64_MAX-s->words<n) {s->invalid=1;return 0;}
    s->words+=n;
    while(at<n) {
        uint32_t h=p[at++],count=(h>>18)&2047u,method=h&0x1ffcu,sub=(h>>13)&7u;
        const uint32_t allowed=0x40000000u|(2047u<<18)|(7u<<13)|0x1ffcu;
        if((h&~allowed) || (!count && h) || count>n-at ||
           (!(h&0x40000000u) && count && method+4u*(count-1u)>0x1ffcu)) {
            s->invalid=1;return 0;
        }
        if(sub==1 && count) {
            s->seen=1;
            for(uint32_t i=0;i<count;i++) {
                uint32_t m=method+((h&0x40000000u)?0:4u*i);
                if(!m) {s->invalid=1;return 0;} /* object binding unknown */
                for(uint32_t j=0;j<9;j++) if(m==registers[j]) {
                    s->value[j]=p[at+i];s->known|=1u<<j;break;
                }
            }
        }
        at+=count;
    }
    return 1;
}

size_t auto_ring_restore(const auto_ring_shadow *s,uint32_t *p,size_t capacity)
{
    size_t n=0;
    if(s->invalid || (s->known&~511u)) return SIZE_MAX;
    for(uint32_t i=0;i<9;i++) if(s->known&(1u<<i)) n+=2;
    if(n>capacity || (!p && n)) return SIZE_MAX;
    n=0;
    for(uint32_t i=0;i<9;i++) if(s->known&(1u<<i)) {
        p[n++]=METHOD(1,registers[i],1);p[n++]=s->value[i];
    }
    return n;
}

size_t auto_ring_copy_packet(uint32_t *p,uint32_t destination,uint32_t report,uint32_t bytes)
{
    if(!p || !bytes || bytes>126u*32u || (bytes&31u) || (destination&15u) || (report&1u) ||
       report>=2048 || bytes/16u>2048u-report || (uint64_t)destination+bytes>UINT64_C(0x100000000)) return 0;
    p[0]=METHOD(1,0x184,2);p[1]=0x66626660u;p[2]=0xfeed0001u;
    p[3]=METHOD(1,0x30c,8);p[4]=16u*report;p[5]=destination;
    p[6]=bytes;p[7]=bytes;p[8]=bytes;p[9]=1;p[10]=0x101;p[11]=0;
    return 12;
}

static void barrier(uint32_t *p,uint32_t label,uint32_t value)
{
    p[0]=METHOD(0,0x1d6c,1);p[1]=16u*label;
    p[2]=METHOD(0,0x1d70,1);
    p[3]=(value&0xff00ff00u)|((value>>16)&255u)|((value&255u)<<16);
    p[4]=METHOD(0,0x64,1);p[5]=16u*label;p[6]=METHOD(0,0x68,1);p[7]=value;
}
size_t auto_ring_flush_packet(uint32_t *p,size_t capacity,const auto_ring_shadow *s,uint32_t destination,
                              uint32_t report,uint32_t bytes,uint32_t label,uint32_t generation)
{
    uint32_t copy[12],restore[AUTO_RING_RESTORE_WORDS];
    if(!p || !s || !generation || generation>UINT32_MAX/2u-1u ||
       !(label==64 || (label>=69 && label<=254))) return 0;
    size_t n=auto_ring_restore(s,restore,AUTO_RING_RESTORE_WORDS);
    if(n==SIZE_MAX || capacity<28u+n || !auto_ring_copy_packet(copy,destination,report,bytes)) return 0;
    barrier(p,label,2u*generation-1u);
    for(unsigned i=0;i<12;i++)p[8+i]=copy[i];
    barrier(p+20,label,2u*generation);
    for(size_t i=0;i<n;i++)p[28+i]=restore[i];
    return 28u+n;
}

int auto_ring_tail_take(auto_ring_tail *t,uintptr_t begin,uintptr_t current,uintptr_t *end,
                        uint32_t credit,uint64_t requested)
{
    if(!t || !end || t->active || !begin || ((begin|current|*end)&3u) || current<begin ||
       current>*end || !credit || !requested || (uint64_t)credit+requested<requested ||
       (uint64_t)credit+requested>(uint64_t)(*end-current)/4u) return 0;
    t->begin=begin;t->physical=*end;t->visible=*end-(uintptr_t)credit*4u;t->active=1;
    *end=t->visible;
    return 1;
}

int auto_ring_tail_release(auto_ring_tail *t,uintptr_t begin,uintptr_t current,uintptr_t *end)
{
    if(!t || !end || !t->active || begin!=t->begin || *end!=t->visible ||
       (current&3u) || current<begin || current>t->visible) return 0;
    *end=t->physical;t->active=0;
    return 1;
}
