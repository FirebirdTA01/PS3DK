/* Test-only actual pfo + pfp + pfr integration. Original .prof input only.
 * Memory IO supplies a deterministic fault model, not filesystem durability.
 * All failed evidence stays in the caller's unique owned fixture directory. */
#include "roll_ppu.h"
#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
enum { SOURCE_CAP=4194304,FILE_CAP=4194304,ARENA_CAP=1048576,IO_CAP=16384,CALL_CAP=4096,PATH_CAP=1024 };
#define CHECK(x) do { if(!(x)){fprintf(stderr,"FAIL ppu-output:%d: %s\n",__LINE__,#x);exit(1);} } while(0)
#define SEMANTIC(name,x) do { if(!(x)){fprintf(stderr,"C_SEMANTIC[%s]: %s\n",name,#x);exit(1);} } while(0)
typedef struct { unsigned kind,ordinal;uint64_t offset,requested;int64_t result; } io_event;
typedef struct {
    uint8_t *bytes;size_t length,capacity;
    unsigned writes,syncs,fail_kind,fail_nth,short_bytes,triggered,trigger_index;
    io_event events[IO_CAP];unsigned count;
} memory;
typedef struct { unsigned op;uint64_t original;int rc;pfo_view before,after;unsigned writes_before,syncs_before,writes_after,syncs_after; } call_event;
typedef struct {
    memory *m; pfr_store *store; void *scratch; pfr_config store_cfg; pfr_io io;
    pfo_output *output;pfo_buffers buffers;pfp_config cfg;pfp_layout plan;void *context;
    uint8_t *initial,*shadow_cp,*backup,*shadow[2],*known_good;unsigned shadow_active,io_failed;
    pfr_view known_view;call_event calls[CALL_CAP];unsigned call_count,checkpoints;
    const char *directory;
} fixture;
static uint32_t r32(const uint8_t *p){return (uint32_t)p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24;}
static void event(memory *m,unsigned kind,unsigned ordinal,uint64_t offset,size_t n,int64_t result)
{
    io_event *e;CHECK(m->count<IO_CAP);e=&m->events[m->count++];e->kind=kind;e->ordinal=ordinal;e->offset=offset;e->requested=n;e->result=result;
}
static int64_t read_at(void *ctx,uint64_t at,void *p,size_t n)
{memory *m=ctx;if(at>m->length||n>m->length-(size_t)at)return -1;memcpy(p,m->bytes+(size_t)at,n);return (int64_t)n;}
static int64_t write_at(void *ctx,uint64_t at,const void *p,size_t n)
{
    memory *m=ctx;size_t actual=n;int64_t result;++m->writes;
    if(!m->triggered&&(m->fail_kind==1||m->fail_kind==3)&&m->writes==m->fail_nth){
        m->triggered=1;m->trigger_index=m->count;result=m->fail_kind==3?0:-1;
    }else if(at>m->length||n>m->length-(size_t)at)result=-1;
    else{if(m->short_bytes&&actual>m->short_bytes)actual=m->short_bytes;memcpy(m->bytes+(size_t)at,p,actual);result=(int64_t)actual;}
    event(m,1,m->writes,at,n,result);return result;
}
static int resize_memory(void *ctx,uint64_t n)
{memory *m=ctx;if(n>m->capacity)return -1;memset(m->bytes,0,(size_t)n);m->length=(size_t)n;return 0;}
static int size_memory(void *ctx,uint64_t *n){*n=((memory *)ctx)->length;return 0;}
static int sync_memory(void *ctx)
{
    memory *m=ctx;int rc=0;++m->syncs;
    if(!m->triggered&&m->fail_kind==2&&m->syncs==m->fail_nth){m->triggered=1;m->trigger_index=m->count;rc=-1;}
    event(m,2,m->syncs,0,0,rc);return rc;
}
static void path(char out[PATH_CAP],const char *dir,const char *name)
{int n=snprintf(out,PATH_CAP,"%s/%s",dir,name);CHECK(n>=0&&(size_t)n<PATH_CAP);}
static void write_new(const char *dir,const char *name,const void *bytes,size_t n)
{
    char out[PATH_CAP];FILE *f;int ok;path(out,dir,name);f=fopen(out,"wbx");CHECK(f);
    ok=fwrite(bytes,1,n,f)==n;if(fclose(f))ok=0;CHECK(ok);
}
static uint8_t *load(const char *name,size_t *length)
{
    FILE *f=fopen(name,"rb");long size;uint8_t *p;CHECK(f);
    CHECK(!fseek(f,0,SEEK_END)&&(size=ftell(f))>=16&&size<=SOURCE_CAP&&!fseek(f,0,SEEK_SET));
    p=malloc((size_t)size);CHECK(p);CHECK(fread(p,1,(size_t)size,f)==(size_t)size&&fgetc(f)==EOF&&!ferror(f));CHECK(!fclose(f));
    *length=(size_t)size;return p;
}
static unsigned number(const char *s)
{char *end;unsigned long long n;CHECK(*s&&strspn(s,"0123456789")==strlen(s));errno=0;n=strtoull(s,&end,10);CHECK(!errno&&!*end&&n<=UINT32_MAX);return (unsigned)n;}
static void checkpoint_file(fixture *f,uint64_t next,const void *p,size_t n)
{char name[48];int k=snprintf(name,sizeof name,"accepted-%06" PRIu64 ".ckp",next);CHECK(k>0&&(size_t)k<sizeof name);write_new(f->directory,name,p,n);++f->checkpoints;}
static pfo_view view(fixture *f){pfo_view v;CHECK(pfo_get_view(f->output,&v)==PFR_OK);return v;}
static void init(fixture *f,const uint8_t header[16],unsigned payload)
{
    pfr_layout layout;size_t initial_bytes;unsigned i;
    CHECK(pfp_plan(&f->cfg,&f->plan)==PFR_OK&&f->plan.arena_bytes<=ARENA_CAP&&f->plan.context_bytes<=ARENA_CAP);
    CHECK(f->cfg.checkpoint_bytes<=1048576&&payload>=20&&payload<=1048596);
    f->context=malloc(f->plan.context_bytes);f->initial=malloc(f->cfg.checkpoint_bytes);f->shadow_cp=malloc(f->cfg.checkpoint_bytes);
    f->backup=malloc(f->plan.arena_bytes);CHECK(f->context&&f->initial&&f->shadow_cp&&f->backup);
    for(i=0;i<2;++i){f->buffers.arenas[i]=malloc(f->plan.arena_bytes);f->shadow[i]=malloc(f->plan.arena_bytes);CHECK(f->buffers.arenas[i]&&f->shadow[i]);}
    for(i=0;i<3;++i){f->buffers.checkpoints[i]=malloc(f->cfg.checkpoint_bytes);CHECK(f->buffers.checkpoints[i]);}
    f->buffers.arena_bytes=f->plan.arena_bytes;f->buffers.checkpoint_capacity=f->cfg.checkpoint_bytes;
    f->buffers.payload=malloc(payload);CHECK(f->buffers.payload);f->buffers.payload_capacity=payload;
    f->buffers.encoder_ctx=f->context;f->buffers.prepare=pfp_prepare;
    CHECK(pfp_init(f->context,f->plan.context_bytes,&f->cfg,header,f->buffers.arenas[0],f->plan.arena_bytes,
        f->initial,f->cfg.checkpoint_bytes,&initial_bytes)==PFR_OK);
    memcpy(f->shadow[0],f->buffers.arenas[0],f->plan.arena_bytes);
    f->store_cfg.max_bytes=FILE_CAP;f->store_cfg.retained_slots=3;f->store_cfg.checkpoint_bytes=f->cfg.checkpoint_bytes;
    f->store_cfg.payload_bytes=payload;memset(f->store_cfg.session,0x74,16);
    CHECK(pfr_plan(&f->store_cfg,&layout)==PFR_OK&&layout.file_bytes<=FILE_CAP);
    f->m=calloc(1,sizeof *f->m);CHECK(f->m);f->m->capacity=(size_t)layout.file_bytes;f->m->bytes=malloc(f->m->capacity);CHECK(f->m->bytes);
    f->known_good=malloc(f->m->capacity);f->scratch=malloc(layout.scratch_bytes);f->store=malloc(pfr_state_bytes());f->output=malloc(pfo_state_bytes());
    CHECK(f->known_good&&f->scratch&&f->store&&f->output);
    f->io.ctx=f->m;f->io.read_at=read_at;f->io.write_at=write_at;f->io.resize=resize_memory;f->io.size=size_memory;f->io.sync=sync_memory;
    CHECK(pfr_format_v2(f->store,&f->io,&f->store_cfg,f->scratch,layout.scratch_bytes)==PFR_OK);
    CHECK(pfo_bind(f->output,f->store,&f->buffers,f->initial,initial_bytes)==PFR_OK);
    f->m->count=f->m->writes=f->m->syncs=0; /* injected faults never target setup */
    memcpy(f->known_good,f->m->bytes,f->m->length);CHECK(pfr_get_view(f->store,&f->known_view)==PFR_OK);
    checkpoint_file(f,0,f->initial,initial_bytes);
}
/* op: 0 accept, 1 flush, 2 finish. The actual callback stays pfp_prepare;
 * shadow checking happens only AFTER successful coordinator acceptance. */
static int execute(fixture *f,unsigned op,const uint8_t *input,size_t n,uint64_t original)
{
    call_event *e;int rc;CHECK(f->call_count<CALL_CAP);e=&f->calls[f->call_count++];memset(e,0,sizeof *e);
    e->op=op;e->original=original;e->before=view(f);memcpy(f->backup,e->before.accepted_arena,f->plan.arena_bytes);
    e->writes_before=f->m->writes;e->syncs_before=f->m->syncs;
    if(!f->io_failed){memcpy(f->known_good,f->m->bytes,f->m->length);CHECK(pfr_get_view(f->store,&f->known_view)==PFR_OK);}
    rc=op==0?pfo_accept(f->output,input,n):op==1?pfo_flush(f->output):pfo_finish(f->output);
    e->rc=rc;e->after=view(f);e->writes_after=f->m->writes;e->syncs_after=f->m->syncs;
    if(op==0&&rc==PFR_OK){pfo_prepared prepared;unsigned next=f->shadow_active^1u;
        CHECK(pfp_prepare(f->context,f->shadow[f->shadow_active],f->shadow[next],input,n,f->shadow_cp,f->cfg.checkpoint_bytes,&prepared)==PFR_OK);
        CHECK(!memcmp(f->shadow[next],e->after.accepted_arena,f->plan.arena_bytes));f->shadow_active=next;
        CHECK(e->after.accepted_next_chunk==e->before.accepted_next_chunk+1&&e->after.accepted_payload_bytes==e->before.accepted_payload_bytes+n);
        CHECK(e->after.terminal==prepared.terminal);
        if(!prepared.terminal)checkpoint_file(f,e->after.accepted_next_chunk,f->shadow_cp,prepared.checkpoint_bytes);
    }else{
        SEMANTIC("accepted-state-immutable",e->after.accepted_next_chunk==e->before.accepted_next_chunk&&e->after.accepted_payload_bytes==e->before.accepted_payload_bytes
            &&e->after.accepted_arena==e->before.accepted_arena&&!memcmp(f->backup,e->after.accepted_arena,f->plan.arena_bytes));
        if(op==0)SEMANTIC("refused-pending-immutable",e->after.pending_bytes==e->before.pending_bytes);
    }
    CHECK(!memcmp(f->shadow[f->shadow_active],e->after.accepted_arena,f->plan.arena_bytes));
    if(rc==PFR_IO)f->io_failed=1;
    return rc;
}
static void view_json(FILE *out,const pfo_view *v)
{
    CHECK(fprintf(out,"{\"accepted_next\":%" PRIu64 ",\"accepted_bytes\":%" PRIu64 ",\"committed_next\":%" PRIu64 ",\"committed_bytes\":%" PRIu64
        ",\"pending\":%zu,\"phase\":%u,\"reason\":%u,\"terminal\":%u,\"failure\":%d}",
        v->accepted_next_chunk,v->accepted_payload_bytes,v->committed_next_chunk,v->committed_payload_bytes,v->pending_bytes,v->phase,v->reason,v->terminal,v->failure)>0);
}
static void physical_json(FILE *out,const pfr_view *v)
{
    CHECK(fprintf(out,"{\"revision\":%" PRIu64 ",\"first_segment\":%" PRIu64 ",\"next_segment\":%" PRIu64 ",\"first_chunk\":%" PRIu64
        ",\"next_chunk\":%" PRIu64 ",\"bytes\":%" PRIu64 ",\"state\":%u,\"reason\":%u,\"recovered\":%u}",
        v->generation,v->first_segment,v->next_segment,v->first_chunk,v->next_chunk,v->bound_payload_bytes,v->state,v->reason,v->recovered)>0);
}
static void artifacts(fixture *f,int stop_rc)
{
    char name[PATH_CAP];FILE *out;pfr_store *reopen;void *scratch;pfr_layout layout;pfr_io readonly=f->io;
    pfr_view recovered;int reopen_rc;unsigned i;pfo_view final=view(f);
    write_new(f->directory,"rolling.profr",f->m->bytes,f->m->length);write_new(f->directory,"known-good.profr",f->known_good,f->m->length);
    CHECK(pfr_plan(&f->store_cfg,&layout)==PFR_OK);reopen=malloc(pfr_state_bytes());scratch=malloc(layout.scratch_bytes);CHECK(reopen&&scratch);
    readonly.write_at=NULL;readonly.resize=NULL;readonly.sync=NULL;memset(&recovered,0,sizeof recovered);
    reopen_rc=pfr_open(reopen,&readonly,&f->store_cfg,scratch,layout.scratch_bytes);
    if(reopen_rc==PFR_OK)CHECK(pfr_get_view(reopen,&recovered)==PFR_OK);
    path(name,f->directory,"trace.json");out=fopen(name,"wx");CHECK(out);
    CHECK(fprintf(out,"{\"schema\":1,\"stop_rc\":%d,\"reopen_rc\":%d,\"checkpoints\":%u,\"fault_kind\":%u,\"fault_nth\":%u,\"fault_triggered\":%u,\"fault_io_index\":%u,\"final\":",
        stop_rc,reopen_rc,f->checkpoints,f->m->fail_kind,f->m->fail_nth,f->m->triggered,f->m->trigger_index)>0);view_json(out,&final);
    CHECK(fputs(",\"known_good\":",out)>=0);physical_json(out,&f->known_view);
    CHECK(fputs(",\"reopened\":",out)>=0);physical_json(out,&recovered);CHECK(fputs(",\"calls\":[",out)>=0);
    for(i=0;i<f->call_count;++i){call_event *e=&f->calls[i];
        CHECK(fprintf(out,"%s{\"op\":%u,\"original\":%" PRIu64 ",\"rc\":%d,\"writes_before\":%u,\"syncs_before\":%u,\"writes_after\":%u,\"syncs_after\":%u,\"before\":",
            i?",":"",e->op,e->original,e->rc,e->writes_before,e->syncs_before,e->writes_after,e->syncs_after)>0);
        view_json(out,&e->before);CHECK(fputs(",\"after\":",out)>=0);view_json(out,&e->after);CHECK(fputc('}',out)!=EOF);
    }
    CHECK(fputs("],\"io\":[",out)>=0);
    for(i=0;i<f->m->count;++i){io_event *e=&f->m->events[i];CHECK(fprintf(out,"%s{\"kind\":%u,\"ordinal\":%u,\"offset\":%" PRIu64 ",\"requested\":%" PRIu64 ",\"result\":%" PRId64 "}",
        i?",":"",e->kind,e->ordinal,e->offset,e->requested,e->result)>0);}
    CHECK(fputs("]}\n",out)>=0);CHECK(!fclose(out));free(scratch);free(reopen);
}
static void destroy(fixture *f)
{
    unsigned i;for(i=0;i<3;++i)free(f->buffers.checkpoints[i]);for(i=0;i<2;++i){free(f->buffers.arenas[i]);free(f->shadow[i]);}
    free(f->buffers.payload);free(f->context);free(f->initial);free(f->shadow_cp);free(f->backup);free(f->known_good);
    free(f->scratch);free(f->store);free(f->output);free(f->m->bytes);free(f->m);
}
int main(int argc,char **argv)
{
    fixture *f;uint8_t *source;size_t length,at=16;unsigned flush_each,payload,fail_kind,fail_nth,short_bytes;
    int rc=PFR_OK,stop_rc=PFR_OK;uint64_t original=0;
    if(argc!=14){fputs("usage: rolling-ppu-output-fixture SOURCE OUTPUT_DIR FLUSH_EACH INC DESCS STACK IDENTITIES METADATA CHECKPOINT PAYLOAD FAULT_KIND FAULT_NTH SHORT_BYTES\n",stderr);return 2;}
    source=load(argv[1],&length);f=calloc(1,sizeof *f);CHECK(f);f->directory=argv[2];flush_each=number(argv[3]);CHECK(flush_each<=1);
    f->cfg.incarnations=number(argv[4]);f->cfg.descriptors=number(argv[5]);f->cfg.stack_rows=number(argv[6]);f->cfg.frame_identities=number(argv[7]);
    f->cfg.metadata_bytes=number(argv[8]);f->cfg.checkpoint_bytes=number(argv[9]);payload=number(argv[10]);
    fail_kind=number(argv[11]);fail_nth=number(argv[12]);short_bytes=number(argv[13]);CHECK(fail_kind<=3&&(fail_kind==0||fail_nth));
    init(f,source,payload);f->m->fail_kind=fail_kind;f->m->fail_nth=fail_nth;f->m->short_bytes=short_bytes;
    while(at<length){size_t n;CHECK(length-at>=20&&r32(source+at+4)<=1048576);n=(size_t)r32(source+at+4)+20;CHECK(n<=length-at);
        rc=execute(f,0,source+at,n,original);if(rc!=PFR_OK){stop_rc=rc;break;}at+=n;++original;
        if(flush_each){rc=execute(f,1,NULL,0,original);if(rc!=PFR_OK){stop_rc=rc;break;}}
    }
    if(rc==PFO_RESOURCE){CHECK(view(f).phase==PFO_STOPPING);rc=execute(f,2,NULL,0,original);if(rc!=PFR_OK)stop_rc=rc;}
    else if(rc==PFR_OK){
        if(view(f).terminal){rc=execute(f,2,NULL,0,original);if(rc!=PFR_OK)stop_rc=rc;}
        else{rc=execute(f,1,NULL,0,original);if(rc!=PFR_OK)stop_rc=rc;}
    }
    if(view(f).phase==PFO_POISONED){unsigned calls=f->m->count;
        SEMANTIC("poison-refuses-reentry",execute(f,0,source+16,length-16,original)==PFR_FAILED);CHECK(execute(f,1,NULL,0,original)==PFR_FAILED);CHECK(execute(f,2,NULL,0,original)==PFR_FAILED);CHECK(f->m->count==calls);
    }else if(view(f).phase==PFO_FINISHED){unsigned calls=f->m->count;
        CHECK(execute(f,0,source+16,length-16,original)==PFR_END);CHECK(execute(f,1,NULL,0,original)==PFR_END);CHECK(execute(f,2,NULL,0,original)==PFR_END);CHECK(f->m->count==calls);
    }else if(rc==PFR_OK){unsigned calls=f->m->count;CHECK(execute(f,1,NULL,0,original)==PFR_OK);CHECK(f->m->count==calls);}
    artifacts(f,stop_rc);
    printf("PASS actual PPU output fixture: accepted %" PRIu64 ", committed %" PRIu64 ", stop %d, injected fault %u\n",view(f).accepted_next_chunk,view(f).committed_next_chunk,stop_rc,f->m->triggered);
    destroy(f);free(f);free(source);return 0;
}
