/* Actual APP-mode runtime: a rejected whole chunk must not acknowledge its
 * producer/table state. This is a prerequisite for bounded output admission;
 * a short failed physical write still fails the append sink permanently. */
#include "prof_internal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint32_t reject_kind, reject_occurrence=1, rejected, accepted[44];
static uint64_t accepted_records;
static uint32_t accepted_descriptors;
static char long_name[513], long_tag[513], long_file[513];
static ps3dk_prof_desc many[800];
static uint32_t get32(const uint8_t *p)
{ return (uint32_t)p[0] | (uint32_t)p[1]<<8 | (uint32_t)p[2]<<16 | (uint32_t)p[3]<<24; }
static long emit(void *ctx,const void *bytes,unsigned long n)
{
    const uint8_t *b=bytes; uint32_t kind;
    (void)ctx;
    if(n==16 && !memcmp(b,"PS3DKPRF",8)) return (long)n;
    if(n<20 || (uint64_t)get32(b+4)+20!=n) abort();
    kind=get32(b);
    if(kind==reject_kind && accepted[kind]+1==reject_occurrence) { ++rejected; return -1; }
    if(kind==C_RECORDS) accepted_records+=get32(b+20);
    if(kind==C_DESCRIPTORS) accepted_descriptors+=get32(b+16);
    if(kind<44) ++accepted[kind];
    return (long)n;
}
static int close_sink(void *ctx) { (void)ctx; return 0; }
#define CHECK(c,m) do { if(!(c)) { fprintf(stderr,"ACK_ASSERT: %s\n",m); return 1; } } while(0)
int main(int argc,char **argv)
{
    ps3dk_prof_config config; prof *p; uint32_t si,gen;
    ps3dk_prof_sink_callbacks cb={emit,NULL,close_sink,NULL};
    if(argc!=2) return 3;
    reject_kind=(uint32_t)strtoul(argv[1],NULL,10);
    if(reject_kind==105) { reject_kind=C_RECORDS; reject_occurrence=2; }
    if(reject_kind==103) { reject_kind=C_DESCRIPTORS; reject_occurrence=2; }
    ps3dk_prof_config_defaults(&config);
    config.mode=PS3DK_PROF_MODE_APP; config.max_threads=2;
    config.max_descriptors=reject_kind==C_DESCRIPTORS && reject_occurrence==2 ? 1024 : 8;
    config.ring_records=reject_occurrence==2 ? 65536 : 512;
    CHECK(ps3dk_prof_set_sink_callbacks(&cb)==0,"set callback");
    CHECK(ps3dk_prof_init(&config)==0,"init");
    CHECK(ps3dk_prof_thread_begin("ack-owner")==0,"thread begin");
    CHECK(PS3DK_PROF_CAPTURE_WHOLE_RUN()==0,"window begin");
    ps3dk_prof_zone zone=PS3DK_PROF_ZONE_BEGIN("accepted-prefix");
    PS3DK_PROF_ZONE_END(zone);
    ps3dk_prof_frame();
    if(reject_occurrence==2 && reject_kind==C_RECORDS)
        for(unsigned i=0;i<33000;++i) ps3dk_prof_frame();
    if(reject_occurrence==2 && reject_kind==C_DESCRIPTORS) {
        memset(long_name,'n',512); memset(long_tag,'t',512); memset(long_file,'f',512);
        for(unsigned i=0;i<800;++i) {
            many[i]=(ps3dk_prof_desc){long_name,long_tag,long_file,i+1,1,0,0};
            zone=ps3dk_prof_zone_begin(&many[i],PS3DK_PROF_TOKEN_NONE);
            CHECK(zone.kind==ZK_PUSHED,"split descriptor source admitted");
            ps3dk_prof_zone_end(&zone);
        }
    }
    p=g_prof; si=0;
    while(si<p->nslots && p->slots[si].state==SLOT_FREE) ++si;
    CHECK(si<p->nslots,"owner slot"); gen=p->slots[si].generation;
    CHECK(p->slots[si].rg->write_idx>0,"source records exist");
    if(!reject_kind || reject_kind==C_RETIRED) ps3dk_prof_thread_end();
    int rc=ps3dk_prof_drain();
    if(!reject_kind) {
        CHECK(rc==0 && !rejected,"control drain");
        CHECK(accepted[C_THREADS] && accepted[C_DESCRIPTORS] && accepted[C_RECORDS]
              && accepted[C_WINDOW_BEGIN] && accepted[C_RETIRED],"control accepts every family");
        CHECK(p->dr.retired_gen[si]==gen && p->slots[si].state==SLOT_FREE,"accepted retirement frees slot");
        CHECK(ps3dk_prof_shutdown()==0,"control shutdown");
    } else {
        CHECK(rc!=0 && rejected==1,"one refused chunk stops sink");
        if(reject_kind==C_THREADS) CHECK(p->dr.decl_gen[si]==0,"rejected THREADS acknowledged generation");
        if(reject_kind==C_DESCRIPTORS) {
            CHECK(accepted[C_DESCRIPTORS]+1==reject_occurrence,"descriptor split reached selected chunk");
            CHECK(p->dr.desc_emitted==accepted_descriptors,"rejected DESCRIPTORS acknowledged table");
        }
        if(reject_kind==C_WINDOW_BEGIN) CHECK(p->winq_r==0,"rejected WINDOW acknowledged queue");
        if(reject_kind==C_RECORDS) {
            CHECK(accepted[C_RECORDS]+1==reject_occurrence,"record split reached selected chunk");
            CHECK(p->slots[si].rg->read_idx==accepted_records,"rejected RECORDS acknowledged ring");
            CHECK(p->dr.written[si]==accepted_records,"rejected RECORDS incremented written");
        }
        if(reject_kind==C_RETIRED) {
            CHECK(p->dr.retired_gen[si]==0,"rejected RETIRED acknowledged generation");
            CHECK(p->slots[si].state!=SLOT_FREE,"rejected RETIRED freed slot");
        }
        if(reject_kind!=C_RETIRED) ps3dk_prof_thread_end();
        CHECK(ps3dk_prof_shutdown()==PS3DK_PROF_ESINKFAIL,"failed append shutdown remains ESINKFAIL");
    }
    puts("runtime chunk acknowledgement PASS"); return 0;
}
