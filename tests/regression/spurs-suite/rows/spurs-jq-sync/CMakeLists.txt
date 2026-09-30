# job queue ordering: does cellSpursJobQueuePortPushSync(port, 1) make jobs
# pushed after it wait until the earlier tag-0 jobs have finished, with and
# without a PushFlush in between; a run without it is the control
suite_ppu(spurs-jq-sync jq_sync.cpp spurs_jq_stub)
ps3_add_spu_image(spurs-jq-sync
    NAME ${SUITE_SPU}
    SOURCES ${SUITE_DIR}/spu/jq_sync.c
    JOBBIN_WRAP
    CFLAGS -mspurs-job-initialize
    LDFLAGS -mspurs-job-initialize -Wl,-q)
suite_self(spurs-jq-sync)
