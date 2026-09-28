# the SPU job-queue runtime: handles and information from a job, a job
# suspending itself with WaitSignal, and a task on a job-queue semaphore
suite_ppu(spurs-jq-spu jq_spu.cpp spurs_jq_stub)
suite_task_manual(spurs-jq-spu jq_spu_task.c spurs_jq)
ps3_add_spu_image(spurs-jq-spu
    NAME ${SUITE_SPU}_job
    SOURCES ${SUITE_DIR}/spu/jq_spu_job.c
    NOSTARTFILES
    JOBBIN_WRAP
    CFLAGS -mspurs-job-initialize
    LDFLAGS -mspurs-job-initialize -Wl,-q -Wl,--gc-sections
    LDSCRIPT ${PS3DK}/spu/ldscripts/spurs_job.ld
    LIBS spurs_jq)
target_compile_definitions(spurs-jq-spu PRIVATE
    "SUITE_JOB_HEADER=\"${SUITE_SPU}_job_bin.h\""
    "SUITE_JOB_BIN=${SUITE_SPU}_job_bin"
    "SUITE_JOB_JOBHEADER=${SUITE_SPU}_job_jobheader_bin"
    "SUITE_JOB_JOBHEADER_HEADER=\"${SUITE_SPU}_job_jobheader_bin.h\"")
suite_self(spurs-jq-spu)
