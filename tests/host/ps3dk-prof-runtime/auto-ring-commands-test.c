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
/* The first header the decoder rejects is named: the word, its byte offset from the context begin (the caller's
 * origin plus its index) and the class. Accepted streams name nothing; later rejections never overwrite it. */
static void rejection_named(void) {
    static const struct { uint32_t w[4]; size_t n; uint32_t index,reason; } c[]={
        {{0,0x20001000u},2,1,AUTO_RING_REJECT_JUMP},
        {{0x00001001u},1,0,AUTO_RING_REJECT_JUMP},
        {{0x1002},1,0,AUTO_RING_REJECT_CALL},
        {{0x00020000u},1,0,AUTO_RING_REJECT_RETURN},
        {{0x80000000u|M(1,0x184,1),0},2,0,AUTO_RING_REJECT_RESERVED},
        {{0x40000000u|0x184u},1,0,AUTO_RING_REJECT_EMPTY},
        {{M(1,0x184,2),0xfeed0000},2,0,AUTO_RING_REJECT_TRUNCATED},
        {{M(0,0x1ffc,2),0,0},3,0,AUTO_RING_REJECT_RANGE},
        {{M(6,0x184,1),0,M(1,0,1),123},4,2,AUTO_RING_REJECT_BINDING},
    };
    for(unsigned i=0;i<sizeof c/sizeof c[0];i++) {
        auto_ring_shadow s={0};
        uint32_t ok[]={M(1,0x184,2),0xfeed0000,0xfeed0001};
        s.origin=0x40;assert(auto_ring_shadow_scan(&s,ok,3) && !s.bad_reason && !s.bad_header && !s.bad_at);
        s.origin=0x100;
        assert(!auto_ring_shadow_scan(&s,c[i].w,c[i].n) && s.invalid);
        assert(s.bad_reason==c[i].reason && s.bad_header==c[i].w[c[i].index] && s.bad_at==0x100+4*c[i].index);
        s.origin=0x200;assert(!auto_ring_shadow_scan(&s,ok,3));
        auto_ring_shadow_reject(&s,0,8,AUTO_RING_REJECT_CURSOR);
        assert(s.bad_reason==c[i].reason && s.bad_at==0x100+4*c[i].index); /* the first one stays */
    }
    auto_ring_shadow s={0};
    auto_ring_shadow_reject(&s,0,0x80,AUTO_RING_REJECT_CURSOR);
    assert(s.invalid && s.bad_reason==AUTO_RING_REJECT_CURSOR && s.bad_at==0x80 && !s.bad_header);
}
/* ---- the CALL into the reserved prefix of the cellGcmInit host buffer (IO 0 up to the context start) ----
 * A synthetic default-state routine of the firmware's layout, not its words: other subchannels' objects and DMA
 * contexts, the M2MF object on subchannel 1 with its notify, IN and OUT contexts, RET; zero up to the context start.
 * host[PRE] is the first word of the command buffer (the context begin): a routine never reaches it. */
#define RET 0x00020000u
#define CALL(target) ((uint32_t)(target)|2u)
#define M2MF 0x31337303u
enum { PRE=64 };
static uint32_t host[PRE+8];
/* the init record: the EA of IO offset 0 and the context start's IO offset */
static void record(auto_ring_shadow *s) {s->io=host;s->start=4u*PRE;}
static size_t routine_at(uint32_t word,uint32_t in,uint32_t out)
{
    const uint32_t r[]={M(0,0,1),0x31337000u,M(0,0x180,2),0x66604200u,0xfeed0000u,M(7,0,1),0xcafebabeu,
                        M(1,0,1),M2MF,M(1,0x180,3),0x66604200u,in,out,
                        M(3,0,1),0x313371c3u,M(3,0x180,2),0x66604200u,0xfeed0000u,RET};
    assert(word+sizeof r/4u<=PRE);
    memcpy(host+word,r,sizeof r);
    return sizeof r/4u;
}
/* expect the first rejection: reason, header word and byte offset from the context begin */
static void refused(auto_ring_shadow *s,const uint32_t *w,size_t n,uint32_t reason,uint32_t header,uint32_t at)
{
    uint32_t restore[AUTO_RING_RESTORE_WORDS];
    assert(!auto_ring_shadow_scan(s,w,n) && s->invalid);
    printf("  reason %u header %08x at %#x\n",s->bad_reason,s->bad_header,s->bad_at);
    assert(s->bad_reason==reason && s->bad_header==header && s->bad_at==at);
    assert(auto_ring_restore(s,restore,AUTO_RING_RESTORE_WORDS)==SIZE_MAX);
}
/* The flip's CALL to IO 0: with the init record the routine is decoded through the packet decoder (its subchannel-1
 * bind and IN/OUT update the shadow), then decoding resumes after the CALL. Without the record it is refused as
 * before. Decoded again on every CALL: a routine changed between two flips is what the shadow then holds. */
static void flip_call_restores_routine(void)
{
    uint32_t restore[AUTO_RING_RESTORE_WORDS],flush[AUTO_RING_FLUSH_WORDS];
    memset(host,0,sizeof host);
    size_t n=routine_at(0,0xfeed0001u,0xfeed0000u);
    const uint32_t owner[]={M(1,0x184,2),0x11110000u,0x22220000u,M(0,0x64,1),0x30,CALL(0),
                            M(0,0x64,1),0x10,M(1,0x30c,2),8,96};
    auto_ring_shadow none={0};
    none.origin=0x7000;
    refused(&none,owner,11,AUTO_RING_REJECT_CALL,CALL(0),0x7000+4*5);   /* no init record: as before */
    auto_ring_shadow s={0};
    record(&s);s.origin=0x7000;
    assert(auto_ring_shadow_scan(&s,owner,11) && !s.invalid && !s.bad_reason);
    assert(s.value[0]==0xfeed0001u && s.value[1]==0xfeed0000u && s.value[2]==8 && s.value[3]==96 && s.known==15);
    assert(s.words==11+n);   /* the routine's words count as decoder cost */
    assert(auto_ring_restore(&s,restore,AUTO_RING_RESTORE_WORDS)==8);
    assert(restore[0]==M(1,0x184,1) && restore[1]==0xfeed0001u && restore[2]==M(1,0x188,1) && restore[3]==0xfeed0000u);
    assert(restore[4]==M(1,0x30c,1) && restore[5]==8 && restore[6]==M(1,0x310,1) && restore[7]==96);
    size_t nf=auto_ring_flush_packet(flush,AUTO_RING_FLUSH_WORDS,&s,256,4,32,245,3);
    assert(nf==36 && flush[28]==M(1,0x184,1) && flush[29]==0xfeed0001u && flush[31]==0xfeed0000u);
    /* the application sets its own IN/OUT; the next flip's routine was changed: decoded again, never cached */
    const uint32_t mode[]={M(1,0x184,2),0x11110000u,0x22220000u},flip[]={M(0,0x64,1),0x30,CALL(0)};
    assert(auto_ring_shadow_scan(&s,mode,3) && s.value[0]==0x11110000u);
    (void)routine_at(0,0xaaaa0001u,0xbbbb0000u);
    assert(auto_ring_shadow_scan(&s,flip,3) && s.value[0]==0xaaaa0001u && s.value[1]==0xbbbb0000u);
    /* a routine at another offset below the start, its RET the last word before the context start */
    memset(host,0,sizeof host);
    host[PRE]=M(1,0x184,1);   /* the command buffer's first word is never read */
    uint32_t last=PRE-(uint32_t)n;
    (void)routine_at(last,0xcccc0001u,0xdddd0000u);
    const uint32_t late[]={CALL(4u*last)};
    assert(auto_ring_shadow_scan(&s,late,1) && s.value[0]==0xcccc0001u && s.value[1]==0xdddd0000u && !s.invalid);
}
/* A CALL at or past the context start is an application segment: refused (no record of its words) */
static void call_into_context(void)
{
    const uint32_t at_start[]={M(1,0x184,2),0x11110000u,0x22220000u,CALL(4u*PRE)},past[]={CALL(4u*PRE+0x100u)};
    memset(host,0,sizeof host);(void)routine_at(0,0xfeed0001u,0xfeed0000u);
    auto_ring_shadow s={0};record(&s);s.origin=0x40;
    refused(&s,at_start,4,AUTO_RING_REJECT_CALL_TARGET,CALL(4u*PRE),0x40+4*3);
    auto_ring_shadow t={0};record(&t);t.origin=0x80;
    refused(&t,past,1,AUTO_RING_REJECT_CALL_TARGET,CALL(4u*PRE+0x100u),0x80);
}
/* One CALL level: a CALL or a JUMP inside the routine is refused; RET ends the routine only (at top level it is
 * refused as before). */
static void routine_flow(void)
{
    const uint32_t flip[]={M(0,0x64,1),0x30,CALL(0)},ret[]={RET};
    /* routine 2 at word 32 is itself complete: only the depth refuses it */
    memset(host,0,sizeof host);(void)routine_at(32,0xfeed0001u,0xfeed0000u);
    host[0]=M(1,0x184,1);host[1]=0xfeed0001u;host[2]=CALL(4*32);host[3]=RET;
    auto_ring_shadow s={0};record(&s);s.origin=0x200;
    refused(&s,flip,3,AUTO_RING_REJECT_ROUTINE_CALL,CALL(4*32),0x200+8);
    static const uint32_t jumps[]={0x20000000u|0x80u,0x81u};
    for(unsigned i=0;i<2;i++) {
        host[2]=jumps[i];
        auto_ring_shadow j={0};record(&j);j.origin=0x200;
        refused(&j,flip,3,AUTO_RING_REJECT_ROUTINE_JUMP,jumps[i],0x200+8);
    }
    /* RET inside the routine ends it and decoding resumes; RET in the owner words is refused */
    host[2]=RET;
    auto_ring_shadow r={0};record(&r);r.origin=0x300;
    assert(auto_ring_shadow_scan(&r,flip,3) && r.value[0]==0xfeed0001u);
    r.origin=0x300+12;
    refused(&r,ret,1,AUTO_RING_REJECT_RETURN,RET,0x300+12);
}
/* At most (start - target)/4 words: no RET within them refuses, even if one follows at the context begin */
static void routine_bound(void)
{
    const uint32_t flip[]={CALL(0)},late[]={CALL(4u*(PRE-2))};
    memset(host,0,sizeof host);
    host[PRE]=RET;host[PRE+1]=RET;
    auto_ring_shadow s={0};record(&s);s.origin=0x10;
    refused(&s,flip,1,AUTO_RING_REJECT_ROUTINE_UNTERMINATED,CALL(0),0x10);
    host[0]=M(1,0x184,2);host[1]=0xfeed0001u;host[2]=0xfeed0000u;   /* a valid packet first, still no RET */
    auto_ring_shadow t={0};record(&t);t.origin=0x10;
    refused(&t,flip,1,AUTO_RING_REJECT_ROUTINE_UNTERMINATED,CALL(0),0x10);
    /* a packet crossing the bound: its data would be the context begin's words */
    host[PRE-2]=M(1,0x184,2);host[PRE-1]=0xfeed0001u;
    auto_ring_shadow u={0};record(&u);u.origin=0x20;
    refused(&u,late,1,AUTO_RING_REJECT_ROUTINE_BOUND,M(1,0x184,2),0x20);
    host[PRE-2]=0;host[PRE-1]=M(1,0x184,1);
    auto_ring_shadow v={0};record(&v);v.origin=0x20;
    refused(&v,late,1,AUTO_RING_REJECT_ROUTINE_BOUND,M(1,0x184,1),0x20);
}
/* Inside the routine subchannel 1 may bind the M2MF object only */
static void routine_binding(void)
{
    const uint32_t flip[]={CALL(0)};
    memset(host,0,sizeof host);(void)routine_at(0,0xfeed0001u,0xfeed0000u);
    host[8]=0x31337000u;   /* the routine's subchannel-1 bind names another object */
    auto_ring_shadow s={0};record(&s);s.origin=0x400;
    refused(&s,flip,1,AUTO_RING_REJECT_ROUTINE_BINDING,M(1,0,1),0x400);
    /* a non-increment bind: the second data word names another object */
    memset(host,0,sizeof host);
    host[0]=0x40000000u|M(1,0,2);host[1]=M2MF;host[2]=0x3137af00u;host[3]=RET;
    auto_ring_shadow t={0};record(&t);t.origin=0x400;
    refused(&t,flip,1,AUTO_RING_REJECT_ROUTINE_BINDING,0x40000000u|M(1,0,2),0x400);
    host[2]=M2MF;
    auto_ring_shadow u={0};record(&u);u.origin=0x400;
    assert(auto_ring_shadow_scan(&u,flip,1) && !u.invalid && !u.known);
}
int main(int argc,char **argv)
{
    static const struct { const char *name; void (*run)(void); } tests[]={
        {"split",split_transfer_bytes},{"decoder",decoder_and_credit},{"named",rejection_named},
        {"flip",flip_call_restores_routine},{"context",call_into_context},{"flow",routine_flow},
        {"bound",routine_bound},{"binding",routine_binding},
    };
    for(unsigned i=0;i<sizeof tests/sizeof tests[0];i++)
        if(argc<2 || !strcmp(argv[1],tests[i].name)) {printf("%s\n",tests[i].name);tests[i].run();}
    puts("auto-ring command state and physical credit PASS");return 0;
}
