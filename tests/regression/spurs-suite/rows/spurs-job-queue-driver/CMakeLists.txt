# job queue, jobs built with the -mspurs-job-initialize driver mode alone
suite_ppu(spurs-job-queue-driver job_queue.cpp spurs_jq_stub)
ps3_add_spu_image(spurs-job-queue-driver
    NAME spu_jq
    SOURCES ${SUITE_DIR}/spu/job_queue.c
    JOBBIN_WRAP
    CFLAGS -mspurs-job-initialize
    LDFLAGS -mspurs-job-initialize -Wl,-q)
ps3_add_self(spurs-job-queue-driver)
