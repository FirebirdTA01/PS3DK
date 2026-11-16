#include "auto_ring_commands.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

#define M(sub,method,n) (((n)<<18)|((sub)<<13)|(method))

/* Small independent M2MF executor: catches real destination bytes, including
 * state split across packets. Other engines are intentionally not simulated. */
typedef struct gpu { uint32_t reg[0x400/4]; unsigned char local[512],main[512],report[512]; } gpu;
static unsigned char *bank(gpu *g,uint32_t handle) {
    if(handle==0xfeed0000u)return g->local;
    if(handle==0xfeed0001u)return g->main;
    assert(handle==0x66626660u);return g->report;
}
static void execute(gpu *g,const uint32_t *p,size_t words) {
    size_t at=0;
    while(at<words) {
        uint32_t h=p[at++],n=(h>>18)&2047u,method=h&0x1ffcu,sub=(h>>13)&7u;
        assert(n<=words-at);
        for(uint32_t i=0;i<n;i++) {
            uint32_t m=method+((h&0x40000000u)?0:4*i),v=p[at+i];
            if(sub!=1)continue;
            assert(m<0x400);g->reg[m/4]=v;
            if(m==0x328) {
                uint32_t src=g->reg[0x30c/4],dst=g->reg[0x310/4];
                uint32_t len=g->reg[0x31c/4],rows=g->reg[0x320/4];
                assert(g->reg[0x324/4]==0x101);
                for(uint32_t row=0;row<rows;row++) {
                    assert(src+len<=512 && dst+len<=512);
                    memcpy(bank(g,g->reg[0x188/4])+dst,bank(g,g->reg[0x184/4])+src,len);
                    src+=g->reg[0x314/4];dst+=g->reg[0x318/4];
                }
            }
        }
        at+=n;
    }
}
static void split_transfer_bytes(void) {
    auto_ring_shadow s={0};gpu g={0};uint32_t restore[AUTO_RING_RESTORE_WORDS],copy[12],flush[AUTO_RING_FLUSH_WORDS];
    uint32_t mode[]={M(1,0x184,2),0xfeed0000,0xfeed0001};
    uint32_t format[]={M(1,0x314,5),16,32,8,2,0x101};
    uint32_t offset[]={M(1,0x30c,2),8,96,M(1,0x328,1),0};
    memset(g.local,0x37,sizeof g.local);memset(g.report,0xb4,sizeof g.report);
    execute(&g,mode,3);execute(&g,format,6);
    assert(auto_ring_shadow_scan(&s,mode,3));assert(auto_ring_shadow_scan(&s,format,6));
    /* A covered draw closes the block between app Format and Offset. */
    assert(auto_ring_copy_packet(copy,256,4,32)==12);execute(&g,copy,12);
    assert(!memcmp(g.main+256,g.report+64,32));
    size_t n=auto_ring_restore(&s,restore,AUTO_RING_RESTORE_WORDS);
    assert(n && n<=AUTO_RING_RESTORE_WORDS);execute(&g,restore,n);
    execute(&g,offset,5);
    assert(!memcmp(g.main+96,g.local+8,8));assert(!memcmp(g.main+128,g.local+24,8));
    assert(g.main[104]==0 && g.main[136]==0); /* original pitches and lengths */
    size_t nf=auto_ring_flush_packet(flush,AUTO_RING_FLUSH_WORDS,&s,256,4,32,245,3);
    assert(nf==28+n && flush[0]==M(0,0x1d6c,1) && flush[1]==245*16);
    assert(flush[3]==0x00050000 && flush[6]==M(0,0x68,1) && flush[7]==5);
    assert(flush[8]==M(1,0x184,2) && flush[23]==0x00060000 && flush[27]==6);
    execute(&g,flush,nf);execute(&g,offset,5);
    assert(!memcmp(g.main+96,g.local+8,8) && !memcmp(g.main+128,g.local+24,8));
    memset(flush,0xa5,sizeof flush);
    assert(!auto_ring_flush_packet(flush,nf-1,&s,256,4,32,245,3) && flush[0]==0xa5a5a5a5);
    /* The ordinary complete SetTransferData shape on both sides of a copy. */
    uint32_t transfer[]={M(1,0x184,2),0xfeed0001,0xfeed0000,
        M(1,0x30c,8),96,200,0,0,8,1,0x101,0};
    execute(&g,transfer,12);assert(auto_ring_shadow_scan(&s,transfer,12));
    assert(!memcmp(g.local+200,g.main+96,8));
    execute(&g,copy,12);n=auto_ring_restore(&s,restore,AUTO_RING_RESTORE_WORDS);execute(&g,restore,n);
    execute(&g,offset,5); /* mode is MAIN -> LOCAL, format is now the complete transfer's */
    assert(!memcmp(g.local+96,g.main+8,8));
}
static void decoder_and_credit(void) {
    auto_ring_shadow s={0};uint32_t restore[AUTO_RING_RESTORE_WORDS];
    uint32_t other[]={M(6,0x184,1),0xfeed0000,M(0,0x1800,1),0x01000020};
    assert(auto_ring_shadow_scan(&s,other,4));assert(!s.seen && auto_ring_restore(&s,restore,18)==0);
    uint32_t raw[]={M(1,0x184,2)|0x40000000u,0xfeed0000,0xfeed0001};
    assert(auto_ring_shadow_scan(&s,raw,3));assert(s.value[0]==0xfeed0001 && s.known==1);
    assert(auto_ring_restore(&s,restore,1)==SIZE_MAX); /* no partial restore */
    assert(auto_ring_restore(&s,restore,18)==2);
    uint32_t bind[]={M(1,0,1),123};assert(!auto_ring_shadow_scan(&s,bind,2));
    assert(auto_ring_restore(&s,restore,18)==SIZE_MAX);
    memset(&s,0,sizeof s);uint32_t call[]={0x1002};assert(!auto_ring_shadow_scan(&s,call,1));
    memset(&s,0,sizeof s);uint32_t short_packet[]={M(1,0x184,2),0xfeed0000};
    assert(!auto_ring_shadow_scan(&s,short_packet,2));
    auto_ring_tail t={0};uintptr_t end=1024;
    assert(!auto_ring_tail_take(&t,128,1000,&end,46,2));assert(end==1024);
    assert(auto_ring_tail_take(&t,128,256,&end,46,2));assert(end==840);
    uintptr_t current=end; /* application consumed all visible words */
    assert(auto_ring_tail_release(&t,128,current,&end));assert(end==1024);
    current+=46*4;assert(current==end); /* reserved flush fits, callback never invoked */
    assert(auto_ring_tail_take(&t,128,256,&end,46,2));
    end-=4;assert(!auto_ring_tail_release(&t,128,256,&end));assert(end==836); /* foreign mutation stays intact */
}
int main(void) { split_transfer_bytes();decoder_and_credit();puts("auto-ring command state and physical credit PASS");return 0; }
