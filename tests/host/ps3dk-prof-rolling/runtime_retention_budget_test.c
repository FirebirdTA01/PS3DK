/* Actual public shutdown -> real retention inspection/index. No fake capture. */
#ifdef PS3DK_PROF_WITH_SPU
#define RUNTIME_SPU_ENTRY retention_existing_d6_entry
#include "runtime_spu_test.c"
#define RB(c,m) D6(c,m)
#else
#define RUNTIME_PPU_ENTRY retention_existing_d5_entry
#include "runtime_ppu_test.c"
#define RB(c,m) CHECK(c,m)
#endif
#include "roll_retention_inspect.h"
#include <errno.h>

static uint32_t rb_metadata,rb_inspection,rb_keep,rb_calls,rb_inspections;
static uint64_t rb_selected_limit;
static uint32_t rb_refused_type,rb_refused_count,rb_refused_reason;
/* Observe the genuine candidate/refusal at the selected encoder. This neither
 * repairs metadata capacity nor substitutes an accepted result. */
#ifdef PS3DK_PROF_WITH_SPU
#define RB_REAL_PREPARE __real_ps3dk_prof__roll_combined_prepare
#define RB_WRAP_PREPARE __wrap_ps3dk_prof__roll_combined_prepare
#else
#define RB_REAL_PREPARE __real_ps3dk_prof__roll_ppu_prepare
#define RB_WRAP_PREPARE __wrap_ps3dk_prof__roll_ppu_prepare
#endif
int RB_REAL_PREPARE(void *,const void *,void *,const void *,size_t,void *,size_t,pfo_prepared *);
int RB_WRAP_PREPARE(void *ctx,const void *accepted,void *prepared,const void *raw,size_t bytes,void *cp,size_t cap,pfo_prepared *out)
{
    int rc=RB_REAL_PREPARE(ctx,accepted,prepared,raw,bytes,cp,cap,out);
    if(rc==PFO_RESOURCE){const uint8_t *p=raw;RB(bytes>=20,"resource-candidate-framed");
        ++rb_refused_count;rb_refused_type=(uint32_t)p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24;
        rb_refused_reason=out->reason;}
    return rc;
}
int __real_ps3dk_prof_shutdown(void);
int __wrap_ps3dk_prof_shutdown(void)
{
    int rc=__real_ps3dk_prof_shutdown();
    if(rc==PS3DK_PROF_TEST_EROLLRESOURCE){
        ps3dk_prof_test_rolling_record actual;
        RB(rb_keep==1&&rb_inspection&&rb_refused_count==1&&rb_refused_type==6,
           "exact-metadata-stop-is-refused-TRAILER");
        RB(!rb_calls&&!rb_inspections,"exact-metadata-stop-before-retention");
        RB(ps3dk_prof_test_rolling_result(&actual)==0&&actual.result==PS3DK_PROF_TEST_EROLLRESOURCE&&
           actual.state==PFR_STOPPED_RESOURCE&&actual.reason==rb_refused_reason&&
           !actual.io_result&&!actual.close_result&&!actual.join_result&&!g_prof->prefix_trailer,
           "exact-metadata-stop-typed-resource-result");
        fprintf(stderr,"EXACT_METADATA_DIAGNOSTIC result=%d type=%u refused=%u reason=%u retention_calls=%u metadata=%u accepted_bytes=%" PRIu64 "\n",
                rc,rb_refused_type,rb_refused_count,rb_refused_reason,rb_calls,rb_metadata,actual.accepted_payload_bytes);
        /* Expected shutdown remains OK. This precise failure diagnoses the
         * shared TRAILER guard before the later enrollment-limit assertions. */
        RB(0,"exact-metadata-capacity-refused-trailer");
    }
    return rc;
}
#ifdef PS3DK_PROF_WITH_SPU
int __real_ps3dk_prof_test_rolling_combined_set(const ps3dk_prof_test_rolling_options *,const pfm_config *);
int __wrap_ps3dk_prof_test_rolling_combined_set(const ps3dk_prof_test_rolling_options *o,const pfm_config *m)
{
    if(m){pfm_config copy=*m;copy.metadata_bytes=rb_metadata;
        return __real_ps3dk_prof_test_rolling_combined_set(o,&copy);}
    return __real_ps3dk_prof_test_rolling_combined_set(o,m);
}
#endif
int __real_ps3dk_prof__pal_retain_complete_kind(const char *,const char *,uint64_t,unsigned,uint32_t,const pfi_limits *);
int __wrap_ps3dk_prof__pal_retain_complete_kind(const char *path,const char *app,uint64_t session,unsigned keep,uint32_t format,const pfi_limits *limits)
{
    ++rb_calls;RB(keep==1&&format==2&&limits,"real-completion-enrollment-request");
    rb_selected_limit=limits->max_metadata_bytes;
#ifdef PS3DK_PROF_WITH_SPU
    RB(rb_selected_limit==rb_metadata,"combined-retention-metadata-budget");
#else
    RB(rb_selected_limit==(uint64_t)rb_metadata+16u,"ppu-retention-original-header-budget");
#endif
    return __real_ps3dk_prof__pal_retain_complete_kind(path,app,session,keep,format,limits);
}
int __real_ps3dk_prof__roll_retention_inspect(const pfr_io *,const pfi_limits *,const pfi_buffers *,const char *,size_t,uint64_t,pfi_result *);
int __wrap_ps3dk_prof__roll_retention_inspect(const pfr_io *io,const pfi_limits *limits,const pfi_buffers *buffers,const char *app,size_t len,uint64_t session,pfi_result *result)
{
    int rc=__real_ps3dk_prof__roll_retention_inspect(io,limits,buffers,app,len,session,result);
    ++rb_inspections;
    if(rc==PFI_OK&&rb_inspection){
        pfi_limits exact=*limits;pfi_result out,sentinel;
        exact.max_metadata_bytes=rb_inspection;
        RB(__real_ps3dk_prof__roll_retention_inspect(io,&exact,buffers,app,len,session,&out)==PFI_OK,"cold-exact-metadata-bound");
        --exact.max_metadata_bytes;memset(&out,0xa5,sizeof out);sentinel=out;
        RB(__real_ps3dk_prof__roll_retention_inspect(io,&exact,buffers,app,len,session,&out)==PFI_SPACE&&
           !memcmp(&out,&sentinel,sizeof out),"cold-one-byte-under-metadata-bound");
    }
    return rc;
}
static uint32_t rb_number(const char *s)
{char *end;unsigned long v;errno=0;v=strtoul(s,&end,10);RB(!errno&&*s&&!*end&&v<=UINT32_MAX,"numeric-argument");return (uint32_t)v;}
int main(int argc,char **argv)
{
    ps3dk_prof_test_rolling_record out;ps3dk_prof_capture_record capture_record;
    char file[1200];FILE *manifest_file;const char *outdir;
    if(argc!=5){fputs("usage: runtime-retention OUTPUT METADATA KEEP INSPECTION_BOUND\n",stderr);return 2;}
    outdir=argv[1];rb_metadata=rb_number(argv[2]);rb_keep=rb_number(argv[3]);rb_inspection=rb_number(argv[4]);
    RB(rb_metadata>0&&rb_metadata<=32768&&rb_keep<=1,"explicit-capacities");
    RB(ps3dk_prof_set_capture_retention(rb_keep)==0,"set-real-retention");
#ifdef PS3DK_PROF_WITH_SPU
    uint32_t image;prod *p=&d6_producers[0];g_case="retention";g_out=outdir;d6_setup(1,0);
    RB(ps3dk_prof_spu_image_register_table((const uint8_t *)"\x02" "D6-image",9,d6_descs,1,&image)==0,"retention-real-image");
    d6_lane(0,image);d6_drain();
    for(unsigned i=0;i<4;++i){ev(p,1);ev(p,0);d6_flush(p);ps3dk_prof_frame();d6_drain();}
    d6_end(p);RB(ps3dk_prof_capture_end()==0,"retention-close-window");d6_drain();
    RB(ps3dk_prof_get_capture_record(&capture_record)==0,"retention-capture-record");
    /* Retention intentionally allocates inspection/index scratch after close.
     * Unlike the no-retention D6 fixture, this target makes no zero-allocation
     * shutdown claim and never disables counting to manufacture one. */
    RB(ps3dk_prof_shutdown()==0,"completed-capture-enrolled");
    atomic_store(&g_counting,0);
    RB(ps3dk_prof_test_rolling_result(&out)==0,"retention-result");
    d6_copy("capture.profr");RB(!fclose(d6_audit),"retention-audit-close");d6_audit=NULL;
    RB(!g_bad,"retention-original-port-checks");
    const char *family="spu";
#else
    ps3dk_prof_config c;directory=outdir;config(&c);options.ppu.metadata_bytes=rb_metadata;start(&c);
    RB(ps3dk_prof_thread_begin("owner")==0&&PS3DK_PROF_CAPTURE_WHOLE_RUN()==0,"retention-ppu-owner-window");
    for(unsigned i=0;i<4;++i){ps3dk_prof_frame();zone(&early);drain_ok();}
    RB(ps3dk_prof_capture_end()==0,"retention-close-window");drain_ok();
    RB(ps3dk_prof_get_capture_record(&capture_record)==0,"retention-capture-record");
    finished(0,PFR_SEALED,0);out=result;
    RB(!fclose(original),"retention-audit-close");original=NULL;
    const char *family="ppu";
#endif
    RB(out.state==PFR_SEALED&&out.result==0&&!out.io_result&&!out.close_result&&!out.join_result,"retention-real-terminal-result");
    RB(rb_calls==rb_keep&&rb_inspections==rb_keep,"real-retention-invocation-count");
    int n=snprintf(file,sizeof file,"%s/result.json",outdir);RB(n>0&&(size_t)n<sizeof file,"retention-manifest-path");
    manifest_file=fopen(file,"wbx");RB(manifest_file,"retention-manifest-open");
    RB(fprintf(manifest_file,"{\"family\":\"%s\",\"metadata\":%u,\"keep\":%u,\"inspection\":%u,\"selected_limit\":%" PRIu64 ",\"app\":\"%s\",\"session\":%" PRIu64 ",\"file_bytes\":%" PRIu64 ",\"result\":%d}\n",family,rb_metadata,rb_keep,rb_inspection,rb_selected_limit,capture_record.app,capture_record.session_id,out.file_bytes,out.result)>0&&!fclose(manifest_file),"retention-manifest-write");
    puts("PASS actual runtime retention budget");return 0;
}
