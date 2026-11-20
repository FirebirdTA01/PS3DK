/* Initial planner/bootstrap contracts, independently measured wire header.
 * Run each named case separately so RED never hides later obligations. */
#include "combined_encoder_config.h"
#include "roll_ppu.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(x) do { if(!(x)){fprintf(stderr,"FAIL combined-encoder:%d: %s\n",__LINE__,#x);return 1;} }while(0)
static uint32_t rd32(const uint8_t *b)
{return (uint32_t)b[0]|(uint32_t)b[1]<<8|(uint32_t)b[2]<<16|(uint32_t)b[3]<<24;}
static uint32_t crc(const uint8_t *p,size_t n)
{uint32_t c=UINT32_MAX;unsigned i;while(n--){c^=*p++;for(i=0;i<8;++i)c=(c>>1)^(UINT32_C(0xedb88320)&(0u-(c&1u)));}return ~c;}
static void put32(uint8_t *p,uint32_t v)
{unsigned i;for(i=0;i<4;++i)p[i]=(uint8_t)(v>>(8*i));}
static size_t bound(const pfm_config *c)
{return 148u+(c->metadata_bytes-16u)+128u*c->incarnations+80u*c->ppu_stack_rows+16u*c->frame_identities+32u*c->journal_entries+64u*c->event_runs+24u*c->saved_spu_witnesses;}
static int planner(const char *name)
{
    pfm_config c=test_config();pfm_layout l;
    if(!strcmp(name,"planner")){
        CHECK(pfm_plan(&c,&l)==PFR_OK);CHECK(l.context_bytes&&l.arena_bytes&&l.arena_bytes<=1048576);
        CHECK(l.arena_alignment&&l.arena_alignment<=_Alignof(max_align_t));CHECK(l.checkpoint_bound==bound(&c));
        c.checkpoint_bytes=(uint32_t)bound(&c);CHECK(pfm_plan(&c,&l)==PFR_OK);
    }else if(!strcmp(name,"undersized")){
        c.checkpoint_bytes=(uint32_t)bound(&c)-1;CHECK(pfm_plan(&c,&l)==PFR_INVALID);
    }else{
        c.metadata_bytes=15;CHECK(pfm_plan(&c,&l)==PFR_INVALID);
        c=test_config();c.saved_spu_witnesses=UINT32_MAX;CHECK(pfm_plan(&c,&l)==PFR_INVALID);
        c=test_config();c.incarnations=UINT32_MAX;CHECK(pfm_plan(&c,&l)==PFR_INVALID);
        c=test_config();c.prepare_work=0;CHECK(pfm_plan(&c,&l)==PFR_INVALID);
    }return 0;
}
static int initial(const char *name)
{
    pfm_config c=test_config();pfm_layout l;uint8_t h[16]={'P','S','3','D','K','P','R','F',1,0,0,0,1,0,0,0};
    uint8_t *ctx,*a,*cp;size_t n;unsigned k;int rc=0;
    const uint32_t good[]={1,3,7,513,515,519},bad[]={0,2,4,5,6,8,512,UINT32_MAX};
    CHECK(pfm_plan(&c,&l)==PFR_OK);
    ctx=malloc(l.context_bytes);a=malloc(l.arena_bytes);cp=malloc(c.checkpoint_bytes);CHECK(ctx&&a&&cp);
    if(!strcmp(name,"flags")){
        for(k=0;k<sizeof(bad)/sizeof(bad[0]);++k){put32(h+12,bad[k]);n=777;
            CHECK(pfm_init(ctx,l.context_bytes,&c,h,a,l.arena_bytes,cp,c.checkpoint_bytes,&n)==PFR_INVALID);CHECK(!n);}
    }else if(!strcmp(name,"init-short-cap")){
        c.checkpoint_bytes=(uint32_t)bound(&c)-1;
        CHECK(pfm_init(ctx,l.context_bytes,&c,h,a,l.arena_bytes,cp,65536,&n)==PFR_INVALID);CHECK(!n);
    }else{
        for(k=0;k<sizeof(good)/sizeof(good[0]);++k){unsigned s;put32(h+12,good[k]);
            CHECK(pfm_init(ctx,l.context_bytes,&c,h,a,l.arena_bytes,cp,c.checkpoint_bytes,&n)==PFR_OK);
            CHECK(n==148&&!memcmp(cp,"PFRMIX01",8));CHECK(cp[8]==1&&cp[9]==0&&cp[10]==80&&cp[11]==0);
            CHECK(rd32(cp+12)==4&&!memcmp(cp+16,h,16));CHECK(rd32(cp+48)==64&&rd32(cp+52)==0);
            CHECK(rd32(cp+56)==3&&rd32(cp+60)==0);CHECK(!memcmp(cp+64,"\1\0\1\0\1\0\1\0",8));
            for(s=32;s<48;++s)CHECK(cp[s]==0);
            for(s=72;s<80;++s)CHECK(cp[s]==0);
            for(s=0;s<4;++s){size_t at=80+16*s;CHECK(cp[at]==s+1&&cp[at+1]==0&&cp[at+2]==1&&cp[at+3]==0);
                CHECK(!rd32(cp+at+4)&&!rd32(cp+at+8)&&!rd32(cp+at+12));}
            CHECK(rd32(cp+144)==crc(cp,144));
        }
    }free(cp);free(a);free(ctx);return rc;
}
static int ppu_seam(void)
{
    pfp_config c={16,16,32,64,32768,65536};pfp_layout l;uint8_t h[16]={'P','S','3','D','K','P','R','F',1,0,0,0,0,0,0,0};
    const uint32_t unsupported[]={1,3,7,513,515,519};uint8_t *ctx,*a,*cp;unsigned k;size_t n;
    CHECK(pfp_plan(&c,&l)==PFR_OK);CHECK(l.checkpoint_bound==132u+32768u+120u*16u+80u*32u+16u*64u);
    ctx=malloc(l.context_bytes);a=malloc(l.arena_bytes);cp=malloc(c.checkpoint_bytes);CHECK(ctx&&a&&cp);
    for(k=0;k<2;++k){put32(h+12,k?512:0);CHECK(pfp_init(ctx,l.context_bytes,&c,h,a,l.arena_bytes,cp,c.checkpoint_bytes,&n)==PFR_OK);
        CHECK(n==132&&!memcmp(cp,"PFRCKP01",8)&&!memcmp(cp+80,h,16)&&rd32(cp+128)==crc(cp,128));}
    for(k=0;k<sizeof(unsupported)/sizeof(unsupported[0]);++k){put32(h+12,unsupported[k]);
        CHECK(pfp_init(ctx,l.context_bytes,&c,h,a,l.arena_bytes,cp,c.checkpoint_bytes,&n)==PFR_INVALID);CHECK(!n);}
    free(cp);free(a);free(ctx);return 0;
}
int main(int argc,char **argv)
{
    int rc;if(argc!=2)return 2;
    if(!strcmp(argv[1],"ppu-seam"))rc=ppu_seam();
    else if(!strcmp(argv[1],"planner")||!strcmp(argv[1],"undersized")||!strcmp(argv[1],"overflow"))rc=planner(argv[1]);
    else if(!strcmp(argv[1],"initial")||!strcmp(argv[1],"flags")||!strcmp(argv[1],"init-short-cap"))rc=initial(argv[1]);
    else return 2;
    if(!rc)printf("PASS %s\n",argv[1]);
    return rc;
}
