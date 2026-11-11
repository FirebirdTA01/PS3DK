#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "auto_rsx_model.h"
#include "fixture.h"
static uint32_t buf[70002];
static uint32_t ring[262146];
static arm_model model;
static unsigned body_calls;
static int body(arm_model *m,const arm_operation *op,void *user) {
    (void)user;body_calls++;
    gcmContextData c={buf+1,buf+70001};fixture_group_count=0;
    if(!arm_words(op)) {uint32_t nop[2]={RSX_METHOD(NV40TCL_NOP,1),0};return arm_emit_packet(m,nop,2);}
    if(op->kind==ARM_ARRAY)fixture_DrawVertexArray(&c,op->mode,op->start,op->count);
    else if(op->kind==ARM_INDEX)fixture_DrawIndexArray(&c,op->mode,op->offset,op->count,op->index_type,0);
    else fixture_ClearSurface(&c,op->mask);
    uint32_t *p=buf+1;
    for(unsigned i=0;i<fixture_group_count;i++) {
        uint32_t n=fixture_groups[i].count;
        assert(fixture_groups[i].start==p && (size_t)(c.current-p)>=n);
        if(arm_emit_packet(m,p,n))return 1;
        p+=n;
    }
    assert(p==c.current);
    return 0;
}
static arm_operation operation(unsigned kind) {
    arm_operation o={0};o.kind=kind;o.count=1;o.mode=5;o.mask=0xf3;
    o.context=o.generation=o.frame_owner=1;return o;
}
static void reset(unsigned cap,unsigned pairs,unsigned metadata) {
    assert(cap+2<sizeof ring/sizeof ring[0]);ring[0]=ring[cap+2]=0xfeedface;
    arm_init(&model,ring+1,cap,pairs,metadata);assert(arm_frame_open(&model,1,0));body_calls=0;
}
static int capacity(void) {
    unsigned cases=0;
    reset(10,1,1);model.current=5;arm_operation clear=operation(ARM_CLEAR);
    arm_result boundary=arm_submit(&model,&clear,body,0);
    if(body_calls!=1){fputs("delegated clear call missing\n",stderr);return 1;}
    if(boundary.reason!=ARM_ROOM || model.current!=6 || model.wraps!=1){fputs("clear reservation group was split\n",stderr);return 1;}
    for(unsigned kind=ARM_ARRAY;kind<=ARM_CLEAR;kind++) {
        arm_operation o=operation(kind);uint32_t n=(uint32_t)arm_words(&o);
        reset(n+12,1,1);arm_result r=arm_submit(&model,&o,body,0);
        if(r.reason!=ARM_OBSERVED){fprintf(stderr,"exact-fit expected OBSERVED got %u\n",r.reason);return 1;}
        assert(r.ticket && r.application_result==0 && body_calls==1 && model.marker_words==12 && model.app_words==n && model.wraps==0);
        assert(ring[0]==0xfeedface && ring[n+14]==0xfeedface);
        uint64_t expected=model.app_hash;
        reset(n+11,1,1);r=arm_submit(&model,&o,body,0);
        if(r.reason!=ARM_ROOM){fprintf(stderr,"one-short expected ROOM got %u\n",r.reason);return 1;}
        assert(body_calls==1 && model.marker_words==0 && model.app_words==n && model.app_hash==expected);
        for(unsigned reason=ARM_CONTEXT;reason<=ARM_METADATA;reason++) {
            if(reason==ARM_SIZE)continue;
            reset(64,reason==ARM_REPORTS?0:1,reason==ARM_METADATA?0:1);
            if(reason==ARM_CONTEXT)o.context=2;
            if(reason==ARM_REENTRY)model.busy=1;
            r=arm_submit(&model,&o,body,0);
            assert(r.reason==reason && body_calls==1 && model.marker_words==0 && model.app_hash==expected);
            o.context=1;cases++;
        }
        reset(32,1,1);model.current=31;r=arm_submit(&model,&o,body,0);
        assert(r.reason==ARM_ROOM && model.wraps==1 && model.phases[0]==1 && model.phases[1]==1 && model.app_hash==expected);
        assert(body_calls==1 && model.calls==1);cases+=3;
    }
    for(unsigned kind=ARM_UNSAFE;kind<=ARM_CALL;kind++) {
        arm_operation o=operation(kind);reset(64,1,1);
        arm_result r=arm_submit(&model,&o,body,0);
        assert(r.reason==ARM_UNSUPPORTED && body_calls==1 && model.marker_words==0);cases++;
    }
    printf("capacity/projection=%u PASS\n",cases);return 0;
}
static int lifetime(void) {
    arm_operation o=operation(ARM_CLEAR);reset(128,1,1);
    arm_result r=arm_submit(&model,&o,body,0);assert(r.reason==ARM_OBSERVED);
    assert(!arm_gpu_complete(&model,r.ticket,1,0,1,100));
    assert(!arm_sink_result(&model,r.ticket,1));
    if(!arm_publish(&model,1,1,r.ticket)){fputs("publish expected success\n",stderr);return 1;}
    assert(!arm_publish(&model,1,2,r.ticket));assert(!arm_publish(&model,1,1,r.ticket+1));
    assert(!arm_gpu_complete(&model,r.ticket,2,0,1,100));
    assert(!arm_gpu_complete(&model,r.ticket,1,0,7,100));
    assert(!arm_gpu_complete(&model,r.ticket,1,0,1,UINT64_MAX));
    assert(arm_gpu_complete(&model,r.ticket,1,0,1,100));
    assert(!arm_gpu_complete(&model,r.ticket,1,0,1,100));
    assert(!arm_sink_result(&model,r.ticket,1));
    assert(arm_submit(&model,&o,body,0).reason==ARM_REPORTS);
    assert(arm_gpu_complete(&model,r.ticket,1,1,2,101));
    assert(!arm_sink_result(&model,r.ticket,0));
    assert(arm_submit(&model,&o,body,0).reason==ARM_REPORTS);
    assert(arm_drain_offer(&model,r.ticket));
    assert(arm_sink_result(&model,r.ticket,1));
    arm_result next=arm_submit(&model,&o,body,0);assert(next.reason==ARM_OBSERVED && next.ticket!=r.ticket);
    assert(!arm_gpu_complete(&model,r.ticket,1,1,2,101));
    arm_stop(&model);assert(model.tickets[0].state==ARM_QUARANTINE);
    assert(arm_submit(&model,&o,body,0).reason==ARM_CONTEXT);
    reset(128,1,1);r=arm_submit(&model,&o,body,0);assert(arm_publish(&model,1,1,r.ticket));
    assert(arm_gpu_complete(&model,r.ticket,1,0,1,101) && arm_gpu_complete(&model,r.ticket,1,1,2,100));
    assert(model.tickets[0].state==ARM_QUARANTINE && !arm_drain_offer(&model,r.ticket));
    assert(!arm_sink_result(&model,r.ticket,1));
    reset(128,1,1);model.next_serial=UINT32_MAX-1;
    assert(arm_submit(&model,&o,body,0).reason==ARM_REPORTS);
    printf("lifetime stale/publication/sink/stop/serial PASS\n");return 0;
}
static void words(const char *name,const uint32_t *p,unsigned n) {
    printf("%s",name);for(unsigned i=0;i<n;i++)printf(" %08x",p[i]);putchar('\n');
}
static arm_result cell_alias(arm_model *m,const arm_operation *o) {
    return arm_submit(m,o,body,0); /* alias delegates to the canonical seam only */
}
static int projection(void) {
    for(unsigned kind=1;kind<=3;kind++)for(unsigned type=0;type<2;type++)for(unsigned off=0;off<128;off+=4) {
        arm_operation o=operation(kind);o.count=257;o.index_type=type;o.offset=off;
        reset(512,0,0);arm_result r=cell_alias(&model,&o);
        assert(r.reason==ARM_REPORTS && body_calls==1);
        words("OFF",ring+1,model.current);
        reset(512,2,2);r=cell_alias(&model,&o);
        assert(!r.reason && body_calls==1 && model.calls==1);
        words("ON",ring+1,model.current);
    }
    return 0;
}
static int retire(arm_result r) {
    if(r.reason)return 1;
    arm_ticket *t=0;
    for(unsigned i=0;i<model.pair_limit;i++)if(model.tickets[i].id==r.ticket)t=&model.tickets[i];
    assert(t);
    return arm_publish(&model,1,1,r.ticket) && arm_gpu_complete(&model,r.ticket,1,0,t->serial[0],100)
        && arm_gpu_complete(&model,r.ticket,1,1,t->serial[1],101) && arm_sink_result(&model,r.ticket,1);
}
static void coverage(const char *scenario,unsigned prefix,unsigned count) {
    printf("{\"scenario\":\"%s\",\"prefix_bytes\":%u,\"draw_count\":%u,\"coverage_lost\":%u,\"frames\":[",scenario,prefix,count,model.coverage_lost);
    for(unsigned f=0;f<model.frames_used;f++) {
        arm_frame *p=&model.frames[f];
        printf("%s{\"owner\":%u,\"frame\":%llu,\"closed\":%u,\"unsupported\":%llu,\"kinds\":[",f?",":"",p->owner,(unsigned long long)p->id,p->closed,(unsigned long long)p->unsupported);
        for(unsigned k=0;k<3;k++) {
            printf("%s{\"visited\":%llu,\"observed\":%llu,\"excluded\":[",k?",":"",(unsigned long long)p->visited[k],(unsigned long long)p->observed[k]);
            for(unsigned r=0;r<ARM_REASONS;r++)printf("%s%llu",r?",":"",(unsigned long long)p->excluded[k][r]);
            printf("]}");
        }
        printf("]}");
    }
    printf("]}\n");
}
static int sequence(void) {
    reset(4096,1,1);
    for(unsigned f=0;f<3;f++) {
        if(f)assert(arm_frame_open(&model,1,f));
        for(unsigned k=1;k<=3;k++) {
            arm_operation o=operation(k);o.count=257;o.offset=4;o.frame=f;
            unsigned start=model.current;arm_result r=cell_alias(&model,&o);assert(!r.reason);
            assert(model.tickets[0].frame_owner==1 && model.tickets[0].frame==f && model.tickets[0].kind==k);
            words("OFF",buf+1,(unsigned)arm_words(&o));words("ON",ring+1+start,model.current-start);
            assert(retire(r));
        }
        assert(arm_frame_close(&model,1,f));
    }
    assert(!model.wraps && body_calls==9);return 0;
}
static int dense(void) {
    const unsigned counts[]={1,257,4096,0x7ff01};
    for(unsigned prefix=4;prefix<=4096;prefix*=1024)for(unsigned c=0;c<4;c++) {
        /* Total storage 1 MiB, modeled firmware prefix, one reserved tail word. */
        unsigned cap=(1048576-prefix-4)/4;reset(cap,2,2);
        for(unsigned f=0;f<64;f++) {
            if(f)assert(arm_frame_open(&model,1,f));
            for(unsigned j=0;j<4096;j++) {
                arm_operation o=operation(j%16==0?ARM_CLEAR:(j&1?ARM_INDEX:ARM_ARRAY));
                o.frame=f;o.count=counts[c];o.offset=(j%32)*4;o.index_type=j&1;
                arm_result r=arm_submit(&model,&o,body,0);
                assert(r.reason==ARM_OBSERVED || r.reason==ARM_ROOM);
                assert(retire(r));
                assert(ring[0]==0xfeedface && ring[cap+2]==0xfeedface);
            }
            assert(arm_frame_close(&model,1,f));
        }
        assert(body_calls==64*4096 && model.calls==64*4096);
        coverage("immediate-retirement",prefix,counts[c]);
    }
    reset(4096,1,1);
    for(unsigned f=0;f<64;f++) {
        if(f)assert(arm_frame_open(&model,1,f));
        for(unsigned j=0;j<4096;j++){arm_operation o=operation(ARM_CLEAR);o.frame=f;(void)arm_submit(&model,&o,body,0);}
        assert(arm_frame_close(&model,1,f));
    }
    coverage("blocked-sink",0,1);return 0;
}
static int uncertain(arm_model *m,const arm_operation *o,void *unused) {
    (void)o;(void)unused;const uint32_t packet[]={0x00040100,0};
    assert(arm_emit_packet(m,packet,2)==0);return 17;
}
static int ownership(void) {
    arm_registry registry={0};reset(128,1,1);
    if(!arm_claim_reports(&registry,1,1,0,2,240)){fputs("registry initial claim refused\n",stderr);return 1;}
    assert(!arm_claim_reports(&registry,2,1,1,2,241));
    assert(!arm_claim_reports(&registry,2,1,2,2,240));
    assert(!arm_claim_reports(&registry,1,2,2,2,241));
    assert(!arm_claim_reports(&registry,2,1,2047,2,241));
    assert(!arm_claim_reports(&registry,2,1,UINT32_MAX,2,241));
    assert(!arm_claim_reports(&registry,2,1,2,2,65));
    assert(arm_claim_reports(&registry,2,1,2,2,241));
    assert(!arm_release_reports(&registry,1,1,&model));
    arm_stop(&model);assert(arm_release_reports(&registry,1,1,&model));
    assert(arm_claim_reports(&registry,1,2,0,2,240));
    reset(128,1,1);model.generation=2;arm_operation o=operation(ARM_CLEAR);o.generation=2;
    arm_result r=arm_submit(&model,&o,uncertain,0);
    assert(r.reason==ARM_EMISSION && r.application_result==17 && model.tickets[0].state==ARM_QUARANTINE);
    arm_stop(&model);assert(!arm_release_reports(&registry,1,2,&model));
    assert(arm_submit(&model,&o,body,0).reason==ARM_CONTEXT);
    memset(&registry,0,sizeof registry);reset(128,1,1);o.generation=1;
    assert(arm_claim_reports(&registry,2,1,0,2,241));
    assert(!arm_bind_reports(&model,&registry,1,2,240));
    assert(arm_submit(&model,&o,body,0).reason==ARM_REPORTS && body_calls==1 && model.marker_words==0);
    reset(128,1,1);assert(arm_bind_reports(&model,&registry,2,2,242));
    r=arm_submit(&model,&o,body,0);assert(r.reason==ARM_OBSERVED);
    assert(ring[2]==0x01000020 && ring[4]==242*16);
    assert(!arm_bind_reports(&model,&registry,4,2,243));
    printf("registry/conflict/quiescence/uncertain PASS\n");return 0;
}
static int recursive_body(arm_model *m,const arm_operation *o,void *unused) {
    (void)unused;arm_operation child=operation(ARM_CLEAR);
    arm_result r=arm_submit(m,&child,body,0);
    assert(r.reason==ARM_REENTRY);
    return body(m,o,0);
}
static int reentry(void) {
    reset(128,1,1);arm_operation o=operation(ARM_ARRAY);
    arm_result r=arm_submit(&model,&o,recursive_body,0);
    assert(r.reason==ARM_EMISSION && model.calls==2 && body_calls==2 && model.marker_words==6 && !model.wraps);
    assert(model.tickets[0].state==ARM_QUARANTINE && !model.busy);
    assert(model.frames[1].excluded[2][ARM_REENTRY]==1 && model.frames[1].excluded[0][ARM_EMISSION]==1);
    printf("recursive delegate and retained uncertain ownership PASS\n");return 0;
}
static int schedules(void) {
    unsigned pairs,metadata;char actions[128];
    while(scanf("%u %u %127s",&pairs,&metadata,actions)==3) {
        assert(pairs>=1 && pairs<=3 && metadata<=2);reset(4096,pairs,metadata);
        arm_operation o=operation(ARM_CLEAR);arm_result r=arm_submit(&model,&o,body,0);
        for(unsigned j=0;actions[j];j++) {
            int result=0;unsigned reason=99;
            switch(actions[j]) {
            case 'P':result=arm_publish(&model,1,1,model.next_ticket);break;
            case '0':result=arm_gpu_complete(&model,1,1,0,1,100);break;
            case '1':result=arm_gpu_complete(&model,1,1,1,2,101);break;
            case 'A':result=arm_sink_result(&model,1,1);break;
            case 'F':result=arm_sink_result(&model,1,0);break;
            case 'N':r=arm_submit(&model,&o,body,0);result=r.application_result;reason=r.reason;break;
            case 'T':arm_stop(&model);break;
            default:assert(0);
            }
            printf("%d,%u,%llu,%llu",result,reason,(unsigned long long)model.next_ticket,(unsigned long long)model.calls);
            for(unsigned i=0;i<pairs;i++){arm_ticket *t=&model.tickets[i];printf(",%llu,%u,%u,%u",(unsigned long long)t->id,t->state,t->complete,t->metadata_accepted);}
            putchar(actions[j+1]?';':'\n');
        }
    }
    return 0;
}
int main(int argc,char **argv) {
    if(argc==2 && !strcmp(argv[1],"projection"))return projection();
    if(argc==2 && !strcmp(argv[1],"dense"))return dense();
    if(argc==2 && !strcmp(argv[1],"sequence"))return sequence();
    if(argc==2 && !strcmp(argv[1],"schedules"))return schedules();
    const uint32_t counts[]={1,2,31,32,33,127,128,255,256,257,511,512,513,4096,0x7ff00,0x7ff01,0xfffeff,0xffffff};
    unsigned cases=0;
    for(unsigned kind=ARM_ARRAY;kind<=ARM_CLEAR;kind++)
    for(unsigned type=0;type<2;type++)
    for(unsigned off=0;off<132;off+=4)
    for(unsigned i=0;i<sizeof counts/sizeof counts[0];i++) {
        arm_operation op={0};op.kind=kind;op.count=counts[i];op.index_type=type;op.offset=off;op.mode=5;op.mask=0xf3;
        buf[0]=buf[70001]=0xdeadbeef;
        gcmContextData c={buf+1,buf+70001};fixture_group_count=0;
        if(kind==ARM_ARRAY) fixture_DrawVertexArray(&c,op.mode,op.start,op.count);
        else if(kind==ARM_INDEX) fixture_DrawIndexArray(&c,op.mode,op.offset,op.count,op.index_type,0);
        else fixture_ClearSurface(&c,op.mask);
        uint64_t n=(uint64_t)(c.current-(buf+1));
        if(arm_words(&op)!=n){fprintf(stderr,"sizing kind=%u count=%u offset=%u type=%u predicted=%llu actual=%llu\n",kind,op.count,off,type,(unsigned long long)arm_words(&op),(unsigned long long)n);return 1;}
        assert(buf[0]==0xdeadbeef && buf[70001]==0xdeadbeef);cases++;
    }
    arm_operation op={0};op.kind=ARM_ARRAY;
    assert(arm_words(&op)==0);op.count=0x1000000;assert(arm_words(&op)==0);op.count=UINT32_MAX;assert(arm_words(&op)==0);
    op.kind=ARM_INDEX;op.count=1;op.index_type=2;assert(arm_words(&op)==0);
    uint64_t bytes=17;assert(!arm_words_to_bytes(UINT64_MAX/4+1,&bytes)&&bytes==17);
    assert(arm_words_to_bytes(UINT64_MAX/4,&bytes)&&bytes==(UINT64_MAX/4)*4);
    printf("sizing=%u unsupported/overflow PASS\n",cases);
    if(capacity())return 1;
    if(lifetime())return 1;
    if(ownership())return 1;
    return reentry();
}
