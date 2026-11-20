/* Original source bytes only -> general C prepare callback -> checkpoints.
 * Test harness, not a product converter. No Rust checkpoint is an input. */
#include "roll_ppu.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
enum { SOURCE_CAP=4194304, ARENA_CAP=1048576, CUT_CAP=4096, PATH_CAP=1024, MANIFEST_CAP=1048576 };
static uint32_t r32(const unsigned char *p)
{ return (uint32_t)p[0] | (uint32_t)p[1]<<8 | (uint32_t)p[2]<<16 | (uint32_t)p[3]<<24; }
static int path_for(char path[PATH_CAP],const char *dir,const char *name)
{ int n=snprintf(path,PATH_CAP,"%s/%s",dir,name); return n>=0&&(size_t)n<PATH_CAP; }
static int write_new(const char *dir,const char *name,const void *bytes,size_t n)
{
    char path[PATH_CAP]; FILE *f; int ok;
    if(!path_for(path,dir,name))return 0;
    f=fopen(path,"wbx"); if(!f){fprintf(stderr,"ppu_encoder_fixture: cannot exclusively create %s\n",path);return 0;}
    ok=fwrite(bytes,1,n,f)==n; if(fclose(f))ok=0;
    if(!ok){(void)remove(path);fprintf(stderr,"ppu_encoder_fixture: short write/close %s\n",path);} return ok;
}
static int read_source(const char *path,unsigned char **bytes,size_t *n)
{
    FILE *f=fopen(path,"rb"); long len; unsigned char *p=NULL; int ok=0;
    if(!f){fprintf(stderr,"ppu_encoder_fixture: cannot open original source %s\n",path);return 0;}
    if(fseek(f,0,SEEK_END)||(len=ftell(f))<16||len>SOURCE_CAP||fseek(f,0,SEEK_SET))goto done;
    p=malloc((size_t)len); if(!p)goto done;
    if(fread(p,1,(size_t)len,f)!=(size_t)len||fgetc(f)!=EOF||ferror(f))goto done;
    *n=(size_t)len; ok=1;
done:
    if(fclose(f))ok=0;
    if(!ok){free(p);fprintf(stderr,"ppu_encoder_fixture: invalid size/transfer for original source\n");}
    else *bytes=p;
    return ok;
}
static void name_cut(char name[32],unsigned cut) { (void)snprintf(name,32,"cut-%06u.ckp",cut); }
static int number(const char *text,uint32_t *value)
{
    char *end;unsigned long long n;
    if(!*text||strspn(text,"0123456789")!=strlen(text))return 0;
    errno=0;n=strtoull(text,&end,10);
    if(errno||*end||n>UINT32_MAX)return 0;
    *value=(uint32_t)n;return 1;
}
int main(int argc,char **argv)
{
    pfp_config cfg={16,16,32,64,32768,65536}; pfp_layout layout;
    unsigned char *source=NULL,*cp[2]={NULL,NULL},*ctx=NULL,*ctx_copy=NULL,*arena[3]={NULL,NULL,NULL},*before=NULL,*input_copy=NULL;
    char *manifest=NULL; size_t length=0,offset=16,cp_bytes=0,mn=0; unsigned active=0,created=0,chunks=0,i; int rc=1,terminal=0;
    if(argc!=3&&argc!=9){fputs("usage: rolling-ppu-encoder-fixture ORIGINAL_SOURCE FRESH_OUTPUT_DIRECTORY [INC DESCS STACK IDENTITIES METADATA CHECKPOINT]\n",stderr);return 2;}
    if(argc==9&&(!number(argv[3],&cfg.incarnations)||!number(argv[4],&cfg.descriptors)||!number(argv[5],&cfg.stack_rows)
        ||!number(argv[6],&cfg.frame_identities)||!number(argv[7],&cfg.metadata_bytes)||!number(argv[8],&cfg.checkpoint_bytes))){
        fputs("ppu_encoder_fixture: invalid numeric capacity\n",stderr);return 2;
    }
    if(cfg.checkpoint_bytes>1048576u){fputs("ppu_encoder_fixture: checkpoint over fixture cap\n",stderr);return 2;}
    if(!read_source(argv[1],&source,&length))goto done;
    if(pfp_plan(&cfg,&layout)!=PFR_OK){fputs("ppu_encoder_fixture: general PPU arena planner unavailable\n",stderr);goto done;}
    if(!layout.context_bytes||layout.context_bytes>ARENA_CAP||!layout.arena_bytes||layout.arena_bytes>ARENA_CAP){fputs("ppu_encoder_fixture: arena/context over fixture cap\n",stderr);goto done;}
    ctx=malloc(layout.context_bytes);ctx_copy=malloc(layout.context_bytes);before=malloc(layout.arena_bytes);
    for(i=0;i<3;++i)arena[i]=malloc(layout.arena_bytes);
    for(i=0;i<2;++i)cp[i]=malloc(cfg.checkpoint_bytes);
    manifest=malloc(MANIFEST_CAP);input_copy=malloc(1048576u+20u);
    if(!ctx||!ctx_copy||!before||!arena[0]||!arena[1]||!arena[2]||!cp[0]||!cp[1]||!manifest||!input_copy)goto done;
    if(pfp_init(ctx,layout.context_bytes,&cfg,source,arena[0],layout.arena_bytes,cp[0],cfg.checkpoint_bytes,&cp_bytes)!=PFR_OK){fputs("ppu_encoder_fixture: general PPU initial checkpoint unavailable\n",stderr);goto done;}
    if(!cp_bytes||cp_bytes>cfg.checkpoint_bytes)goto done;
    memcpy(ctx_copy,ctx,layout.context_bytes);
    if(!write_new(argv[2],"cut-000000.ckp",cp[0],cp_bytes))goto done;
    created=1;
    mn=(size_t)snprintf(manifest,MANIFEST_CAP,"{\"schema\":1,\"cuts\":[{\"next_chunk\":0,\"source_offset\":16,\"checkpoint_bytes\":%zu}",cp_bytes);
    while(offset<length){
        size_t n; unsigned next=(active+1)%3,relocated=(active+2)%3;
        pfo_prepared result,repeat; int a,b,written;
        if(terminal||length-offset<20||chunks>=CUT_CAP){fputs("ppu_encoder_fixture: fixture framing/count/terminal limit\n",stderr);goto done;}
        if(r32(source+offset+4)>1048576u){fputs("ppu_encoder_fixture: original chunk over format cap\n",stderr);goto done;}
        n=(size_t)r32(source+offset+4)+20;
        if(n>length-offset){fputs("ppu_encoder_fixture: torn original chunk\n",stderr);goto done;}
        memcpy(before,arena[active],layout.arena_bytes);memcpy(input_copy,source+offset,n);
        memset(arena[next],0xa5,layout.arena_bytes);memset(arena[relocated],0x5a,layout.arena_bytes);
        memset(&result,0xa5,sizeof result);memset(&repeat,0x5a,sizeof repeat);
        a=pfp_prepare(ctx,arena[active],arena[next],source+offset,n,cp[0],cfg.checkpoint_bytes,&result);
        b=pfp_prepare(ctx,arena[active],arena[relocated],source+offset,n,cp[1],cfg.checkpoint_bytes,&repeat);
        if(memcmp(before,arena[active],layout.arena_bytes)||memcmp(ctx,ctx_copy,layout.context_bytes)||memcmp(input_copy,source+offset,n)){
            fputs("ppu_encoder_fixture: prepare changed accepted arena/context/input\n",stderr);goto done;
        }
        if(a!=PFR_OK||b!=PFR_OK){fprintf(stderr,"ppu_encoder_fixture: valid original chunk %u refused (%d,%d), reason %u\n",chunks,a,b,result.reason);goto done;}
        if(result.reason||repeat.reason||result.terminal!=(r32(source+offset)==6)||result.terminal!=repeat.terminal
            ||result.checkpoint_bytes!=repeat.checkpoint_bytes||result.checkpoint_bytes>cfg.checkpoint_bytes
            ||memcmp(arena[next],arena[relocated],layout.arena_bytes)
            ||memcmp(cp[0],cp[1],result.checkpoint_bytes)){
            fputs("ppu_encoder_fixture: callback result/canonical prepared arena depends on dirty scratch\n",stderr);goto done;
        }
        terminal=(int)result.terminal;
        if((terminal&&result.checkpoint_bytes)||(!terminal&&!result.checkpoint_bytes))goto done;
        /* Relocate the exact successful arena and poison both old addresses.
         * A pointer into old accepted/prepared storage cannot remain usable. */
        memcpy(arena[relocated],arena[next],layout.arena_bytes);
        memset(arena[active],0xdd,layout.arena_bytes);memset(arena[next],0xee,layout.arena_bytes);
        active=relocated; offset+=n; ++chunks;
        if(!terminal){
            char name[32]; name_cut(name,chunks);
            if(!write_new(argv[2],name,cp[0],result.checkpoint_bytes))goto done;
            ++created;
            if(mn>=MANIFEST_CAP)goto done;
            written=snprintf(manifest+mn,MANIFEST_CAP-mn,",{\"next_chunk\":%u,\"source_offset\":%zu,\"checkpoint_bytes\":%zu}",chunks,offset,result.checkpoint_bytes);
            if(written<0||(size_t)written>=MANIFEST_CAP-mn)goto done;
            mn+=(size_t)written;
        }
    }
    {
        int written=snprintf(manifest+mn,MANIFEST_CAP-mn,"],\"next_chunk\":%u,\"prefix_bytes\":%zu,\"terminal\":%u}\n",chunks,offset-16,(unsigned)terminal);
        if(written<0||(size_t)written>=MANIFEST_CAP-mn)goto done;
        mn+=(size_t)written;
    }
    if(!write_new(argv[2],"cuts.json",manifest,mn))goto done;
    printf("PASS general C encoder: %u original chunks, %u checkpoints, terminal %u; immutable/relocatable arenas\n",chunks,created,(unsigned)terminal);rc=0;
done:
    if(rc)fprintf(stderr,"ppu_encoder_fixture: retaining %u successful checkpoints in owned fixture directory %s\n",created,argv[2]);
    free(manifest);for(i=0;i<2;++i)free(cp[i]);for(i=0;i<3;++i)free(arena[i]);
    free(input_copy);free(before);free(ctx_copy);free(ctx);free(source);return rc;
}
