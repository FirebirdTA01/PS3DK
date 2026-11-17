/* Private bounded AUTO forward state. Included once by roll_combined2.c.
 * No historical OPS replay, heap allocation, producer MAIN pool or pointers in
 * accepted state. Exact negotiated legacy/ring formats share one typed state. */
typedef struct {
    uint32_t owner,width;uint64_t frame,records[3],abandoned,call_excluded,index;
    uint8_t raw[528];
} au_build;
typedef struct {uint64_t index;uint8_t raw[96];} au_clock;
typedef struct {
    uint32_t config_bytes,status_bytes,final_count,touched_count,build_count,clock_count;
    uint32_t flags,last_owner,last_anchor,reserved;
    uint64_t config_index,status_index,last_op,max_snapshot,last_frame,last_lost,last_coverage;
    uint64_t ops,op_chunks,frames,clocks,statuses,bytes,chunks,history,status_chunks,snapshot[5];
    uint8_t config[216],status[208],ring[96],ring_snapshot[96];
} au_state;
typedef struct {fi_run *finals,*touched;au_build *builds;au_clock *clocks;} au_pools;
enum {AU_CONFIG=1,AU_STATUS=2,AU_FRAME=4,AU_FINAL=8,AU_INVALID=16,AU_UNASSOCIATED=32};
static int au_inc(uint64_t *n,uint64_t by){return plus64(n,by)?PFR_OK:PFR_COUNTER_RANGE;}
static int au_zero(const uint8_t *p,size_t n){while(n--)if(*p++)return 0;return 1;}
static int au_ringed(const au_state *s){return s->config_bytes==216;}
static uint32_t au_mask(const au_state *s){return r16(s->config+2)?0:r32(s->config+24);}
static int au_label(uint32_t n){return n==64||(n>=69&&n<=254);}
static uint32_t au_width(const au_state *s){return s->config_bytes?r32(s->config+60):0;}
static int au_run(fi_run *runs,uint32_t *count,uint32_t cap,uint32_t owner,uint64_t frame,int unique)
{
    uint32_t start=0,end,n;uint64_t first=frame,last=frame;
    while(start<*count&&(runs[start].owner<owner||(runs[start].owner==owner&&fi_succ(runs[start].last)<frame)))++start;
    end=start;
    while(end<*count&&runs[end].owner==owner&&runs[end].first<=fi_succ(last)){
        if(unique&&runs[end].first<=frame&&frame<=runs[end].last)return PFR_INVALID;
        if(runs[end].first<first)first=runs[end].first;
        if(runs[end].last>last)last=runs[end].last;
        ++end;
    }
    n=*count-(end-start)+1;if(n>cap)return PFR_IDENTITIES;
    memmove(runs+start+1,runs+end,(size_t)(*count-end)*sizeof *runs);
    runs[start].owner=owner;runs[start].reserved=0;runs[start].first=first;runs[start].last=last;*count=n;return PFR_OK;
}
static int au_entry(au_state *s,au_build *rows,uint32_t cap,uint32_t owner,uint64_t frame,au_build **out)
{
    uint32_t i=0;
    while(i<s->build_count&&(rows[i].owner<owner||(rows[i].owner==owner&&rows[i].frame<frame)))++i;
    if(i==s->build_count||rows[i].owner!=owner||rows[i].frame!=frame){
        if(s->build_count>=cap)return PFR_IDENTITIES;
        memmove(rows+i+1,rows+i,(size_t)(s->build_count-i)*sizeof *rows);memset(rows+i,0,sizeof *rows);
        rows[i].owner=owner;rows[i].frame=frame;++s->build_count;
    }
    *out=rows+i;return PFR_OK;
}
static int au_config(au_state *s,const uint8_t *p,size_t n,uint64_t hz,int owner,int domain)
{
    uint32_t mask,pairs,first,count,width;int ring;
    if((n!=160&&n!=176&&n!=216)||(s->flags&AU_CONFIG)||r16(p)!=1||r32(p+4)!=1||!hz||r64(p+64)!=hz)return PFR_INVALID;
    if(r16(p+2)==1){
        if(n==216||r32(p+24)<1||r32(p+24)>10||!au_zero(p+8,16)||!au_zero(p+32,32)
           ||!au_zero(p+72,24)||(n==176&&!au_zero(p+160,16)))return PFR_INVALID;
        memcpy(s->config,p,n);s->config_bytes=(uint32_t)n;s->flags|=AU_CONFIG;return PFR_OK;
    }
    if(r16(p+2)||!r32(p+8)||!r32(p+12)||!r32(p+16)||!r32(p+20)||!owner||!domain)return PFR_INVALID;
    mask=r32(p+24);ring=n==216;
    if(mask!=7&&mask!=15&&mask!=31&&mask!=63&&mask!=127&&mask!=255&&mask!=511&&mask!=1023&&mask!=2047)return PFR_INVALID;
    width=mask==2047?176:mask==1023?160:mask>=63?144:128;
    if(r32(p+28)!=(mask==7?15u:11u)||r32(p+60)!=width
       ||n!=(mask==2047?216u:mask==1023?176u:160u))return PFR_INVALID;
    pairs=r32(p+48);first=r32(p+32);count=r32(p+36);
    if(!pairs||pairs>504||(uint64_t)count!=16+2*(uint64_t)pairs||(uint64_t)first+count>2048
       ||!au_label(r32(p+40))||!au_label(r32(p+44))||r32(p+40)==r32(p+44)
       ||!r32(p+52)||r32(p+52)>(ring?65536u:1024u)||!r32(p+56)||r32(p+56)>256
       ||!r64(p+80)||r64(p+80)>(UINT64_C(2)<<20)||!r64(p+72)||r64(p+72)>r64(p+80)
       ||r32(p+88)!=3||r32(p+92)!=1)return PFR_INVALID;
    if(ring){
        uint32_t bytes=r32(p+176),blocks=r32(p+180),legacy=r32(p+208);
        if(r64(p+168)||!bytes||bytes>(UINT32_C(256)<<20)||(bytes%1048576)||blocks<2||blocks>4
           ||r32(p+184)!=126||r32(p+196)!=4096||r32(p+200)!=bytes/4096||!r32(p+204)||r32(p+212)!=1
           ||!legacy||(uint64_t)legacy+(uint64_t)blocks*126!=pairs
           ||(uint64_t)r32(p+188)+(uint64_t)blocks*252!=(uint64_t)first+count
           ||!au_label(r32(p+192))||r32(p+192)==r32(p+40)||r32(p+192)==r32(p+44))return PFR_INVALID;
    }
    if(n>=176){
        uint32_t legacy=ring?r32(p+208):pairs,call=legacy>=128?32:legacy/4;
        uint32_t range=legacy>=128?16:legacy/8,max=legacy>call+64?legacy-call-64:0;
        if(r64(p+168))return PFR_INVALID;
        if(r32(p+160)!=call||(r32(p+164)!=range&&r32(p+164)>max))return PFR_INVALID;
    }
    memcpy(s->config,p,n);s->config_bytes=(uint32_t)n;s->flags|=AU_CONFIG;return PFR_OK;
}
static int au_ring_record(au_state *s,const uint8_t *p)
{
    uint8_t *r=s->ring;const uint8_t *cfg=s->config;
    uint32_t g=r32(p+160),slot=r32(p+164),ord=r16(p+168),local=r16(p+170),state=r32(p+172);
    uint32_t old=r32(r+48),base;uint16_t flags=r16(p+14);uint64_t count;
    if(r16(p+12)<1||r16(p+12)>3||(flags&64)||!g||g>UINT32_MAX/2-1
       ||slot!=(g-1)%r32(cfg+200)||local!=(g-1)%r32(cfg+180)||ord>=126||state>2||((flags&2)&&state==2))return PFR_INVALID;
    base=r32(cfg+188)+252*local+2*ord;
    if(r32(p+96)!=base||r32(p+100)!=base+1||r32(p+104)!=2*g-1||r32(p+108)!=2*g)return PFR_INVALID;
    if(old==g){
        if(ord!=r32(r+52)+1||r32(r+56)!=(flags&1)||r32(r+60)!=r32(p+8)||r64(r+64)!=r64(p+16))return PFR_INVALID;
    }else{
        if(g!=old+1||ord)return PFR_INVALID;
        if(!old)w32(r+8,g);
        w32(r+12,g);count=r64(r+16);if(au_inc(&count,1))return PFR_COUNTER_RANGE;w64(r+16,count);w32(r+72,0);
    }
    if(flags&2){
        if(r32(r+72)&&r32(r+72)!=state+1)return PFR_INVALID;
        if(!r32(r+72)){count=r64(r+32+8*state);if(au_inc(&count,1))return PFR_COUNTER_RANGE;w64(r+32+8*state,count);w32(r+72,state+1);}
    }
    count=r64(r+24);if(au_inc(&count,1))return PFR_COUNTER_RANGE;w64(r+24,count);
    w32(r+48,g);w32(r+52,ord);w32(r+56,flags&1);w32(r+60,r32(p+8));w64(r+64,r64(p+16));w32(r+76,1);
    if(r64(p+120)>r64(r))w64(r,r64(p+120));
    return PFR_OK;
}
static int au_op(au_state *s,au_pools pools,const pfm2_config *cfg,const uint8_t *p)
{
    const uint8_t *c=s->config;uint16_t kind=r16(p+12),flags=r16(p+14);uint64_t id=r64(p),frame=r64(p+16),published=r64(p+120);
    uint32_t owner=r32(p+8),first=r32(c+32)+16,end=r32(c+32)+r32(c+36),build_owner=0,build_flags=0,depth=0,j,mask=au_mask(s);
    uint64_t build_frame=0;int ring=au_ringed(s),segment=(flags&64)!=0,valid=(flags&2)!=0,rc;au_build *entry;
    if(id<=s->last_op||(flags&~(ring?255u:mask!=7?127u:63u))||kind<1
       ||(kind>3&&!(kind==4&&(mask&8))&&!(kind==5&&(mask&128))&&!(kind==6&&(mask&512))))return PFR_INVALID;
    if(au_width(s)>=144){build_frame=r64(p+128);build_owner=r32(p+136);build_flags=r16(p+140);
        if((build_flags&~1u)||r16(p+142))return PFR_INVALID;
        if(!segment&&kind!=5&&(build_flags||build_owner||build_frame))return PFR_INVALID;
        if((build_flags&&build_owner!=r32(c+16))||(!build_flags&&(build_owner||build_frame))
           ||(build_flags&&(flags&1)&&owner==build_owner&&build_frame>frame))return PFR_INVALID;
    }
    if(au_width(s)>=160){depth=r32(p+152);if(r32(p+156)||(depth!=0)!=(kind==6))return PFR_INVALID;}
    if(ring){
        end=first+2*r32(c+208);
        if(flags&128){rc=au_ring_record(s,p);if(rc)return rc;}
        else if(!au_zero(p+160,16))return PFR_INVALID;
    }
    if(segment){if(!r32(p+92)||(!r32(p+116)&&!(flags&4)))return PFR_INVALID;}
    else if(r32(p+92)||r32(p+116))return PFR_INVALID;
    if(kind==4&&(segment||!(flags&1)||r32(p+64)<1||r32(p+64)>2))return PFR_INVALID;
    if(kind==5&&(segment||!(flags&1)||!r32(p+64)||r32(p+68)>r32(p+64)||!r32(p+72)||!r32(p+76)
       ||r32(p+80)>r32(p+68)||(build_flags!=0)!=(r32(p+80)!=0)))return PFR_INVALID;
    if(kind==6&&(segment||build_flags||!(flags&1)||!r32(p+64)||r32(p+68)>1||depth<1||depth>4))return PFR_INVALID;
    if(valid==((flags&4)!=0)||(valid&&(flags&48))||((flags&32)&&published)||published>UINT32_MAX
       ||((flags&1)?owner!=r32(c+16):(owner!=0||frame!=0))
       ||(r64(p+32)<r64(p+24)&&!(flags&8))||r32(p+112)!=r32(c+20)||!r32(p+64))return PFR_INVALID;
    for(j=1;j<7;++j){int used=kind==1?(j==1||j==2||j==5):kind==2?(j==2||j==3||j==4||j==5):
        kind==3?j==6:kind==5?(j>=1&&j<=4):kind==6?j==1:0;
        if(!used&&r32(p+64+4*j))return PFR_INVALID;
    }
    if(!(flags&128)&&(r32(p+96)<first||r32(p+96)>=end||r32(p+100)<first||r32(p+100)>=end||r32(p+96)==r32(p+100)))return PFR_INVALID;
    if(!r32(p+104)||r32(p+104)==UINT32_MAX||r32(p+108)!=r32(p+104)+1)return PFR_INVALID;
    if(valid){if(r64(p+40)>r64(p+48)||published<r32(p+108))return PFR_INVALID;}
    else if(r64(p+40)||r64(p+48))return PFR_INVALID;
    if(flags&1){rc=au_run(pools.touched,&s->touched_count,cfg->auto_touched_runs,owner,frame,0);if(rc)return rc;}
    else s->flags|=AU_UNASSOCIATED;
    if(kind<=3){
        if(segment){owner=build_owner;frame=build_frame;}
        if((segment&&build_flags)||(!segment&&(flags&1))){
            rc=au_entry(s,pools.builds,cfg->auto_build_entries,owner,frame,&entry);if(rc)return rc;
            if(au_inc(&entry->records[kind-1],1)||(segment&&!r32(p+116)&&au_inc(&entry->abandoned,1)))return PFR_COUNTER_RANGE;
        }
    }
    if(kind==5&&valid&&!(flags&8)&&build_flags){
        rc=au_entry(s,pools.builds,cfg->auto_build_entries,build_owner,build_frame,&entry);if(rc)return rc;
        if(!plus64(&entry->call_excluded,r32(p+80)))entry->call_excluded=UINT64_MAX;
    }
    if(au_inc(&s->ops,1))return PFR_COUNTER_RANGE;
    s->last_op=id;if(!(flags&128)&&published>s->max_snapshot)s->max_snapshot=published;return PFR_OK;
}
static int au_coverage(au_state *s,au_pools pools,const pfm2_config *cfg,const uint8_t *p,size_t n,uint64_t index)
{
    int ring=au_ringed(s),rc;uint16_t flags=r16(p+2);uint32_t owner=r32(p+8),scopes=r32(p+12),k,j,mask=au_mask(s);
    uint64_t frame=r64(p+16),begin=r64(p+24),end=r64(p+32),lost=r64(p+48),sum;au_build *entry;
    uint32_t width=ring?528:(mask&512)?480:(mask&256)?416:(mask&16)?384:352;
    if(n!=width||r16(p)!=1||r32(p+4)!=r32(s->config+4)||(flags&~(mask!=7?127u:31u))||!(flags&2)
       ||((flags&1)?owner!=r32(s->config+16):owner!=0)||(!(flags&64)&&(scopes||r64(p+56)))
       ||((s->flags&AU_FRAME)&&s->last_owner==owner&&frame<=s->last_frame)
       ||lost<s->last_lost||((lost!=0)!=((flags&8)!=0))||(!(flags&16)&&end<begin)
       ||((flags&64)&&!scopes&&r64(p+56)))return PFR_INVALID;
    if(mask&16){
        if((!(mask&32)&&r32(p+380))||(!(flags&64)&&!au_zero(p+352,28))
           ||(!r32(p+352)&&r64(p+368))||r32(p+376)>scopes)return PFR_INVALID;
    }
    if(mask&256){
        uint32_t runs=r32(p+384),label=r32(p+396);uint64_t ticks=r64(p+400),one=r64(p+408);
        if(((!runs)!=(label==0))||(label>256&&label!=UINT32_MAX)||(!runs&&ticks)
           ||(label==UINT32_MAX?(runs<2||one>ticks):one!=ticks))return PFR_INVALID;
    }
    if(mask&512){
        uint32_t recorded=scopes<2?scopes:2;uint64_t prior=0;
        if(r32(p+444))return PFR_INVALID;
        sum=0;
        for(j=0;j<2;++j){uint64_t b=r64(p+448+16*j),e=r64(p+456+16*j);
            if(j<recorded){if(b>e||b<begin||(!(flags&16)&&e>end)||(j&&prior>b)||!plus64(&sum,e-b))return PFR_INVALID;prior=e;}
            else if(b||e)return PFR_INVALID;
        }
        if(scopes<=2&&sum!=r64(p+56))return PFR_INVALID;
    }
    for(k=0;k<3;++k){
        const uint8_t *row=p+64+96*k;sum=0;
        if(!(flags&4)){
            for(j=1;j<12;++j)if(!plus64(&sum,r64(row+8*j)))return PFR_INVALID;
            if((mask&512)&&!plus64(&sum,r64(p+416+8*k)))return PFR_INVALID;
            if(ring&&(!plus64(&sum,r64(p+480+8*k))||!plus64(&sum,r64(p+504+8*k))))return PFR_INVALID;
            if(sum!=r64(row))return PFR_INVALID;
        }
        for(j=7;j<12;++j)if(r64(row+8*j))s->flags|=AU_INVALID;
        if(ring&&r64(p+504+8*k))s->flags|=AU_INVALID;
    }
    rc=au_run(pools.finals,&s->final_count,cfg->auto_final_runs,owner,frame,1);if(rc)return rc;
    rc=au_entry(s,pools.builds,cfg->auto_build_entries,owner,frame,&entry);if(rc)return rc;
    entry->width=(uint32_t)n;entry->index=index;memcpy(entry->raw,p,n);
    if(au_inc(&s->frames,1))return PFR_COUNTER_RANGE;
    s->last_owner=owner;s->last_frame=frame;s->last_lost=lost;s->last_coverage=index;s->flags|=AU_FRAME;return PFR_OK;
}
static int au_clock_accept(au_state *s,au_pools pools,const pfm2_config *cfg,const uint8_t *p,size_t n,uint64_t index)
{
    uint16_t flags;uint32_t id,serial,first=r32(s->config+32);int labels;
    if(n!=96||r16(p)!=1||r32(p+4)!=r32(s->config+4)||r32(p+8)!=r32(s->config+12)||r32(p+16)||r32(p+84))return PFR_INVALID;
    flags=r16(p+2);id=r32(p+12);serial=r32(p+24);
    if((flags!=1&&flags!=2&&flags!=4&&flags!=8)||id<=s->last_anchor||id==UINT32_MAX||serial!=id
       ||r32(p+20)<first||r32(p+20)>=first+16||r32(p+28)>100000||r32(p+80)!=r32(s->config+16))return PFR_INVALID;
    labels=(au_mask(s)&64)?serial<UINT32_MAX/2&&r32(p+64)<2*serial-1&&r32(p+68)>=2*serial:
        r32(p+64)==serial&&r32(p+68)==serial;
    if(flags==1){if(r64(p+48)==UINT64_MAX||r64(p+48)!=r64(p+56)||!labels||r64(p+40)<r64(p+32)
        ||r64(p+88)!=r64(p+40)-r64(p+32)||!r32(p+28))return PFR_INVALID;}
    else if(r64(p+48)||r64(p+56))return PFR_INVALID;
    if(s->clock_count>=cfg->auto_clock_anchors)return PFR_IDENTITIES;
    pools.clocks[s->clock_count].index=index;memcpy(pools.clocks[s->clock_count].raw,p,96);++s->clock_count;
    if(au_inc(&s->clocks,1))return PFR_COUNTER_RANGE;
    s->last_anchor=id;return PFR_OK;
}
static int au_status(au_state *s,const uint8_t *p,size_t n)
{
    uint64_t value[15],sum;uint16_t flags;unsigned j;int ring=au_ringed(s),lower,final;
    if(n!=(ring?208u:128u)||r16(p)!=1||r32(p+4)!=r32(s->config+4))return PFR_INVALID;
    flags=r16(p+2);lower=(flags&2)!=0;final=(flags&1)!=0;
    if(r16(s->config+2)==1){
        if(flags!=1||!au_zero(p+8,120))return PFR_INVALID;
        if(au_inc(&s->statuses,1))return PFR_COUNTER_RANGE;
        memcpy(s->status,p,n);s->status_bytes=(uint32_t)n;s->flags|=AU_STATUS|AU_FINAL;return PFR_OK;
    }
    if((flags&~((au_mask(s)&512)?4095u:(au_mask(s)&32)?2047u:au_mask(s)!=7?1023u:63u))||((flags&12)&&!lower))return PFR_INVALID;
    for(j=0;j<15;++j){value[j]=r64(p+8+8*j);
        if(s->status_bytes&&j!=4&&j!=14&&value[j]<r64(s->status+8+8*j))return PFR_INVALID;
    }
    if(s->status_bytes&&((r16(s->status+2)&UINT16_C(4084))&~flags))return PFR_INVALID;
    if(!(flags&4)){sum=value[2];if(!plus64(&sum,value[3])||!plus64(&sum,value[4])||sum>value[1]||(!lower&&sum!=value[1]))return PFR_INVALID;}
    if(value[0]<s->last_op||value[13]<s->max_snapshot||value[13]>UINT32_MAX||value[14]>r32(s->config+48)||value[10]>value[9])return PFR_INVALID;
    if(ring){
        uint64_t issued=r64(p+128),retired=r64(p+136),published=r64(p+144),held=r64(p+152),untouched=r64(p+168),restored=r64(p+176);
        uint32_t reason=r32(p+200);sum=untouched;
        if(issued>UINT32_MAX/2-1||retired>issued||held!=issued-retired||(published&1)||published>2*issued
           ||published<r64(s->ring)||issued<r32(s->ring+48)||r64(p+160)>issued*32*126
           ||!plus64(&sum,restored)||sum>issued||r32(p+204)||reason>16||(!reason&&(r32(p+192)||r32(p+196))))return PFR_INVALID;
        if(s->status_bytes){
            for(j=0;j<8;++j)if(j!=3&&r64(p+128+8*j)<r64(s->status+128+8*j))return PFR_INVALID;
            if(r32(s->status+200)&&memcmp(s->status+192,p+192,12))return PFR_INVALID;
        }
        if(final&&((!lower&&(held||issued!=r32(s->ring+48)))||r64(s->ring+32)>untouched||r64(s->ring+40)>restored))return PFR_INVALID;
    }
    if(final){
        if(value[2]!=s->ops||value[5]!=s->frames||value[10]!=s->clocks
           ||(!lower&&(value[4]||value[14]||value[6]||value[9]-value[10]!=value[11])))return PFR_INVALID;
        s->flags|=AU_FINAL;
    }
    if(au_inc(&s->statuses,1))return PFR_COUNTER_RANGE;
    memcpy(s->status,p,n);s->status_bytes=(uint32_t)n;s->flags|=AU_STATUS;
    s->snapshot[0]=s->ops;s->snapshot[1]=s->frames;s->snapshot[2]=s->clocks;
    s->snapshot[3]=s->last_op;s->snapshot[4]=s->max_snapshot;
    memcpy(s->ring_snapshot,s->ring,96);s->status_chunks=s->op_chunks;return PFR_OK;
}
static int au_accept(au_state *s,au_pools pools,const pfm2_config *cfg,uint32_t type,const uint8_t *p,size_t n,
                     uint64_t index,uint64_t hz,int owner,int domain)
{
    int rc;uint32_t rows=0,i,width;
    if((s->flags&AU_FINAL)||(type!=34&&!(s->flags&AU_CONFIG))
       ||((s->flags&AU_CONFIG)&&r16(s->config+2)==1&&type!=37))return PFR_INVALID;
    switch(type){
    case 34:rc=au_config(s,p,n,hz,owner,domain);if(!rc)s->config_index=index;break;
    case 35:
        if(n<16||r16(p)!=1||r16(p+2)||r32(p+4)!=r32(s->config+4)||!r32(p+8)||r32(p+12)!=au_width(s))return PFR_INVALID;
        rows=r32(p+8);width=au_width(s);if((uint64_t)rows*width+16!=n)return PFR_INVALID;
        for(i=0;i<rows;++i){rc=au_op(s,pools,cfg,p+16+(size_t)i*width);if(rc)return rc;}
        rc=au_inc(&s->op_chunks,1);break;
    case 36:if(n<64)return PFR_INVALID;rc=au_coverage(s,pools,cfg,p,n,index);rows=1;break;
    case 37:rc=au_status(s,p,n);if(!rc)s->status_index=index;break;
    case 38:rc=au_clock_accept(s,pools,cfg,p,n,index);break;
    default:return PFR_INVALID;
    }
    if(rc)return rc;
    if(au_inc(&s->history,1+(uint64_t)rows)||au_inc(&s->bytes,(uint64_t)n+20)||au_inc(&s->chunks,1))return PFR_COUNTER_RANGE;
    return PFR_OK;
}
/* Accepted shared bootstrap only: original raw HEADER/CLOCKS/PROFILE payloads
 * become the inner typed bootstrap without copying historical events. */
static int au_boot(const pfp_view *v,uint32_t cap,uint32_t *count,uint32_t *bytes)
{
    size_t at=0;uint32_t want=v->next>2&&(r32(v->header+12)&2)?3:v->next>1?2:v->next?1:0,i;
    uint64_t total=0;
    for(i=0;i<want;++i){uint32_t n,type;
        if(v->metadata_bytes-at<4)return PFR_INVALID;
        n=r32(v->metadata+at);at+=4;if(n<20||n>v->metadata_bytes-at)return PFR_INVALID;
        type=r32(v->metadata+at);if(type!=(i==0?1:i==1?2:17)||r64(v->metadata+at+8)!=i)return PFR_INVALID;
        total+=16+(uint64_t)n-20;at+=n;
    }
    if(total>cap)return PFR_SUPPORT;
    *count=want;*bytes=(uint32_t)total;return PFR_OK;
}
static int au_size(const au_state *s,au_pools pools,const pfm2_config *cfg,const pfp_view *v,size_t *out)
{
    uint32_t count,boot,i;uint64_t n;int rc=au_boot(v,cfg->auto_bootstrap_bytes,&count,&boot);if(rc)return rc;
    if(s->final_count>cfg->auto_final_runs||s->touched_count>cfg->auto_touched_runs||s->build_count>cfg->auto_build_entries
       ||s->clock_count>cfg->auto_clock_anchors)return PFR_INVALID;
    n=(au_ringed(s)?532u:340u)+(uint64_t)boot+s->config_bytes+s->status_bytes
        +24*((uint64_t)s->final_count+s->touched_count)+80*(uint64_t)s->build_count+104*(uint64_t)s->clock_count;
    for(i=0;i<s->build_count;++i)n+=pools.builds[i].width;
    if(n>UINT32_MAX||n-340>cfg->auto_support_bytes)return PFR_SUPPORT;
    *out=(size_t)n;return PFR_OK;
}
static int au_encode(const au_state *s,au_pools pools,const pfm2_config *cfg,const pfp_view *v,uint8_t *b,size_t expected)
{
    uint32_t boot_count,boot_bytes,i,j;size_t fixed=au_ringed(s)?512:320,at,source=0;int rc;
    rc=au_boot(v,cfg->auto_bootstrap_bytes,&boot_count,&boot_bytes);if(rc)return rc;
    memset(b,0,fixed);memcpy(b,"PFRAUTO1",8);w32(b+8,au_ringed(s)?2:1);w32(b+12,(uint32_t)fixed);
    w64(b+16,expected);w64(b+24,v->next);w64(b+32,s->history);w32(b+40,s->flags|(boot_count>=2?64:0));
    w32(b+44,r32(v->header+12));w64(b+48,boot_count>=2?v->clock_frequency:0);w64(b+56,s->config_index);
    w64(b+64,s->last_op);w64(b+72,s->max_snapshot);w32(b+80,s->last_owner);w32(b+84,s->last_anchor);
    w64(b+88,s->last_frame);w64(b+96,s->last_lost);w64(b+104,s->ops);w64(b+112,s->op_chunks);w64(b+120,s->frames);
    w64(b+128,s->clocks);w64(b+136,s->statuses);w64(b+144,s->bytes);w64(b+152,s->chunks);w64(b+160,s->status_index);
    for(i=0;i<5;++i)w64(b+168+8*i,s->snapshot[i]);
    w64(b+208,s->last_coverage);w32(b+216,s->config_bytes);w32(b+220,s->status_bytes);w32(b+224,s->final_count);
    w32(b+228,s->touched_count);w32(b+232,s->build_count);w32(b+236,s->clock_count);w32(b+240,boot_count);w32(b+244,boot_bytes);
    if(au_ringed(s)){memcpy(b+320,s->ring,96);memcpy(b+416,s->ring_snapshot,96);}
    at=fixed;memcpy(b+at,v->header,16);at+=16;
    for(i=0;i<boot_count;++i){uint32_t n=r32(v->metadata+source);const uint8_t *raw=v->metadata+source+4;
        w64(b+at,r64(raw+8));w32(b+at+8,r32(raw));w32(b+at+12,n-20);memcpy(b+at+16,raw+16,n-20);at+=16+n-20;source+=4+n;
    }
    memcpy(b+at,s->config,s->config_bytes);at+=s->config_bytes;memcpy(b+at,s->status,s->status_bytes);at+=s->status_bytes;
    for(j=0;j<2;++j){const fi_run *r=j?pools.touched:pools.finals;uint32_t count=j?s->touched_count:s->final_count;
        for(i=0;i<count;++i){w32(b+at,r[i].owner);w32(b+at+4,0);w64(b+at+8,r[i].first);w64(b+at+16,r[i].last);at+=24;}}
    for(i=0;i<s->build_count;++i){const au_build *e=pools.builds+i;
        memset(b+at,0,80);w32(b+at,e->owner);w32(b+at+4,e->width!=0);w64(b+at+8,e->frame);
        for(j=0;j<3;++j)w64(b+at+16+8*j,e->records[j]);
        w64(b+at+40,e->abandoned);w64(b+at+48,e->call_excluded);w64(b+at+56,e->index);w32(b+at+64,e->width);at+=80;
        memcpy(b+at,e->raw,e->width);at+=e->width;
    }
    for(i=0;i<s->clock_count;++i){w64(b+at,pools.clocks[i].index);memcpy(b+at+8,pools.clocks[i].raw,96);at+=104;}
    if(at+4!=expected)return PFR_INVALID;
    w32(b+at,crc(b,at));return PFR_OK;
}
