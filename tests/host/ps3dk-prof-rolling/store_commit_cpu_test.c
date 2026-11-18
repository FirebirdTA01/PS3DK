/* Compile against an instrumented private implementation. Work bounds are
 * deterministic; timings are deliberately not a test assertion. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static size_t cleared_bytes, crc_bytes;
static void *counted_memset(void *p, int c, size_t n)
{ cleared_bytes += n; return memset(p, c, n); }
#define memset counted_memset
#include "roll_store.c"
#undef memset
#define CHECK(x) do { if (!(x)) { fprintf(stderr,"FAIL line %d: %s\n",__LINE__,#x); exit(1); } } while (0)

typedef struct { uint8_t *p; uint64_t size; unsigned syncs; } disk;
static int64_t rd(void *v,uint64_t o,void *p,size_t n)
{ disk *d=v; CHECK(o<=d->size && n<=d->size-o); memcpy(p,d->p+(size_t)o,n); return (int64_t)n; }
static int64_t wr(void *v,uint64_t o,const void *p,size_t n)
{ disk *d=v; CHECK(o<=d->size && n<=d->size-o); memcpy(d->p+(size_t)o,p,n); return (int64_t)n; }
static int sz(void *v,uint64_t *n) { *n=((disk *)v)->size; return 0; }
static int resize(void *v,uint64_t n) { ((disk *)v)->size=n; return 0; }
static int sync_disk(void *v) { ++((disk *)v)->syncs; return 0; }
static uint32_t slow_crc(const uint8_t *p,size_t n)
{
    uint32_t c=UINT32_MAX; unsigned j;
    while(n--) { c^=*p++; for(j=0;j<8;++j) c=(c>>1)^(UINT32_C(0xedb88320)&(0u-(c&1u))); }
    return ~c;
}
static void put32(uint8_t *p,uint32_t n)
{ unsigned i; for(i=0;i<4;++i) p[i]=(uint8_t)(n>>(8*i)); }
static void put64(uint8_t *p,uint64_t n)
{ unsigned i; for(i=0;i<8;++i) p[i]=(uint8_t)(n>>(8*i)); }
/* Full materialized wire encoder, no production encoding or CRC helpers. */
static void encode_expected_slot(uint8_t *p,size_t sn,const pfr_config *c,uint64_t segment,
                   uint64_t first,uint64_t next,const uint8_t *cp,size_t cn,const uint8_t *data,size_t pn)
{
    uint8_t *f=p+sn-64; uint32_t cc=slow_crc(cp,cn),pc=slow_crc(data,pn);
    memset(p,0,sn); memcpy(p,"PFRSLOT1",8); memcpy(p+8,c->session,16);
    put64(p+24,segment); put64(p+32,segment+2); put64(p+40,first); put64(p+48,next);
    put32(p+56,(uint32_t)cn); put32(p+60,(uint32_t)pn); put32(p+64,cc); put32(p+68,pc);
    put32(p+80,slow_crc(p,80)); memcpy(p+96,cp,cn); memcpy(p+96+cn,data,pn);
    memcpy(f,"PFRCMIT1",8); put64(f+8,segment); put64(f+16,segment+2);
    put64(f+24,first); put64(f+32,next); put32(f+40,cc); put32(f+44,pc);
    put32(f+48,slow_crc(p,sn-64)); put32(f+60,slow_crc(f,60));
}
int main(int argc,char **argv)
{
    unsigned version,i,violations=0; uint64_t total_clear=0,total_crc=0;
    int allow_full=argc==2 && !strcmp(argv[1],"--allow-full");
    for(version=1;version<=2;++version) {
        pfr_config c={0}; pfr_layout l; pfr_store *s; disk d={0}; pfr_io io;
        uint8_t *scratch,*expected,*cp,*data; uint64_t seq=0; uint32_t old[3]={0};
        c.max_bytes=UINT64_C(32)*1024*1024; c.retained_slots=2;
        c.checkpoint_bytes=2u*1024*1024; c.payload_bytes=1024*1024; c.session[0]=0xa7;
        CHECK(pfr_plan(&c,&l)==0);
        s=malloc(pfr_state_bytes()); scratch=malloc(l.scratch_bytes); expected=malloc(l.slot_bytes);
        cp=malloc(c.checkpoint_bytes); data=malloc(c.payload_bytes); d.p=malloc((size_t)l.file_bytes);
        CHECK(s&&scratch&&expected&&cp&&data&&d.p);
        memset(d.p,0xad,(size_t)l.file_bytes); memset(scratch,0xcd,l.scratch_bytes);
        memset(cp,0xb9,c.checkpoint_bytes); memset(data,0x79,c.payload_bytes);
        io=(pfr_io){&d,rd,wr,resize,sz,sync_disk};
        CHECK((version==1?pfr_format:pfr_format_v2)(s,&io,&c,scratch,l.scratch_bytes)==0);
        for(i=0;i<24;++i) {
            const size_t sizes[]={0,1,4095,4096,8193,1048576};
            size_t cn=sizes[i%6],pn=i%4==0?0:i%4==1?20:i%4==2?257:c.payload_bytes;
            size_t used=96+cn+pn,prefix; unsigned syncs=d.syncs; uint32_t physical=i%3;
            if(i==12) {
                /* A failed long write into the spare need not appear in roots. */
                memset(d.p+12288+(size_t)physical*l.slot_bytes+1500000,0xfe,512);
                memset(scratch,0x37,l.scratch_bytes);
                CHECK(pfr_open(s,&io,&c,scratch,l.scratch_bytes)==0);
                old[0]=old[1]=old[2]=l.slot_bytes-64;
            }
            if(i && i%3==0) { pfr_segment seg; CHECK(pfr_get_segment(s,0,&seg)==0); }
            /* Scratch ownership grants no promise that its tail stays zero. */
            memset(scratch,0xa5,l.scratch_bytes);
            if(pn) { put32(data,43); put32(data+4,(uint32_t)pn-20); put64(data+8,seq); put32(data+pn-4,slow_crc(data,pn-4)); }
            prefix=old[physical]>used?old[physical]:used;
            cleared_bytes=crc_bytes=0;
            CHECK(pfr_commit(s,cp,cn,data,pn)==0);
            total_clear+=cleared_bytes; total_crc+=crc_bytes;
            if(cleared_bytes>96+64+4096+prefix-used || crc_bytes>3*used+4096+256) ++violations;
            CHECK(d.syncs==syncs+2);
            encode_expected_slot(expected,l.slot_bytes,&c,i,seq,seq+(pn!=0),cp,cn,data,pn);
            CHECK(!memcmp(expected,d.p+12288+(size_t)physical*l.slot_bytes,l.slot_bytes));
            if(pn) ++seq;
            old[physical]=(uint32_t)used;
        }
        CHECK(pfr_open(s,&io,&c,scratch,l.scratch_bytes)==0);
        free(d.p); free(data); free(cp); free(expected); free(scratch); free(s);
    }
    printf("wire oracle PASS: 48 commits, dirty scratch, reopen; clear=%llu crc=%llu work_violations=%u\n",
           (unsigned long long)total_clear,(unsigned long long)total_crc,violations);
    if(!allow_full) CHECK(!violations);
    return 0;
}
