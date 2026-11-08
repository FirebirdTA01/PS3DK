/* Fixture replay, not a protocol implementation. Expected bytes never come
 * from the candidate codec. The same assertions grade both RED and GREEN. */
#include "gdbstub.h"
#include "fnv1a.h"
#include "red_data.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define BUFFER 65536u
typedef struct {
  const step_t *step;
  unsigned call, script;
  int failed, read_mode;
  unsigned char rx[BUFFER], tx[BUFFER];
  size_t nrx, pos, ntx;
} replay;
static const char *active;
static unsigned assertions;
#define TEST(r,x) do { ++assertions; if (!(x)) { if (!(r)->failed) fprintf(stderr,"%s:%d: %s\n",active,__LINE__,#x); (r)->failed=1; } } while (0)
static unsigned nibble(char c) {
  if(c>='0'&&c<='9')return (unsigned)(c-'0');
  if(c>='A'&&c<='F')return (unsigned)(c-'A'+10);
  if(c>='a'&&c<='f')return (unsigned)(c-'a'+10);
  fprintf(stderr,"invalid fixture hex\n");exit(2);
}
static size_t unhex(const char *s,unsigned char *out,size_t cap) {
  size_t n=s?strlen(s):0;
  if(n%2 || n/2>cap){fprintf(stderr,"fixture hex capacity\n");exit(2);}
  for(size_t i=0;i<n/2;++i)out[i]=(unsigned char)((nibble(s[2*i])<<4)|nibble(s[2*i+1]));
  return n/2;
}
static uint64_t hexnum(const char *s) {
  uint64_t n=0;
  for(;s&&*s;++s){unsigned d=nibble(*s);if(n>(UINT64_MAX-d)/16){fprintf(stderr,"fixture integer overflow\n");exit(2);}n=n*16+d;}
  return n;
}
static gdbstub_status status(const char *s) {
  if(!s || !strcmp(s,"ok"))return GDBSTUB_OK;
  if(!strcmp(s,"error"))return GDBSTUB_ERROR;
  if(!strcmp(s,"limit"))return GDBSTUB_LIMIT;
  if(!strcmp(s,"frame"))return GDBSTUB_FRAME;
  if(!strcmp(s,"bad_state"))return GDBSTUB_BAD_STATE;
  fprintf(stderr,"unknown fixture status %s\n",s);exit(2);
}
static gdbstub_state state(const char *s) {
  const char *names[]={"READY","TX","WAIT_ACK","RUNNING","DETACH_ACK","CLOSED"};
  for(unsigned i=0;i<6;++i)if(!strcmp(s,names[i]))return (gdbstub_state)i;
  fprintf(stderr,"unknown fixture state\n");exit(2);
}
static const call_expected_t *call(replay *r,const char *op) {
  static const call_expected_t missing={.status="error"};
  unsigned i=r->call++;
  TEST(r,i<(unsigned)r->step->n_calls);
  if(i>=(unsigned)r->step->n_calls)return &missing;
  const call_expected_t *e=&r->step->calls[i];
  TEST(r,!strcmp(e->op,op));return e;
}
static void bytes_equal(replay *r,const unsigned char *data,size_t n,const char *hex) {
  unsigned char expected[BUFFER];size_t en=unhex(hex,expected,sizeof expected);
  TEST(r,n==en);TEST(r,!n||data!=NULL);
  if(n==en && (!n||data))TEST(r,!n||!memcmp(data,expected,n));
}
static void result_data(replay *r,const call_expected_t *e,unsigned char *out,size_t cap,size_t *len) {
  unsigned char bytes[BUFFER];size_t n=unhex(e->res_data_hex,bytes,sizeof bytes);
  TEST(r,len!=NULL);TEST(r,!cap||out!=NULL);
  if(len)*len=n; /* Actual length even if invalid; don't turn it into ERROR. */
  if(out)memcpy(out,bytes,n<cap?n:cap);
}
static void selector(replay *r,gdbstub_thread got,int kind,uint64_t id) {
  TEST(r,got.kind==kind);if(kind==GDBSTUB_THREAD_ID)TEST(r,got.id==id);
}
static gdbstub_status stop_reason(void *ctx,uint8_t *sig,uint64_t *id,int *has) {
  replay *r=ctx;const call_expected_t *e=call(r,"stop_reason");
  *sig=(uint8_t)e->result_signal;*has=e->result_has_thread;*id=hexnum(e->result_thread);return status(e->status);
}
static gdbstub_status read_regs(void *ctx,uint8_t *out,size_t cap,size_t *n) {
  replay *r=ctx;const call_expected_t *e=call(r,"read_regs");result_data(r,e,out,cap,n);return status(e->status);
}
static gdbstub_status write_regs(void *ctx,const uint8_t *in,size_t n) {
  replay *r=ctx;const call_expected_t *e=call(r,"write_regs");bytes_equal(r,in,n,e->data_hex);return status(e->status);
}
static gdbstub_status read_reg(void *ctx,unsigned reg,uint8_t *out,size_t cap,size_t *n) {
  replay *r=ctx;const call_expected_t *e=call(r,"read_reg");TEST(r,e->has_reg&&reg==e->reg);result_data(r,e,out,cap,n);return status(e->status);
}
static gdbstub_status write_reg(void *ctx,unsigned reg,const uint8_t *in,size_t n) {
  replay *r=ctx;const call_expected_t *e=call(r,"write_reg");TEST(r,e->has_reg&&reg==e->reg);bytes_equal(r,in,n,e->data_hex);return status(e->status);
}
static gdbstub_status read_mem(void *ctx,uint64_t addr,size_t len,uint8_t *out,size_t *n) {
  replay *r=ctx;const call_expected_t *e=call(r,"read_mem");TEST(r,e->has_addr&&addr==e->addr);TEST(r,e->has_length&&len==e->length);
  result_data(r,e,out,len,n);return status(e->status);
}
static gdbstub_status write_mem(void *ctx,uint64_t addr,const uint8_t *in,size_t n) {
  replay *r=ctx;const call_expected_t *e=call(r,"write_mem");TEST(r,e->has_addr&&addr==e->addr);bytes_equal(r,in,n,e->data_hex);return status(e->status);
}
static gdbstub_status threads(void *ctx,int first,uint64_t *ids,size_t cap,size_t *n) {
  replay *r=ctx;const call_expected_t *e=call(r,"threads");TEST(r,e->has_first&&first==e->first);
  *n=(size_t)e->n_res_ids;for(size_t i=0;i<*n&&i<cap;++i)ids[i]=e->res_ids[i];return status(e->status);
}
static gdbstub_status current_thread(void *ctx,uint64_t *id) {
  replay *r=ctx;const call_expected_t *e=call(r,"current_thread");*id=hexnum(e->result_id);return status(e->status);
}
static gdbstub_status select_thread(void *ctx,char which,gdbstub_thread t) {
  replay *r=ctx;const call_expected_t *e=call(r,"select_thread");TEST(r,which==e->which);selector(r,t,e->thread_kind,e->thread_id);return status(e->status);
}
static gdbstub_status breakpoint(void *ctx,int insert,int type,uint64_t addr,uint64_t kind) {
  replay *r=ctx;const call_expected_t *e=call(r,"breakpoint");TEST(r,e->has_insert&&insert==e->insert);TEST(r,type==e->type);
  TEST(r,e->has_addr&&addr==e->addr);TEST(r,e->has_kind&&kind==e->kind);return status(e->status);
}
static gdbstub_status resume(void *ctx,char action,uint64_t addr,int has) {
  replay *r=ctx;const call_expected_t *e=call(r,"resume");TEST(r,action==e->action);TEST(r,has==e->has_addr);
  if(has)TEST(r,addr==e->addr);
  return status(e->status);
}
static gdbstub_status vcont(void *ctx,const gdbstub_action *actions,size_t n) {
  replay *r=ctx;const call_expected_t *e=call(r,"vcont");TEST(r,n==(size_t)e->n_actions);
  for(size_t i=0;i<n&&i<(size_t)e->n_actions;++i){TEST(r,actions[i].action==e->actions[i].action);selector(r,actions[i].thread,e->actions[i].thread.kind,e->actions[i].thread.id);}
  return status(e->status);
}
static gdbstub_status detach(void *ctx) {replay *r=ctx;return status(call(r,"detach")->status);}
static gdbstub_status kill_target(void *ctx) {replay *r=ctx;return status(call(r,"kill")->status);}
static gdbstub_io_result read_io(void *ctx,uint8_t *out,size_t cap) {
  replay *r=ctx;
  if(r->read_mode)return (gdbstub_io_result){r->read_mode==OP_EOF?GDBSTUB_EOF:GDBSTUB_AGAIN,0};
  size_t n=r->nrx-r->pos;if(n>cap)n=cap;
  if(!n)return (gdbstub_io_result){GDBSTUB_AGAIN,0};
  memcpy(out,r->rx+r->pos,n);r->pos+=n;return (gdbstub_io_result){GDBSTUB_OK,n};
}
static gdbstub_io_result write_io(void *ctx,const uint8_t *data,size_t len) {
  replay *r=ctx;gdbstub_io_result io={GDBSTUB_OK,len};
  if(r->script<(unsigned)r->step->n_wscript){const wr_entry_t *w=&r->step->wscript[r->script++];
    io.status=w->status=='a'?GDBSTUB_AGAIN:w->status=='e'?GDBSTUB_ERROR:GDBSTUB_OK;io.count=(size_t)w->count;}
  /* Invalid scripted counts are returned faithfully but never copied out of
   * candidate buffers. Only valid positive OK bytes count as transmitted. */
  if(io.status==GDBSTUB_OK&&io.count&&io.count<=len){
    TEST(r,io.count<=sizeof r->tx-r->ntx);
    if(io.count<=sizeof r->tx-r->ntx){memcpy(r->tx+r->ntx,data,io.count);r->ntx+=io.count;}}
  return io;
}
static void queue(replay *r,const unsigned char *data,size_t n) {
  if(r->pos){memmove(r->rx,r->rx+r->pos,r->nrx-r->pos);r->nrx-=r->pos;r->pos=0;}
  TEST(r,n<=sizeof r->rx-r->nrx);if(n<=sizeof r->rx-r->nrx){memcpy(r->rx+r->nrx,data,n);r->nrx+=n;}
}
/* split_step=-1: literal fixture. Otherwise split one positive rx event at
 * split bytes, or byte-at-a-time for split=0. Aggregate that event's oracle. */
static int scenario(const scenario_t *sc,int split_step,size_t split) {
  replay *r=calloc(1,sizeof *r);if(!r){perror("replay");exit(2);}active=sc->id;
  gdbstub_backend b={0};b.ctx=r;
  for(int i=0;i<sc->n_enabled;++i){const char *op=sc->enabled_ops[i];
#define ENABLE(name,fn) if(!strcmp(op,#name)){b.name=fn;continue;}
    ENABLE(stop_reason,stop_reason) ENABLE(read_regs,read_regs) ENABLE(write_regs,write_regs)
    ENABLE(read_reg,read_reg) ENABLE(write_reg,write_reg) ENABLE(read_mem,read_mem) ENABLE(write_mem,write_mem)
    ENABLE(threads,threads) ENABLE(current_thread,current_thread) ENABLE(select_thread,select_thread)
    ENABLE(breakpoint,breakpoint) ENABLE(resume,resume) ENABLE(vcont,vcont) ENABLE(detach,detach) ENABLE(kill,kill_target)
#undef ENABLE
    fprintf(stderr,"unknown enabled callback %s\n",op);exit(2);
  }
  gdbstub_transport transport={r,read_io,write_io};
  gdbstub_config cfg={sc->wire,sc->decoded,sc->response_wire,sc->max_actions,sc->deadline_ms};
  gdbstub_session *s=gdbstub_session_create(&cfg,&b,&transport);TEST(r,s!=NULL);
  uint64_t now=0;
  for(int i=0;s&&i<sc->n_steps;++i){const step_t *st=&sc->steps[i];r->step=st;r->call=r->script=0;r->ntx=0;r->read_mode=0;
    gdbstub_event ev={GDBSTUB_EVT_PUMP,0,0,0};
    if(st->op==OP_TICK)now+=(uint64_t)sc->deadline_ms+1;
    if(st->op==OP_AGAIN||st->op==OP_EOF)r->read_mode=st->op;
    if(st->op==OP_STOP){ev.kind=GDBSTUB_EVT_STOP;ev.signal=(uint8_t)st->signal;ev.has_thread=st->has_stop;ev.thread=st->stop_thread;}
    gdbstub_status rc=GDBSTUB_NIMPL;
    if(st->op==OP_RX){unsigned char bytes[BUFFER];size_t n=unhex(st->hex,bytes,sizeof bytes);
      if(i==split_step){size_t pos=0;while(pos<n){size_t part=split?(pos? n-pos:split):1;queue(r,bytes+pos,part);pos+=part;rc=gdbstub_session_step(s,&ev,now);}}
      else {queue(r,bytes,n);rc=gdbstub_session_step(s,&ev,now);}
    }else rc=gdbstub_session_step(s,&ev,now);
    bytes_equal(r,r->tx,r->ntx,st->expect_tx);TEST(r,gdbstub_session_state(s)==state(st->expect_state));
    TEST(r,r->call==(unsigned)st->n_calls);
    /* Absent expect_api/expect_reason means unchecked, not OK/NULL. */
    if(st->expect_api)TEST(r,rc==status(st->expect_api));
    if(st->expect_reason){const char *got=gdbstub_session_reason(s);TEST(r,got&&!strcmp(got,st->expect_reason));}
  }
  gdbstub_session_free(s);int pass=!r->failed;free(r);return pass;
}
static int input_matches(const char *path,uint64_t want,size_t len) {
  FILE *f=fopen(path,"rb");if(!f){perror(path);return 0;}
  unsigned char *p=malloc(len+1);if(!p){fclose(f);return 0;}
  size_t n=fread(p,1,len+1,f);int ok=n==len&&!ferror(f)&&rdac_fnv1a64(p,n)==want;
  if(fclose(f))ok=0;
  free(p);return ok;
}
int main(int argc,char **argv) {
  if(argc!=3){fprintf(stderr,"usage: red_test fixtures.json register-layout.json\n");return 2;}
  if(!input_matches(argv[1],RED_FIXTURES_FNV1A,RED_FIXTURES_LEN)||!input_matches(argv[2],RED_REGISTRY_FNV1A,RED_REGISTRY_LEN)){
    fprintf(stderr,"input mismatch (FNV-1a64 + length; SHA256 provenance verified separately)\n");return 2;}
  unsigned lp=0,ln=0,cp=0,sp=0,xp=0,xn=0;unsigned total=0;
#define LAYOUT(x) do{++ln;if(x)++lp;else fprintf(stderr,"layout: %s\n",#x);}while(0)
  LAYOUT(gdbstub_reg_count()==red_layout.count);LAYOUT(gdbstub_reg_total_bytes()==red_layout.total_bytes);
  LAYOUT(gdbstub_reg_total_hex()==red_layout.total_hex);
  for(unsigned i=0;i<red_layout.count;++i){unsigned n=gdbstub_reg_bytes(i);LAYOUT(n==red_layout.widths[i]);total+=n;}
  LAYOUT(total==red_layout.total_bytes);LAYOUT(gdbstub_reg_bytes(red_layout.count)==0);
  for(int i=0;i<red_n_codecs;++i){const codec_case_t *c=&red_codecs[i];replay r={0};active=c->id;
    unsigned char in[BUFFER],out[BUFFER];size_t n=unhex(c->input_hex,in,sizeof in),got=SIZE_MAX;
    memset(out,0xa5,sizeof out);if(c->capacity>sizeof out-16){fprintf(stderr,"codec capacity\n");return 2;}
    gdbstub_status rc=!strcmp(c->op,"encode")?gdbstub_codec_encode(in,n,out,c->capacity,&got):gdbstub_codec_decode(in,n,out,c->capacity,&got);
    TEST(&r,rc==status(c->status));TEST(&r,got<=c->capacity);if(got<=c->capacity)bytes_equal(&r,out,got,c->expect_hex);
    for(unsigned j=c->capacity;j<c->capacity+16;++j)TEST(&r,out[j]==0xa5);
    cp+=!r.failed;printf("codec %s %s\n",c->id,r.failed?"FAIL":"PASS");
  }
  for(int i=0;i<red_n_scenarios;++i){int pass=scenario(red_scenarios[i],-1,0);sp+=(unsigned)pass;printf("scenario %s %s\n",red_scenarios[i]->id,pass?"PASS":"FAIL");}
  for(int i=0;i<red_n_scenarios;++i){const scenario_t *sc=red_scenarios[i];
    for(int j=0;j<sc->n_steps;++j){const step_t *st=&sc->steps[j];
      /* Split only ordinary positive requests: no per-step write script,
       * starts with '$', expects '+', and does not finish in TX or CLOSED. */
      if(st->op!=OP_RX||st->n_wscript||!st->hex||strncmp(st->hex,"24",2)||strncmp(st->expect_tx,"2B",2)||!strcmp(st->expect_state,"TX")||!strcmp(st->expect_state,"CLOSED"))continue;
      size_t n=strlen(st->hex)/2;
      for(size_t cut=0;cut<n;++cut){++xn;xp+=(unsigned)scenario(sc,j,cut);}
    }
  }
  printf("layout tests=%u pass=%u fail=%u\n",ln,lp,ln-lp);
  printf("codec tests=%d pass=%u fail=%u\n",red_n_codecs,cp,(unsigned)red_n_codecs-cp);
  printf("scenario tests=%d pass=%u fail=%u\n",red_n_scenarios,sp,(unsigned)red_n_scenarios-sp);
  printf("split tests=%u pass=%u fail=%u\n",xn,xp,xn-xp);
  printf("assertions=%u\n",assertions);
  return lp==ln&&cp==(unsigned)red_n_codecs&&sp==(unsigned)red_n_scenarios&&xp==xn?0:1;
}
