/* Private forward-only FIFO state. Included by roll_combined2.c; no raw prefix
 * events/accounting rows survive except the single protocol cursor. */
#define FI_CEIL (UINT64_C(1)<<63)
enum { FI_CONFIG=1,FI_EVENT=2,FI_VALID_TB=4,FI_STATUS=8,FI_SAT=16,
       FI_FINAL=32,FI_ACCOUNT=64,FI_UNASSOCIATED=128,FI_CLOCK=256 };
typedef struct {uint32_t owner,reserved;uint64_t first,last,index;} fi_run;
typedef struct {uint64_t window,rows,first,last,index;} fi_group;
typedef struct {
    uint32_t flags,run_count,group_count,reserved;
    uint64_t history,events,enter,exit,statuses,valid_tb;
    uint64_t parsed,missing,seq,frame,window,status_enter,status_exit;
    uint64_t config_index,status_index,event_chunks,status_chunks,chunks,bytes;
    uint64_t floor,retired_rows,retired_groups,retired_runs,retired_frame,unknown_windows;
    uint32_t projected,new_groups,new_runs,reserved2;
    uint8_t config[64],event[96],status[80];
} fi_state;
static int fi_inc(uint64_t *n,uint64_t by)
{if(by>UINT64_MAX-*n)return PFR_COUNTER_RANGE;*n+=by;return PFR_OK;}
static uint64_t fi_succ(uint64_t n){return n==UINT64_MAX?n:n+1;}
static int fi_touch(fi_state *s,fi_run *runs,uint32_t cap,const uint8_t *e,uint64_t index)
{
    uint32_t owner=r32(e+40),start=0,end,count;uint64_t first=r64(e+48),last=first;
    if(!(r16(e+38)&1)){s->flags|=FI_UNASSOCIATED;return PFR_OK;}
    if(s->retired_runs&&first<=s->retired_frame)return PFR_INVALID;
    while(start<s->run_count&&(runs[start].owner<owner
          ||(runs[start].owner==owner&&fi_succ(runs[start].last)<first)))++start;
    end=start;
    while(end<s->run_count&&runs[end].owner==owner&&runs[end].first<=fi_succ(last)){
        if(runs[end].first<first)first=runs[end].first;
        if(runs[end].last>last)last=runs[end].last;
        ++end;
    }
    count=s->run_count-(end-start)+1;if(count>cap)return PFR_IDENTITIES;
    memmove(runs+start+1,runs+end,(size_t)(s->run_count-end)*sizeof *runs);
    runs[start].owner=owner;runs[start].reserved=0;runs[start].first=first;runs[start].last=last;runs[start].index=index;
    if(end==start)++s->new_runs;
    s->run_count=count;return PFR_OK;
}
static int fi_config(fi_state *s,const uint8_t *p,size_t n,uint64_t hz,int owner,int domain)
{
    uint32_t records,begin,end;
    if(n!=64||(s->flags&FI_CONFIG)||r16(p)!=1||r16(p+2)!=1||!r32(p+4)||r32(p+16)!=96
       ||!r32(p+48)||r32(p+52)||r64(p+56)||r64(p+32)!=hz||!hz||!owner||!domain)return PFR_INVALID;
    records=r32(p+20);begin=r32(p+40);end=r32(p+44);
    if(records<16||records>65536||r32(p+28)!=(uint64_t)records*96+4096||r32(p+28)>r32(p+24)
       ||(begin&3)||(end&3)||begin>=end||end>=UINT32_C(0x20000000))return PFR_INVALID;
    memcpy(s->config,p,64);s->flags|=FI_CONFIG;return PFR_OK;
}
static int fi_event(fi_state *s,fi_run *runs,uint32_t cap,const uint8_t *p,uint64_t index)
{
    const uint8_t *c=s->config,*old=s->event;uint64_t seq=r64(p),wrap=r64(p+8);
    uint32_t owner=r32(c+12),begin=r32(c+40),end=r32(c+44),tail=r32(p+60),target;
    uint16_t kind=r16(p+36),flags=r16(p+38);int rc;
    if(!seq||seq>=FI_CEIL||!wrap||kind<1||kind>6||(flags&~3u)||r32(p+32)!=owner
       ||r32(p+44)||r32(p+76)||r64(p+80)||r64(p+88)||r32(p+56)!=begin
       ||(tail&3)||tail<=begin||tail>end||r32(p+68)>(end-begin)/4)return PFR_INVALID;
    if(flags&1){if(r32(p+40)!=owner)return PFR_INVALID;}
    else if(r32(p+40)||r64(p+48))return PFR_INVALID;
    target=(kind==2||kind==3)?tail:(kind==4||kind==5)?begin:0;
    if(r32(p+64)!=target||r32(p+72)!=(kind==6?begin:tail)
       ||(!(flags&2)&&r64(p+16)>r64(p+24)))return PFR_INVALID;
    if(s->flags&FI_EVENT){
        uint64_t old_seq=r64(old),old_wrap=r64(old+8);uint16_t old_kind=r16(old+36);
        if(seq<=old_seq||wrap<old_wrap||(wrap==old_wrap&&kind<=old_kind))return PFR_INVALID;
        if(seq==old_seq+1){
            if(old_kind==6){if(old_wrap==UINT64_MAX||kind!=1||wrap!=old_wrap+1)return PFR_INVALID;}
            else if(wrap!=old_wrap||kind!=old_kind+1)return PFR_INVALID;
        }
        if(wrap==old_wrap&&(tail!=r32(old+60)||r32(p+68)!=r32(old+68)
           ||(flags&1)!=(r16(old+38)&1)||r32(p+40)!=r32(old+40)||r64(p+48)!=r64(old+48)))return PFR_INVALID;
        if(!(flags&2)&&(s->flags&FI_VALID_TB)&&r64(p+16)<s->valid_tb)return PFR_INVALID;
    }
    rc=fi_touch(s,runs,cap,p,index);if(rc!=PFR_OK)return rc;
    if(fi_inc(&s->events,1)!=PFR_OK||(kind==1&&fi_inc(&s->enter,1)!=PFR_OK)
       ||(kind==6&&fi_inc(&s->exit,1)!=PFR_OK))return PFR_COUNTER_RANGE;
    if(!(flags&2)){s->valid_tb=r64(p+24);s->flags|=FI_VALID_TB;}
    memcpy(s->event,p,96);s->flags|=FI_EVENT;return PFR_OK;
}
static int fi_status(fi_state *s,const uint8_t *p,size_t n)
{
    uint16_t flags;uint64_t sum,started,completed,open;unsigned i;
    static const unsigned monotone[]={8,24,32,48,56,72};
    if(n!=80||r16(p)!=1||r32(p+4)!=r32(s->config+4))return PFR_INVALID;
    flags=r16(p+2);started=r64(p+48);completed=r64(p+56);
    if((flags&~15u)||((s->flags&FI_SAT)&&!(flags&4))||(!(flags&1)&&!(flags&2))
       ||((flags&8)&&!(flags&2))||r64(p+16)!=s->events
       ||r64(p+40)!=((s->flags&FI_EVENT)?r64(s->event):0)||r64(p+72)>r32(s->config+20))return PFR_INVALID;
    if(!(flags&4)){
        sum=r64(p+16);if(r64(p+24)>UINT64_MAX-sum)return PFR_INVALID;sum+=r64(p+24);
        if(r64(p+8)<sum||r64(p+40)>r64(p+8)||completed>started||s->enter>started||s->exit>completed)return PFR_INVALID;
        if((flags&3)==1){open=started-completed;
            if(r64(p+8)!=sum||open>1||r64(p+64)!=(open?started:0))return PFR_INVALID;
        }
    }
    if(s->flags&FI_STATUS)for(i=0;i<sizeof monotone/sizeof monotone[0];++i)
        if(r64(p+monotone[i])<r64(s->status+monotone[i]))return PFR_INVALID;
    if(fi_inc(&s->statuses,1)!=PFR_OK)return PFR_COUNTER_RANGE;
    memcpy(s->status,p,80);s->flags|=FI_STATUS;
    if(flags&4)s->flags|=FI_SAT;
    if(flags&1)s->flags|=FI_FINAL;
    s->status_enter=s->enter;s->status_exit=s->exit;return PFR_OK;
}
static int fi_account(fi_state *s,fi_group *groups,uint32_t cap,const uint8_t *p,size_t n,
                      int windowed,int declared,uint64_t first,uint64_t last,uint64_t index)
{
    uint32_t reasons;uint64_t seq,frame,window,prepared,published,wraps;fi_group *g;
    if(n!=80||r16(p)!=1||r64(p+72)||r32(p+4)!=r32(s->config+4)||r32(p+8)!=r32(s->config+48)
       ||r32(p+12)!=r32(s->config+8)||r32(p+16)!=r32(s->config+12))return PFR_INVALID;
    reasons=r32(p+20);seq=r64(p+64);frame=r64(p+24);window=r64(p+32);
    prepared=r64(p+40);published=r64(p+48);wraps=r64(p+56);
    if((reasons&~63u)||r16(p+2)!=(reasons?2:3)||!seq||seq>=FI_CEIL||seq<=s->seq
       ||frame>=FI_CEIL||((s->flags&FI_ACCOUNT)&&frame<=s->frame)
       ||(reasons&&(prepared||published||wraps))||(prepared&3)||(published&3)
       ||wraps>prepared/4||wraps>published/4)return PFR_INVALID;
    if(windowed){if(!declared||window<s->window||frame<first||frame>last)return PFR_INVALID;}
    else if(window)return PFR_INVALID;
    if(declared==2&&fi_inc(&s->unknown_windows,1)!=PFR_OK)return PFR_COUNTER_RANGE;
    if(s->group_count&&groups[s->group_count-1].window==window){
        g=&groups[s->group_count-1];if(fi_inc(&g->rows,1)!=PFR_OK)return PFR_COUNTER_RANGE;g->last=frame;
    }else{
        if(s->group_count>=cap)return PFR_IDENTITIES;
        g=&groups[s->group_count++];g->window=window;g->rows=1;g->first=g->last=frame;++s->new_groups;
    }
    g->index=index;
    if(fi_inc(&s->parsed,1)!=PFR_OK||fi_inc(&s->missing,seq-s->seq-1)!=PFR_OK)return PFR_COUNTER_RANGE;
    s->seq=seq;s->frame=frame;s->window=window;s->flags|=FI_ACCOUNT;return PFR_OK;
}
static int fi_accept(fi_state *s,fi_run *runs,uint32_t run_cap,fi_group *groups,uint32_t group_cap,
    uint32_t ty,const uint8_t *p,size_t n,uint64_t index,uint64_t hz,int owner,int domain,
    int windowed,int declared_window,uint64_t first,uint64_t last)
{
    int rc;uint32_t count=0,i;uint64_t charge=1;
    if((s->flags&FI_FINAL)||(ty!=31&&!(s->flags&FI_CONFIG)))return PFR_INVALID;
    switch(ty){
    case 31:
        rc=fi_config(s,p,n,hz,owner,domain);if(rc!=PFR_OK)return rc;s->config_index=index;break;
    case 32:
        if(n<16||r16(p)!=1||r16(p+2)||r32(p+4)!=r32(s->config+4)||r32(p+12))return PFR_INVALID;
        count=r32(p+8);if(!count||(uint64_t)count*96+16!=n)return PFR_INVALID;
        for(i=0;i<count;++i){rc=fi_event(s,runs,run_cap,p+16+(size_t)i*96,index);if(rc!=PFR_OK)return rc;}
        if(fi_inc(&s->event_chunks,1)!=PFR_OK)return PFR_COUNTER_RANGE;
        charge+=(uint64_t)count;break;
    case 33:
        rc=fi_status(s,p,n);if(rc!=PFR_OK)return rc;
        s->status_index=index;s->status_chunks=s->event_chunks;break;
    case 43:
        rc=fi_account(s,groups,group_cap,p,n,windowed,declared_window,first,last,index);if(rc!=PFR_OK)return rc;break;
    default:return PFR_INVALID;
    }
    if(fi_inc(&s->history,charge)!=PFR_OK||fi_inc(&s->chunks,1)!=PFR_OK
       ||fi_inc(&s->bytes,(uint64_t)n+20)!=PFR_OK)return PFR_COUNTER_RANGE;
    return PFR_OK;
}
static int fi_close(fi_state *s,uint64_t window,uint64_t last)
{
    if((s->flags&FI_ACCOUNT)&&s->window==window&&s->frame>last)return PFR_INVALID;
    return fi_inc(&s->history,1);
}
static int fi_project(fi_state *s,fi_run *runs,fi_group *groups,uint64_t floor)
{
    uint32_t i,out=0;uint64_t frame=r64(s->event+48);uint32_t owner=r32(s->event+40);
    if(!s->projected||floor<s->floor)return PFR_INVALID;
    for(i=0;i<s->group_count;++i){
        if(i+1<s->group_count&&groups[i].index<floor){
            if(fi_inc(&s->retired_rows,groups[i].rows)||fi_inc(&s->retired_groups,1))return PFR_COUNTER_RANGE;
        }else groups[out++]=groups[i];
    }
    s->group_count=out;out=0;
    for(i=0;i<s->run_count;++i){
        int pin=(s->flags&FI_EVENT)&&(r16(s->event+38)&1)&&runs[i].owner==owner&&runs[i].first<=frame&&frame<=runs[i].last;
        if(runs[i].index<floor&&!pin){
            if(fi_inc(&s->retired_runs,1))return PFR_COUNTER_RANGE;
            if(runs[i].last>s->retired_frame)s->retired_frame=runs[i].last;
        }else runs[out++]=runs[i];
    }
    s->run_count=out;s->floor=floor;s->new_groups=0;s->new_runs=0;return PFR_OK;
}
static size_t fi_wire_size(const fi_state *s)
{return 164u+((s->flags&FI_CONFIG)?64u:0u)+((s->flags&FI_EVENT)?96u:0u)
    +((s->flags&FI_STATUS)?80u:0u)+(size_t)s->run_count*24;}
static size_t fi_encode(const fi_state *s,const fi_run *runs,uint64_t next,uint64_t frequency,int windowed,uint8_t *b)
{
    size_t n=fi_wire_size(s),at=160;uint32_t i;memset(b,0,n);
    memcpy(b,"PFRFIFO1",8);w32(b+8,1);w32(b+12,64);w64(b+16,n);w64(b+24,next);w64(b+32,s->history);
    w32(b+40,s->flags|(frequency?FI_CLOCK:0));w32(b+44,s->run_count);w32(b+48,(uint32_t)windowed);w64(b+56,frequency);
    w64(b+64,s->events);w64(b+72,s->enter);w64(b+80,s->exit);w64(b+88,s->statuses);w64(b+96,s->valid_tb);
    w64(b+104,s->parsed);w64(b+112,s->missing);w64(b+120,s->seq);w64(b+128,s->frame);w64(b+136,s->window);
    w64(b+144,s->status_enter);w64(b+152,s->status_exit);
    if(s->flags&FI_CONFIG){memcpy(b+at,s->config,64);at+=64;}
    if(s->flags&FI_EVENT){memcpy(b+at,s->event,96);at+=96;}
    if(s->flags&FI_STATUS){memcpy(b+at,s->status,80);at+=80;}
    for(i=0;i<s->run_count;++i){w32(b+at,runs[i].owner);w64(b+at+8,runs[i].first);w64(b+at+16,runs[i].last);at+=24;}
    w32(b+n-4,crc(b,n-4));return n;
}
