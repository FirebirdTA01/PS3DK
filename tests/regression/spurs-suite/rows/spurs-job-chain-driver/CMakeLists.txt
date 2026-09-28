# job chain of BINARY2 jobs built with the -mspurs-job driver mode alone
suite_ppu(spurs-job-chain-driver job_chain.cpp)
ps3_add_spu_image(spurs-job-chain-driver
    NAME ${SUITE_SPU}
    SOURCES ${SUITE_DIR}/spu/job_chain.c
    JOBBIN
    CFLAGS -mspurs-job
    LDFLAGS -mspurs-job -Wl,-q)
suite_self(spurs-job-chain-driver)
