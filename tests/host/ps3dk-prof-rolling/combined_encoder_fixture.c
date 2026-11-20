/* Receives ordinary source only, never Rust checkpoint bytes. Every success
 * is repeated against dirty storage, then relocated before the next chunk. */
#include "combined_encoder_config.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
enum { INPUT_CAP=1048576, STATE_CAP=1048576, CUT_CAP=4096 };
static uint32_t rd32(const uint8_t *p)
{return (uint32_t)p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24;}
static int save(const char *dir,unsigned cut,const void *p,size_t n)
{
    char path[2048];FILE *f;int k=snprintf(path,sizeof path,"%s/cut-%06u.ckp",dir,cut),ok;
    if(k<0||(size_t)k>=sizeof path)return 0;
    f=fopen(path,"wbx");if(!f)return 0;ok=fwrite(p,1,n,f)==n;if(fclose(f))ok=0;return ok;
}
int main(int argc,char **argv)
{
    pfm_config c=test_config();pfm_layout l;pfo_prepared x,y;
    FILE *f=NULL;uint8_t *source=NULL,*ctx=NULL,*ctx_old=NULL,*a[3]={NULL,NULL,NULL},*old=NULL,*cp[2]={NULL,NULL};
    uint8_t *chunk_copy=NULL;size_t n=0,at=16,initial=0;long length;unsigned active=0,cut=0,i;int rc=1,terminal=0,got;
    if(argc!=3){fputs("usage: combined-encoder-fixture ORIGINAL_SOURCE FRESH_OUTPUT_DIRECTORY\n",stderr);return 2;}
    f=fopen(argv[1],"rb");if(!f)goto done;
    if(fseek(f,0,SEEK_END)||(length=ftell(f))<16||length>INPUT_CAP||fseek(f,0,SEEK_SET))goto done;
    n=(size_t)length;source=malloc(n);if(!source||fread(source,1,n,f)!=n||fgetc(f)!=EOF||ferror(f))goto done;
    if(fclose(f)){f=NULL;goto done;}f=NULL;
    got=pfm_plan(&c,&l);if(got!=PFR_OK){fprintf(stderr,"reject stage=plan rc=%d\n",got);goto done;}
    if(!l.context_bytes||l.context_bytes>STATE_CAP||!l.arena_bytes||l.arena_bytes>STATE_CAP)goto done;
    ctx=malloc(l.context_bytes);ctx_old=malloc(l.context_bytes);old=malloc(l.arena_bytes);chunk_copy=malloc(INPUT_CAP);
    for(i=0;i<3;++i)a[i]=malloc(l.arena_bytes);
    for(i=0;i<2;++i)cp[i]=malloc(c.checkpoint_bytes);
    if(!ctx||!ctx_old||!old||!chunk_copy||!a[0]||!a[1]||!a[2]||!cp[0]||!cp[1])goto done;
    got=pfm_init(ctx,l.context_bytes,&c,source,a[0],l.arena_bytes,cp[0],c.checkpoint_bytes,&initial);
    if(got!=PFR_OK){fprintf(stderr,"reject stage=init rc=%d\n",got);goto done;}
    if(!initial||initial>c.checkpoint_bytes||!save(argv[2],0,cp[0],initial))goto done;
    memcpy(ctx_old,ctx,l.context_bytes);
    while(at<n){size_t bytes;unsigned next=(active+1)%3,relocated=(active+2)%3;int repeat;
        if(terminal||n-at<20||cut>=CUT_CAP||rd32(source+at+4)>INPUT_CAP-20)goto done;
        bytes=(size_t)rd32(source+at+4)+20;if(bytes>n-at)goto done;
        memcpy(old,a[active],l.arena_bytes);memcpy(chunk_copy,source+at,bytes);
        memset(a[next],0xa5,l.arena_bytes);memset(a[relocated],0x5a,l.arena_bytes);
        memset(&x,0xa5,sizeof x);memset(&y,0x5a,sizeof y);
        got=pfm_prepare(ctx,a[active],a[next],source+at,bytes,cp[0],c.checkpoint_bytes,&x);
        repeat=pfm_prepare(ctx,a[active],a[relocated],source+at,bytes,cp[1],c.checkpoint_bytes,&y);
        if(memcmp(old,a[active],l.arena_bytes)||memcmp(ctx,ctx_old,l.context_bytes)||memcmp(chunk_copy,source+at,bytes)){
            fputs("FAIL accepted arena/context/input changed\n",stderr);goto done;}
        if(got!=PFR_OK){
            if(repeat!=got||x.checkpoint_bytes||x.terminal||y.checkpoint_bytes||y.terminal||x.reason!=y.reason
               ||(got==PFR_INVALID&&x.reason)){
                fputs("FAIL rejected result exposed successor or was nondeterministic\n",stderr);goto done;}
            fprintf(stderr,"reject stage=prepare index=%u rc=%d reason=%u\n",cut,got,x.reason);goto done;
        }
        if(repeat!=PFR_OK||x.reason||y.reason||x.terminal!=(rd32(source+at)==6)||x.terminal!=y.terminal
           ||x.checkpoint_bytes!=y.checkpoint_bytes||x.checkpoint_bytes>c.checkpoint_bytes
           ||memcmp(a[next],a[relocated],l.arena_bytes)||memcmp(cp[0],cp[1],x.checkpoint_bytes)){
            fputs("FAIL successful state/checkpoint depends on dirty scratch\n",stderr);goto done;}
        terminal=(int)x.terminal;if((terminal&&x.checkpoint_bytes)||(!terminal&&!x.checkpoint_bytes))goto done;
        memcpy(a[relocated],a[next],l.arena_bytes);memset(a[active],0xdd,l.arena_bytes);memset(a[next],0xee,l.arena_bytes);
        active=relocated;at+=bytes;++cut;
        if(!terminal&&!save(argv[2],cut,cp[0],x.checkpoint_bytes))goto done;
    }
    printf("PASS chunks=%u bytes=%zu terminal=%d relocated=1 immutable=1\n",cut,at-16,terminal);rc=0;
done:
    if(f)(void)fclose(f);
    free(chunk_copy);free(old);free(ctx_old);free(ctx);free(source);
    for(i=0;i<3;++i)free(a[i]);
    for(i=0;i<2;++i)free(cp[i]);
    return rc;
}
