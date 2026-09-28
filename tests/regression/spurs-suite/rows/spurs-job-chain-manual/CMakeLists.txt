# job chain of BINARY2 jobs, job startup, runtime and layout named explicitly
suite_ppu(spurs-job-chain-manual job_chain.cpp)
ps3_add_spu_image(spurs-job-chain-manual
    NAME ${SUITE_SPU}
    SOURCES ${SUITE_DIR}/spu/job_chain.c
    NOSTARTFILES
    JOBBIN
    CFLAGS -mspurs-job
    LDFLAGS -mspurs-job -Wl,-q
    LDSCRIPT ${PS3DK}/spu/ldscripts/spurs_job.ld
    LIBS spurs_job)
suite_self(spurs-job-chain-manual)
