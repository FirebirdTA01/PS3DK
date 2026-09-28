# job queue, job startup, runtime and layout named explicitly
suite_ppu(spurs-job-queue-manual job_queue.cpp spurs_jq_stub)
ps3_add_spu_image(spurs-job-queue-manual
    NAME ${SUITE_SPU}
    SOURCES ${SUITE_DIR}/spu/job_queue.c
    NOSTARTFILES
    JOBBIN_WRAP
    CFLAGS -mspurs-job-initialize
    LDFLAGS -mspurs-job-initialize -Wl,-q -Wl,--gc-sections
    LDSCRIPT ${PS3DK}/spu/ldscripts/spurs_job.ld
    LIBS spurs_jq)
suite_self(spurs-job-queue-manual)
