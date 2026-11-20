/* Actual AUTO row work admission. All contexts have identical capacity/layout;
 * only the configured work allowance differs. No private state access. */
#include "combined_auto_resource_config.h"
#define COMBINED2_ENCODER_TEST_CONFIG_H
static pfm2_config test_config(void){pfm2_config c=resource_config();resource_override(&c);return c;}
#define main unused_general_auto_encoder_main
#include "combined2_encoder_fixture.c"
#undef main
#define REQUIRE(x,label) do{if(!(x)){fprintf(stderr,"AUTO_WORK[%s]: %s\n",label,#x);exit(1);}}while(0)
int main(int argc,char **argv)
{
    pfm2_config cfg=test_config(),limited;pfm2_layout l,other;pfo_prepared r,s;
    FILE *f;long size;char *end;unsigned long wanted;unsigned cut=0,active=0,i;
    uint8_t *source,*copy,*bad,*ctx,*ctx_old,*limited_ctx,*limited_old,*a[3],*old,*initial_arena,*cp[2];
    size_t n,at=16,bytes=0,initial=0;uint64_t core_work=0,family_work=0,total,maximum;
    if(argc!=4){fputs("usage: combined-auto-work-fixture SOURCE OUTPUT_DIR AUTO_CHUNK_INDEX\n",stderr);return 2;}
    REQUIRE(*argv[3]&&strspn(argv[3],"0123456789")==strlen(argv[3]),"index-digits");
    errno=0;wanted=strtoul(argv[3],&end,10);REQUIRE(!errno&&!*end&&wanted<CUT_CAP,"index-bound");
    f=fopen(argv[1],"rb");REQUIRE(f,"source-open");
    REQUIRE(!fseek(f,0,SEEK_END)&&(size=ftell(f))>=16&&size<=INPUT_CAP&&!fseek(f,0,SEEK_SET),"source-bound");
    n=(size_t)size;source=malloc(n);copy=malloc(n);bad=malloc(n);REQUIRE(source&&copy&&bad,"source-storage");
    REQUIRE(fread(source,1,n,f)==n&&!ferror(f)&&fgetc(f)==EOF&&!fclose(f),"source-read");memcpy(copy,source,n);
    REQUIRE(pfm2_plan(&cfg,&l)==PFR_OK&&l.context_bytes<=STATE_CAP&&l.arena_bytes<=STATE_CAP,"plan");
    ctx=malloc(l.context_bytes);ctx_old=malloc(l.context_bytes);limited_ctx=malloc(l.context_bytes);limited_old=malloc(l.context_bytes);
    old=malloc(l.arena_bytes);initial_arena=malloc(l.arena_bytes);
    for(i=0;i<3;++i)a[i]=malloc(l.arena_bytes);
    for(i=0;i<2;++i)cp[i]=malloc(cfg.shared.checkpoint_bytes);
    REQUIRE(ctx&&ctx_old&&limited_ctx&&limited_old&&old&&initial_arena&&a[0]&&a[1]&&a[2]&&cp[0]&&cp[1],"preallocation");
    REQUIRE(pfm2_init(ctx,l.context_bytes,&cfg,source,a[0],l.arena_bytes,cp[0],cfg.shared.checkpoint_bytes,&initial)==PFR_OK,"init");
    memcpy(ctx_old,ctx,l.context_bytes);
    while(cut<wanted){unsigned next=(active+1)%3;
        REQUIRE(n-at>=20&&rd32(source+at+4)<=n-at-20,"prefix-frame");bytes=(size_t)rd32(source+at+4)+20;
        REQUIRE(pfm2_prepare(ctx,a[active],a[next],source+at,bytes,cp[0],cfg.shared.checkpoint_bytes,&r)==PFR_OK&&!r.terminal,"accepted-original-prefix");
        active=next;at+=bytes;++cut;
    }
    REQUIRE(n-at>=20&&rd32(source+at)==35&&rd32(source+at+4)<=n-at-20,"actual-auto-row");bytes=(size_t)rd32(source+at+4)+20;
    REQUIRE(pfm2_work(ctx,source+at,bytes,&core_work,&family_work)==PFR_OK&&core_work&&family_work&&family_work<=UINT64_MAX-core_work,"work-quote");
    total=core_work+family_work;maximum=core_work>family_work?core_work:family_work;REQUIRE(maximum<total,"strict-sum");
    memcpy(old,a[active],l.arena_bytes);memcpy(bad,source+at,bytes);bad[bytes-1]^=1;
    for(i=0;i<2;++i){unsigned next=(active+1)%3,second=(active+2)%3;
        limited=cfg;limited.shared.prepare_work=i?total:maximum;
        REQUIRE(pfm2_plan(&limited,&other)==PFR_OK&&!memcmp(&other,&l,sizeof l),"identical-capacity-plan");
        REQUIRE(pfm2_init(limited_ctx,l.context_bytes,&limited,source,initial_arena,l.arena_bytes,cp[0],cfg.shared.checkpoint_bytes,&initial)==PFR_OK,"allowance-context-init");
        memcpy(limited_old,limited_ctx,l.context_bytes);memset(a[next],0,l.arena_bytes);memset(a[second],0xcc,l.arena_bytes);
        if(!i){
            REQUIRE(pfm2_prepare(limited_ctx,a[active],a[next],source+at,bytes,cp[0],cfg.shared.checkpoint_bytes,&r)==PFO_RESOURCE
                &&r.reason==PFR_STATE_BYTES&&!r.checkpoint_bytes&&!r.terminal,"actual-row-max-is-not-sum");
            REQUIRE(pfm2_prepare(limited_ctx,a[active],a[second],bad,bytes,cp[1],cfg.shared.checkpoint_bytes,&s)==PFO_RESOURCE
                &&s.reason==PFR_STATE_BYTES&&!s.checkpoint_bytes&&!s.terminal,"work-before-crc");
            REQUIRE(!memcmp(bad,source+at,bytes-1)&&(uint8_t)(bad[bytes-1]^source[at+bytes-1])==1,"bad-input-immutable");
        }else{
            REQUIRE(pfm2_prepare(limited_ctx,a[active],a[next],source+at,bytes,cp[0],cfg.shared.checkpoint_bytes,&r)==PFR_OK,"exact-sum-clean");
            REQUIRE(pfm2_prepare(limited_ctx,a[active],a[second],source+at,bytes,cp[1],cfg.shared.checkpoint_bytes,&s)==PFR_OK,"exact-sum-poison");
            REQUIRE(!r.reason&&!s.reason&&!r.terminal&&!s.terminal&&r.checkpoint_bytes&&r.checkpoint_bytes==s.checkpoint_bytes
                &&!memcmp(cp[0],cp[1],r.checkpoint_bytes)&&!memcmp(a[next],a[second],l.arena_bytes),"exact-sum-dirty-canonical");
            REQUIRE(save(argv[2],0,cp[0],r.checkpoint_bytes),"success-checkpoint");
        }
        REQUIRE(!memcmp(ctx,ctx_old,l.context_bytes)&&!memcmp(limited_ctx,limited_old,l.context_bytes)
            &&!memcmp(a[active],old,l.arena_bytes)&&!memcmp(source,copy,n),"context-accepted-input-immutable");
    }
    printf("PASS actual_auto_row=1 summed=1 pre_crc=1 immutable=1 dirty=1 core=%llu family=%llu\n",
        (unsigned long long)core_work,(unsigned long long)family_work);
    free(source);free(copy);free(bad);free(ctx);free(ctx_old);free(limited_ctx);free(limited_old);free(old);free(initial_arena);
    for(i=0;i<3;++i)free(a[i]);
    for(i=0;i<2;++i)free(cp[i]);
    return 0;
}
