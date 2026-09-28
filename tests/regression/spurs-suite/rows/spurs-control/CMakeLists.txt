# SPU-side control: tasks run, shut down and release other workloads
suite_ppu(spurs-control control.cpp)
suite_task_manual(spurs-control control.c)
ps3_add_spu_image(spurs-control
    NAME ${SUITE_SPU}_job
    SOURCES ${SUITE_DIR}/spu/job_chain.c
    JOBBIN
    CFLAGS -mspurs-job
    LDFLAGS -mspurs-job -Wl,-q)
target_compile_definitions(spurs-control PRIVATE
    "SUITE_JOB_HEADER=\"${SUITE_SPU}_job_bin.h\""
    "SUITE_JOB_BIN=${SUITE_SPU}_job_bin"
    "SUITE_JOB_BIN_SIZE=${SUITE_SPU}_job_bin_size")
suite_self(spurs-control)
