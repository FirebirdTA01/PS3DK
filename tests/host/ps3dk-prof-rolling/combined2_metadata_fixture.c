/* Independent retained-metadata census drives exact-capacity admission.
 * The unchanged general fixture supplies only config/helpers; no encoder
 * internal state or expected checkpoint enters this program. */
#define main combined2_parity_fixture_main
#include "combined2_encoder_fixture.c"
#undef main
#include <inttypes.h>
#define META_CHECK(label,x) do{if(!(x)){fprintf(stderr,"PFM2_META_ASSERT[%s]: %s\n",label,#x);goto release;}}while(0)
static uint8_t *meta_load(const char *path,size_t *size)
{
    FILE *f=fopen(path,"rb");uint8_t *bytes;long n;if(!f)return NULL;
    if(fseek(f,0,SEEK_END)||(n=ftell(f))<16||n>INPUT_CAP||fseek(f,0,SEEK_SET)){fclose(f);return NULL;}
    bytes=malloc((size_t)n);if(!bytes){fclose(f);return NULL;}
    if(fread(bytes,1,(size_t)n,f)!=(size_t)n||fgetc(f)!=EOF||ferror(f)){free(bytes);fclose(f);return NULL;}
    if(fclose(f)){free(bytes);return NULL;}*size=(size_t)n;return bytes;
}
static int meta_save(const char *dir,const char *name,const void *bytes,size_t n)
{char path[2048];FILE *f;int k=snprintf(path,sizeof path,"%s/%s",dir,name),ok;
    if(k<0||(size_t)k>=sizeof path)return 0;
    f=fopen(path,"wbx");if(!f)return 0;ok=fwrite(bytes,1,n,f)==n;if(fclose(f))ok=0;return ok;}
static int meta_case(const uint8_t *source,size_t size,const char *dir,const char *name,
                     uint32_t cap,unsigned stop,unsigned refused,int expected)
{
    pfm2_config c=test_config();pfm2_layout l;void *ctx=NULL,*ctx_old=NULL,*a[2]={NULL,NULL},*old=NULL;
    uint8_t *cp=NULL,*current=NULL,*raw_old=NULL;size_t at=16,current_bytes=0;unsigned active=0,index=0;int ok=0,rc;
    c.shared.metadata_bytes=cap;
    META_CHECK("plan",pfm2_plan(&c,&l)==PFR_OK&&l.arena_bytes<=STATE_CAP&&l.context_bytes<=STATE_CAP);
    ctx=malloc(l.context_bytes);ctx_old=malloc(l.context_bytes);a[0]=malloc(l.arena_bytes);a[1]=malloc(l.arena_bytes);
    old=malloc(l.arena_bytes);cp=malloc(c.shared.checkpoint_bytes);current=malloc(c.shared.checkpoint_bytes);raw_old=malloc(INPUT_CAP);
    META_CHECK("allocate",ctx&&ctx_old&&a[0]&&a[1]&&old&&cp&&current&&raw_old);
    META_CHECK("init",pfm2_init(ctx,l.context_bytes,&c,source,a[0],l.arena_bytes,current,c.shared.checkpoint_bytes,&current_bytes)==PFR_OK);
    memcpy(ctx_old,ctx,l.context_bytes);
    while(index<=stop){size_t n;pfo_prepared result;
        META_CHECK("source-shape",at<=size&&size-at>=20&&rd32(source+at+4)<=size-at-20);n=20+(size_t)rd32(source+at+4);
        memcpy(old,a[active],l.arena_bytes);memcpy(raw_old,source+at,n);memset(a[active^1u],0xa5,l.arena_bytes);
        rc=pfm2_prepare(ctx,a[active],a[active^1u],source+at,n,cp,c.shared.checkpoint_bytes,&result);
        META_CHECK("replacement-accepted-immutable",!memcmp(old,a[active],l.arena_bytes)&&!memcmp(ctx_old,ctx,l.context_bytes)&&!memcmp(raw_old,source+at,n));
        if(expected!=PFR_OK&&index==refused){
            META_CHECK("short-or-invalid-boundary",rc==expected&&!result.checkpoint_bytes&&!result.terminal
                &&result.reason==(expected==PFO_RESOURCE?PFR_SUPPORT:0));break;
        }
        META_CHECK("exact-net-capacity",rc==PFR_OK&&result.checkpoint_bytes&&!result.terminal&&!result.reason);
        current_bytes=result.checkpoint_bytes;memcpy(current,cp,current_bytes);active^=1u;at+=n;++index;
    }
    META_CHECK("expected-stop",index==(expected==PFR_OK?stop+1:refused));
    META_CHECK("checkpoint-artifact",meta_save(dir,name,current,current_bytes));ok=1;
release:
    free(ctx);free(ctx_old);free(a[0]);free(a[1]);free(old);free(cp);free(current);free(raw_old);return ok;
}
int main(int argc,char **argv)
{
    uint8_t *source=NULL,*bad=NULL;size_t size=0,bad_size=0,at=16,kept=16,last_bytes=0,final_cap=0;
    unsigned index=0,last=UINT32_MAX,over=UINT32_MAX,statuses=0;char json[512];int len,ok=0;
    if(argc!=4){fputs("usage: metadata-fixture ORIGINAL MALFORMED_REPLACEMENT OUTPUT_DIR\n",stderr);return 2;}
    source=meta_load(argv[1],&size);bad=meta_load(argv[2],&bad_size);META_CHECK("inputs",source&&bad&&size==bad_size);
    while(at<size){size_t n;uint32_t ty;META_CHECK("framed-source",size-at>=20&&rd32(source+at+4)<=size-at-20);
        n=20+(size_t)rd32(source+at+4);ty=rd32(source+at);
        if(ty==33){META_CHECK("fixed-status",n==100&&(!last_bytes||last_bytes==n+4));kept-=last_bytes;last_bytes=n+4;last=index;++statuses;}
        if(ty!=5&&ty!=13&&ty!=32&&ty!=43&&ty!=6)kept+=n+4;
        if(ty==33)final_cap=kept;
        at+=n;++index;
    }
    META_CHECK("replacement-witness",statuses>=2&&last!=UINT32_MAX&&final_cap>16&&final_cap<=UINT32_MAX);
    /* Tight cap succeeds at replacement even though old+incoming would not.
     * With fixed-width STATUS, M-1 must first fail at an earlier growth row. */
    at=16;kept=16;last_bytes=0;index=0;
    while(index<=last){size_t n=20+(size_t)rd32(source+at+4);uint32_t ty=rd32(source+at);
        if(ty==33){kept-=last_bytes;last_bytes=n+4;}
        if(ty!=5&&ty!=13&&ty!=32&&ty!=43&&ty!=6)kept+=n+4;
        META_CHECK("prior-prefix-fits",kept<=final_cap);
        if(over==UINT32_MAX&&kept>final_cap-1)over=index;
        at+=n;++index;
    }
    META_CHECK("one-byte-short-growth",over<last&&last_bytes>0&&final_cap+last_bytes>final_cap);
    META_CHECK("exact-case",meta_case(source,size,argv[3],"exact.ckp",(uint32_t)final_cap,last,0,PFR_OK));
    META_CHECK("short-case",meta_case(source,size,argv[3],"short.ckp",(uint32_t)final_cap-1,last,over,PFO_RESOURCE));
    META_CHECK("invalid-case",meta_case(bad,bad_size,argv[3],"invalid.ckp",(uint32_t)final_cap,last,last,PFR_INVALID));
    len=snprintf(json,sizeof json,"{\"metadata_cap\":%zu,\"last_status\":%u,\"first_over\":%u}\n",final_cap,last,over);
    META_CHECK("result-json",len>0&&(size_t)len<sizeof json&&meta_save(argv[3],"metadata-cap.json",json,(size_t)len));
    puts("PASS exact-net-capacity one-byte-short-growth malformed-replacement immutable=1");ok=1;
release:
    free(source);free(bad);return ok?0:1;
}
