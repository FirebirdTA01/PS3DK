/* SPDX-License-Identifier: BSD-2-Clause
 * Finite SPURS job probe. UNVALIDATED ON TARGET. Blocking queue/firmware calls
 * require an external run watchdog; no profiler timeout bounds those calls.
 * All borrowed memory is static and survives failure until process exit. */
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <inttypes.h>
#include <unistd.h>
#include <sys/process.h>
#include <sys/spu.h>
#include <cell/spurs.h>
#include <cell/spurs/job_queue.h>
#include <cell/spurs/job_queue_semaphore.h>
#include <cell/sysmodule.h>
#include <ps3dk/prof.h>
#include "prof_spu.h"
#include "protocol.h"
#include "spurs_prof_job_bin.h"
#include "spurs_prof_job_jobheader_bin.h"
#if SJ_PROFILING
#include "spurs_prof_job_registration_bin.h"
#endif

SYS_PROCESS_PARAM(1001, 0x100000);
enum { DEPTH=16, POOL=SJ_JOBS+2 };
static cell::Spurs::Spurs2 spurs;
static CellSpursJobQueue queue __attribute__((aligned(128)));
static CellSpursJobQueueSemaphore semaphore __attribute__((aligned(128)));
static CellSpursJob128 jobs[SJ_JOBS] __attribute__((aligned(128)));
static uint64_t commands[CELL_SPURS_JOBQUEUE_SIZE_COMMAND_BUFFER(DEPTH)/8]
    __attribute__((aligned(CELL_SPURS_JOBQUEUE_COMMAND_BUFFER_ALIGN)));
static uint8_t pool[CELL_SPURS_JOBQUEUE_JOB_DESCRIPTOR_POOL_SIZE(0,POOL,0,0,0,0,0,0)]
    __attribute__((aligned(CELL_SPURS_JOBQUEUE_JOB_DESCRIPTOR_POOL_ALIGN)));
static volatile struct guarded_result {
    uint32_t before[32]; sj_receipt data; uint32_t after[32];
} slots[SJ_JOBS] __attribute__((aligned(128)));
static sj_receipt copies[SJ_JOBS];
static ps3dk_prof_spu_lane lanes[SJ_JOBS];
static ps3dk_prof_spu_lane_stats stats[SJ_JOBS];
static int end_rc[SJ_JOBS], end_called[SJ_JOBS], receipt_guards[SJ_JOBS];
static ps3dk_prof_capture_record capture;
static unsigned submitted, completed;
static int queue_complete, complete;
static int queue_rc=SJ_NOT_CALLED, close_rc=SJ_NOT_CALLED, queue_shutdown_rc=SJ_NOT_CALLED;
static int join_rc=SJ_NOT_CALLED, queue_exit=SJ_NOT_CALLED, finalize_rc=SJ_NOT_CALLED;
static int shutdown_rc=SJ_NOT_CALLED;
static const char *last_step="start";
static int last_rc;
static_assert(sizeof(CellSpursJob128)==128, "descriptor pool element size");
static_assert(sizeof(jobs[0].workArea.userData)>=8*sizeof(uint64_t), "payload fits");
static_assert(offsetof(guarded_result,data)==128, "guarded result alignment");

static const char *boolean(int v) { return v ? "true" : "false"; }
static void mark(const char *step)
{
    last_step=step;
    std::printf("SPURS_PROF step=%s submitted=%u completed=%u\n",step,submitted,completed);
    std::fflush(stdout);
}

/* Only called after queue completion; this is not a live/torn-receipt reader. */
static void snapshot()
{
    __asm__ volatile("sync" ::: "memory");
    for (unsigned i=0;i<SJ_JOBS;++i) {
        const volatile uint32_t *src=reinterpret_cast<const volatile uint32_t *>(&slots[i].data);
        uint32_t *dst=reinterpret_cast<uint32_t *>(&copies[i]);
        for (unsigned k=0;k<sizeof(sj_receipt)/4;++k) dst[k]=src[k];
        receipt_guards[i]=1;
        for (unsigned k=0;k<32;++k)
            if (slots[i].before[k]!=SJ_GUARD || slots[i].after[k]!=SJ_GUARD) receipt_guards[i]=0;
    }
    __asm__ volatile("sync" ::: "memory");
}

static int write_receipt()
{
    if (!capture.path[0]) return -1;
    char path[320], temp[328];
    int n=std::snprintf(path,sizeof path,"%s.spurs.json",capture.path);
    if (n<0 || static_cast<size_t>(n)>=sizeof path) return -1;
    n=std::snprintf(temp,sizeof temp,"%s.tmp",path);
    if (n<0 || static_cast<size_t>(n)>=sizeof temp) return -1;
    FILE *f=std::fopen(temp,"wb");
    if (!f) return -1;
    std::fprintf(f,"{\"schema\":1,\"arm\":\"%s\",\"build\":\"%s\",\"session\":\"%016" PRIx64 "\","
        "\"active_spus\":%u,\"submitted\":%u,\"completed\":%u,\"queue_complete\":%s,\"complete\":%s,"
        "\"queue_rc\":%d,\"close_rc\":%d,\"queue_shutdown_rc\":%d,\"join_rc\":%d,"
        "\"queue_exit\":%d,\"finalize_rc\":%d,\"shutdown_rc\":%d,\"last_step\":\"%s\",\"last_rc\":%d,\"jobs\":[\n",
        SJ_PROFILING?"ON":"OFF",SJ_BUILD_ID,capture.session_id,unsigned(SJ_ACTIVE_SPUS),submitted,completed,
        boolean(queue_complete),boolean(complete),queue_rc,close_rc,queue_shutdown_rc,join_rc,
        queue_exit,finalize_rc,shutdown_rc,last_step,last_rc);
    for (unsigned i=0;i<SJ_JOBS;++i) {
        const sj_receipt &r=copies[i]; const auto &l=lanes[i]; const auto &s=stats[i];
        std::fprintf(f,"%s{\"job\":%u,\"lane\":%u,\"receipt_job\":%u,\"receipt_lane\":%u,"
            "\"cookie\":\"%016" PRIx64 "\",\"receipt_cookie\":\"%016" PRIx64 "\","
            "\"ring\":%" PRIu64 ",\"receipt_ring\":%" PRIu64 ",\"ctl\":%" PRIu64 ",\"receipt_ctl\":%" PRIu64 ","
            "\"present\":%s,\"post_fini\":%s,\"status\":%d,\"attach_rc\":%d,\"flush_rc\":%d,\"fini_rc\":%d,"
            "\"checksum\":%u,\"expected_checksum\":%u,\"iterations\":%u,\"tag\":%u,"
            "\"mask_before\":%u,\"mask_after\":%u,\"event_before\":%u,\"event_after\":%u,"
            "\"guards\":%s,\"receipt_guards\":%s,\"desc_count\":%u,"
            "\"writer_ls\":%u,\"writer_bytes\":%u,\"staging_ls\":%u,\"staging_bytes\":%u,"
            "\"dec\":[%u,%u,%u],\"lane_end_called\":%s,\"lane_end_rc\":%d,"
            "\"final_written\":%u,\"fin_state\":%u,\"events\":%" PRIu64 ",\"lost\":%" PRIu64 ","
            "\"have_post_join\":%u,\"post_state\":%u,\"post_next_seq\":%" PRIu64 ","
            "\"post_older_lost\":%" PRIu64 ",\"post_refused\":%" PRIu64 "}\n",
            i?",":"",i,l.handle,r.job,r.lane,capture.session_id,r.cookie,l.ea_ring,r.ring,l.ea_ctl,r.ctl,
            boolean(r.magic==SJ_MAGIC && r.version==SJ_VERSION),boolean(r.post_fini==1),r.status,r.attach_rc,r.flush_rc,r.fini_rc,
            r.checksum,sj_checksum(i,SJ_ITERATIONS),r.iterations,r.tag,r.mask_before,r.mask_after,r.event_before,r.event_after,
            boolean(r.guards==1),boolean(receipt_guards[i]),r.desc_count,r.writer_ls,r.writer_bytes,r.staging_ls,r.staging_bytes,
            r.dec[0],r.dec[1],r.dec[2],boolean(end_called[i]),end_rc[i],s.final_written,s.fin_state,s.events,s.lost,
            s.have_post_join,s.post_state,s.post_next_seq,s.post_older_lost,s.post_refused);
    }
    std::fprintf(f,"]}\n");
    int failed=std::ferror(f);
    if (std::fflush(f)) failed=1;
    if (fsync(fileno(f))) failed=1;
    if (std::fclose(f)) failed=1;
    return failed ? -1 : std::rename(temp,path);
}

static int fail(const char *step, int rc)
{
    last_step=step; last_rc=rc; complete=0;
    std::printf("SPURS_PROF FAIL step=%s rc=%d; resources retained until process exit\n",step,rc);
    std::fflush(stdout);
    (void)write_receipt();
    /* No shutdown or lane_end here: neither a failed call nor a timeout proves
     * producer quiescence. Static buffers remain borrowed until process exit. */
    return 1;
}

int main()
{
    for (unsigned i=0;i<SJ_JOBS;++i) {
        end_rc[i]=SJ_NOT_CALLED;
        for (unsigned k=0;k<32;++k) slots[i].before[k]=slots[i].after[k]=SJ_GUARD;
    }
    ps3dk_prof_config cfg; ps3dk_prof_config_defaults(&cfg);
    cfg.mode=PS3DK_PROF_MODE_THREAD; cfg.max_threads=8; cfg.max_descriptors=16; cfg.ring_records=512;
    cfg.app="spurs-job-zones"; cfg.build_id=SJ_BUILD_ID; cfg.workload_tag=SJ_PROFILING?"ON":"OFF";
    ps3dk_prof_ext_config ext={}; ext.size=sizeof ext;
#if SJ_PROFILING
    ext.spu_lanes_max=SJ_JOBS; ext.spu_ring_slots=SJ_RING_SLOTS;
#endif
    int rc=ps3dk_prof_set_sink_path("/dev_hdd0/tmp");
    if (rc) return fail("sink",rc);
    if ((rc=ps3dk_prof_init_ex(&cfg,&ext))) return fail("prof init",rc);
    if ((rc=ps3dk_prof_get_capture_record(&capture))) return fail("capture identity",rc);
    if ((rc=ps3dk_prof_thread_begin("main")) && rc!=PS3DK_PROF_EALREADY) return fail("thread",rc);
    if ((rc=PS3DK_PROF_CAPTURE_WHOLE_RUN())) return fail("capture window",rc);
    std::printf("SPURS_PROF capture=%s session=%016" PRIx64 " build=%s\n",capture.path,capture.session_id,SJ_BUILD_ID);
#if SJ_PROFILING
    uint32_t image=0;
    if ((rc=ps3dk_prof_spu_image_register_elf(spurs_prof_job_registration_bin,spurs_prof_job_registration_bin_size,&image)))
        return fail("register registration ELF",rc);
    for (unsigned i=0;i<SJ_JOBS;++i) {
        char name[32]; std::snprintf(name,sizeof name,"spurs/job/%u",i);
        if ((rc=ps3dk_prof_spu_lane_create(name,image,0,&lanes[i]))) return fail("lane create",rc);
    }
#endif
    /* Check narrow queue/firmware addresses before giving those objects to
     * SPURS. The profiler's DMA EAs and result payload retain all 64 bits. */
    if (!spurs_prof_job_bin_size ||
        uint64_t(reinterpret_cast<uintptr_t>(spurs_prof_job_bin))+spurs_prof_job_bin_size-1>UINT32_MAX ||
        uint64_t(reinterpret_cast<uintptr_t>(&jobs[SJ_JOBS-1]))+sizeof(jobs[0])-1>UINT32_MAX ||
        uint64_t(reinterpret_cast<uintptr_t>(&queue))+sizeof(queue)-1>UINT32_MAX ||
        uint64_t(reinterpret_cast<uintptr_t>(&semaphore))+sizeof(semaphore)-1>UINT32_MAX ||
        uint64_t(reinterpret_cast<uintptr_t>(&spurs))+sizeof(spurs)-1>UINT32_MAX ||
        uint64_t(reinterpret_cast<uintptr_t>(pool))+sizeof(pool)-1>UINT32_MAX ||
        uint64_t(reinterpret_cast<uintptr_t>(commands))+sizeof(commands)-1>UINT32_MAX)
        return fail("queue EA32",-1);
    if ((rc=cellSysmoduleLoadModule(CELL_SYSMODULE_SPURS_JQ))) return fail("load SPURS_JQ",rc);
    cell::Spurs::SpursAttribute attr;
    if ((rc=cell::Spurs::SpursAttribute::initialize(&attr,4,100,2,false))) return fail("spurs attr",rc);
    if ((rc=attr.setNamePrefix("ProfJq",6))) return fail("spurs name",rc);
    if ((rc=attr.setSpuThreadGroupType(SYS_SPU_THREAD_GROUP_TYPE_EXCLUSIVE_NON_CONTEXT))) return fail("spurs group type",rc);
    mark("initialize SPURS");
    if ((rc=cell::Spurs::Spurs2::initialize(&spurs,&attr))) return fail("spurs init",rc);
    CellSpursJobQueueAttribute qa;
    if ((rc=cellSpursJobQueueAttributeInitialize(&qa))) return fail("queue attr",rc);
    if ((rc=cellSpursJobQueueAttributeSetMaxSizeJobDescriptor(&qa,256))) return fail("queue descriptor size",rc);
    if ((rc=cellSpursJobQueueAttributeSetIsHaltOnError(&qa,true))) return fail("queue halt",rc);
    CellSpursJobQueueJobDescriptorPool pd={}; pd.nJob128=POOL;
    uint8_t priorities[8]={8,0,0,0,0,0,0,0};
    if (SJ_ACTIVE_SPUS==2) priorities[1]=8;
    if ((rc=cellSpursCreateJobQueueWithJobDescriptorPool(reinterpret_cast<CellSpurs *>(&spurs),&queue,&qa,
        pool,&pd,"prof-jq",commands,DEPTH,SJ_ACTIVE_SPUS,priorities))) return fail("queue create",rc);
    CellSpursJobQueueHandle handle=CELL_SPURS_JOBQUEUE_HANDLE_INVALID;
    if ((rc=cellSpursJobQueueOpen(&queue,&handle))) return fail("queue open",rc);
    if ((rc=cellSpursJobQueueSemaphoreInitialize(&semaphore,&queue))) return fail("semaphore",rc);
    if (spurs_prof_job_jobheader_bin_size>sizeof(CellSpursJobHeader) || spurs_prof_job_jobheader_bin_size<0x30)
        return fail("jobheader size",-1);
    for (unsigned i=0;i<SJ_JOBS;++i) {
        std::memcpy(&jobs[i].header,spurs_prof_job_jobheader_bin,spurs_prof_job_jobheader_bin_size);
        jobs[i].header.eaBinary+=reinterpret_cast<uintptr_t>(spurs_prof_job_bin);
        if (jobs[i].header.eaBinary>UINT32_MAX) return fail("job binary EA32",-1);
        uint64_t *u=jobs[i].workArea.userData;
        u[0]=reinterpret_cast<uintptr_t>(&slots[i].data); u[1]=lanes[i].ea_ring; u[2]=lanes[i].ea_ctl;
        u[3]=(uint64_t(SJ_RING_SLOTS)<<32)|SJ_LS_CAP; u[4]=capture.session_id;
        u[5]=(uint64_t(i)<<32)|lanes[i].handle;
        u[6]=(uint64_t(SJ_ITERATIONS)<<32)|sj_checksum(i,SJ_ITERATIONS); u[7]=SJ_VERSION;
        if ((rc=cellSpursCheckJob(reinterpret_cast<const CellSpursJob256 *>(&jobs[i]),sizeof jobs[i],256)))
            return fail("job check",rc);
    }
    auto zone=PS3DK_PROF_ZONE_BEGIN("spurs.batch");
    mark("push jobs");
    __asm__ volatile("sync" ::: "memory");
    for (unsigned i=0;i<SJ_JOBS;++i) {
        if ((rc=cellSpursJobQueuePushJob(&queue,handle,&jobs[i].header,sizeof jobs[i],0,&semaphore)))
            return fail("push outcome unknown",rc);
        ++submitted;
    }
    if ((rc=cellSpursJobQueuePushFlush(&queue,handle))) return fail("queue flush",rc);
    mark("acquire six completions");
    queue_rc=cellSpursJobQueueSemaphoreAcquire(&semaphore,SJ_JOBS);
    if (queue_rc) return fail("queue completion",queue_rc);
    completed=SJ_JOBS; queue_complete=1; snapshot();
    PS3DK_PROF_ZONE_END(zone);
    mark("close shutdown join queue");
    close_rc=cellSpursJobQueueClose(&queue,handle);
    if (close_rc) return fail("queue close",close_rc);
    queue_shutdown_rc=cellSpursShutdownJobQueue(&queue);
    if (queue_shutdown_rc) return fail("queue shutdown",queue_shutdown_rc);
    join_rc=cellSpursJoinJobQueue(&queue,&queue_exit);
    if (join_rc || queue_exit) return fail("queue join",join_rc?join_rc:queue_exit);
    mark("finalize SPURS"); finalize_rc=spurs.finalize();
    if (finalize_rc) return fail("spurs finalize",finalize_rc);
    if ((rc=cellSysmoduleUnloadModule(CELL_SYSMODULE_SPURS_JQ))) return fail("unload SPURS_JQ",rc);
    for (unsigned i=0;i<SJ_JOBS;++i) {
        const auto &r=copies[i];
        if (r.magic!=SJ_MAGIC || r.version!=SJ_VERSION || r.job!=i || r.lane!=lanes[i].handle ||
            r.cookie!=capture.session_id || r.ring!=lanes[i].ea_ring || r.ctl!=lanes[i].ea_ctl ||
            r.status || !r.guards || !receipt_guards[i] || r.checksum!=sj_checksum(i,SJ_ITERATIONS) ||
            r.mask_before!=r.mask_after || r.event_before!=r.event_after) return fail("job receipt",-1);
#if SJ_PROFILING
        if (!sj_can_retire(queue_complete,&r,i,lanes[i].handle,capture.session_id,lanes[i].ea_ring,lanes[i].ea_ctl))
            return fail("missing successful lane-bound fini",r.fini_rc);
#endif
    }
    mark("retire lanes with completion receipts");
#if SJ_PROFILING
    for (unsigned i=0;i<SJ_JOBS;++i) {
        end_called[i]=1;
        end_rc[i]=ps3dk_prof_spu_lane_end(lanes[i].handle,1000000,&stats[i]);
        if (end_rc[i]) return fail("lane end",end_rc[i]);
        const auto &s=stats[i];
        if (s.final_written!=1 || s.fin_state!=1 || s.events!=7 || s.lost ||
            s.have_post_join!=1 || s.post_state!=2 || s.post_next_seq!=7 || s.post_older_lost || s.post_refused)
            return fail("lane final counters",-1);
    }
#endif
    ps3dk_prof_thread_end();
    shutdown_rc=ps3dk_prof_shutdown();
    if (shutdown_rc) return fail("prof shutdown",shutdown_rc);
    complete=1; mark("complete receipt; clock not certified");
    if (write_receipt()) return fail("receipt IO",-1);
    return 0;
}
