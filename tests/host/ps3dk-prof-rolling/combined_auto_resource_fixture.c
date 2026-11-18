/* Test-only source transaction probe. Resource refusal and successful retry
 * both start from the same accepted state; scratch contents are independent.
 * No private encoder layout, checkpoint parser or expected bytes enter C. */
#include "combined_auto_resource_config.h"
#define COMBINED2_ENCODER_TEST_CONFIG_H
static pfm2_config test_config(void){pfm2_config c=resource_config();resource_override(&c);return c;}
#define main unused_general_auto_encoder_main
#include "combined2_encoder_fixture.c"
#undef main
#define REQUIRE(x,label) do{if(!(x)){fprintf(stderr,"AUTO_RESOURCE[%s]: %s\n",label,#x);exit(1);}}while(0)
static int exact_plan_bound(pfm2_config cfg)
{
    pfm2_layout generous,exact,refused,sentinel;
    REQUIRE(pfm2_plan(&cfg,&generous)==PFR_OK&&generous.checkpoint_bound>195
        &&generous.checkpoint_bound<=UINT32_MAX,"generous-plan-bound");
    cfg.shared.checkpoint_bytes=(uint32_t)generous.checkpoint_bound;
    REQUIRE(pfm2_plan(&cfg,&exact)==PFR_OK&&!memcmp(&generous,&exact,sizeof exact),"exact-conservative-K-plan");
    --cfg.shared.checkpoint_bytes;memset(&refused,0xa5,sizeof refused);sentinel=refused;
    REQUIRE(pfm2_plan(&cfg,&refused)==PFR_INVALID&&!memcmp(&refused,&sentinel,sizeof refused),"conservative-K-minus-one-no-layout");
    printf("PASS conservative_bound=%zu exact=1 minus_one_refused=1 layout_unchanged=1\n",generous.checkpoint_bound);
    return 0;
}
static uint8_t *read_source(const char *path,size_t *n)
{
    FILE *f=fopen(path,"rb");long size;uint8_t *p;REQUIRE(f,"source-open");
    REQUIRE(!fseek(f,0,SEEK_END)&&(size=ftell(f))>=16&&size<=INPUT_CAP&&!fseek(f,0,SEEK_SET),"bounded-source");
    p=malloc((size_t)size);REQUIRE(p,"source-buffer");REQUIRE(fread(p,1,(size_t)size,f)==(size_t)size&&fgetc(f)==EOF&&!ferror(f),"source-read");
    REQUIRE(!fclose(f),"source-close");*n=(size_t)size;return p;
}
int main(int argc,char **argv)
{
    pfm2_config cfg=test_config();pfm2_layout l;pfo_prepared x,y;uint8_t *source,*retry=NULL,*retry_old=NULL,*ctx,*ctx_old,*a[3],*old,*cp[2],*copy;
    size_t n,rn,at=16,initial=0,last_bytes=0;unsigned active=0,cut=0,i;int rc=0,other,stopped=0;
    if(argc==2&&!strcmp(argv[1],"--plan"))return exact_plan_bound(cfg);
    if(argc!=4){fputs("usage: combined-auto-resource-fixture SOURCE RETRY_ORIGINAL_PREFIX OUTPUT_DIR | --plan\n",stderr);return 2;}
    source=read_source(argv[1],&n);rn=0;
    if(strcmp(argv[2],"-")){retry=read_source(argv[2],&rn);retry_old=malloc(rn);REQUIRE(retry_old,"retry-input-copy");memcpy(retry_old,retry,rn);}
    rc=pfm2_plan(&cfg,&l);if(rc!=PFR_OK){fprintf(stderr,"reject stage=plan rc=%d\n",rc);free(source);free(retry);free(retry_old);return 1;}
    REQUIRE(l.context_bytes<=STATE_CAP&&l.arena_bytes<=STATE_CAP,"state-cap");
    ctx=malloc(l.context_bytes);ctx_old=malloc(l.context_bytes);old=malloc(l.arena_bytes);copy=malloc(INPUT_CAP);
    for(i=0;i<3;++i)a[i]=malloc(l.arena_bytes);
    for(i=0;i<2;++i)cp[i]=malloc(cfg.shared.checkpoint_bytes);
    REQUIRE(ctx&&ctx_old&&old&&copy&&a[0]&&a[1]&&a[2]&&cp[0]&&cp[1],"test-preallocation");
    REQUIRE(pfm2_init(ctx,l.context_bytes,&cfg,source,a[0],l.arena_bytes,cp[0],cfg.shared.checkpoint_bytes,&initial)==PFR_OK,"init");
    memcpy(ctx_old,ctx,l.context_bytes);REQUIRE(save(argv[3],0,cp[0],initial),"initial-checkpoint");last_bytes=initial;memset(&x,0,sizeof x);
    while(at<n){size_t bytes;unsigned next=(active+1)%3,second=(active+2)%3;
        REQUIRE(n-at>=20&&rd32(source+at+4)<=INPUT_CAP-20,"framed-source");bytes=(size_t)rd32(source+at+4)+20;REQUIRE(bytes<=n-at&&cut<CUT_CAP,"source-cut-cap");
        memcpy(old,a[active],l.arena_bytes);memcpy(copy,source+at,bytes);memset(a[next],0xa5,l.arena_bytes);memset(a[second],0x5a,l.arena_bytes);
        rc=pfm2_prepare(ctx,a[active],a[next],source+at,bytes,cp[0],cfg.shared.checkpoint_bytes,&x);
        other=pfm2_prepare(ctx,a[active],a[second],source+at,bytes,cp[1],cfg.shared.checkpoint_bytes,&y);
        REQUIRE(!memcmp(ctx,ctx_old,l.context_bytes)&&!memcmp(a[active],old,l.arena_bytes)&&!memcmp(copy,source+at,bytes),"accepted-context-input-immutable");
        REQUIRE(rc==other&&x.reason==y.reason&&x.terminal==y.terminal&&x.checkpoint_bytes==y.checkpoint_bytes,"repeat-result");
        if(rc!=PFR_OK){size_t retry_bytes;uint32_t refused_reason=x.reason;
            REQUIRE(rc==PFO_RESOURCE&&x.reason&&!x.terminal&&!x.checkpoint_bytes,"typed-zero-successor-refusal");
            if(!retry){printf("PASS refused_index=%u reason=%u accepted_checkpoint_bytes=%zu retry=0 dirty=1 immutable=1\n",cut,refused_reason,last_bytes);stopped=1;break;}
            REQUIRE(rn>at&&rn-at>=20&&!memcmp(source,retry,at),"retry-same-original-prefix");
            retry_bytes=(size_t)rd32(retry+at+4)+20;REQUIRE(retry_bytes==rn-at,"one-genuine-retry-chunk");
            memset(a[next],0,l.arena_bytes);memset(a[second],0xcc,l.arena_bytes);
            REQUIRE(pfm2_prepare(ctx,a[active],a[next],retry+at,retry_bytes,cp[0],cfg.shared.checkpoint_bytes,&x)==PFR_OK,"valid-retry-clean");
            REQUIRE(pfm2_prepare(ctx,a[active],a[second],retry+at,retry_bytes,cp[1],cfg.shared.checkpoint_bytes,&y)==PFR_OK,"valid-retry-poison");
            REQUIRE(!x.reason&&!y.reason&&!x.terminal&&!y.terminal&&x.checkpoint_bytes==y.checkpoint_bytes&&x.checkpoint_bytes
                &&!memcmp(cp[0],cp[1],x.checkpoint_bytes)&&!memcmp(a[next],a[second],l.arena_bytes),"same-accepted-retry-canonical");
            REQUIRE(!memcmp(ctx,ctx_old,l.context_bytes)&&!memcmp(a[active],old,l.arena_bytes)&&!memcmp(copy,source+at,bytes)&&!memcmp(retry,retry_old,rn),"retry-did-not-acknowledge");
            REQUIRE(save(argv[3],UINT32_MAX,cp[0],x.checkpoint_bytes),"retry-checkpoint");
            printf("PASS refused_index=%u reason=%u accepted_checkpoint_bytes=%zu retry=1 dirty=1 immutable=1\n",cut,refused_reason,last_bytes);
            stopped=1;break;
        }
        REQUIRE(!x.reason&&!y.reason&&!memcmp(a[next],a[second],l.arena_bytes)&&!memcmp(cp[0],cp[1],x.checkpoint_bytes),"success-dirty-independent");
        active=next;at+=bytes;++cut;
        if(!x.terminal){REQUIRE(save(argv[3],cut,cp[0],x.checkpoint_bytes),"accepted-checkpoint");last_bytes=x.checkpoint_bytes;}
        else {REQUIRE(at==n&&!x.checkpoint_bytes,"genuine-final-only");break;}
    }
    if(!stopped)printf("PASS complete_chunks=%u terminal=%u\n",cut,x.terminal);
    free(source);free(retry);free(retry_old);free(ctx);free(ctx_old);free(old);free(copy);
    for(i=0;i<3;++i)free(a[i]);
    for(i=0;i<2;++i)free(cp[i]);
    return 0;
}
