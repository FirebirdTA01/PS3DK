# job extras from the SPU: urgent commands and calls, the grab limit,
# SetJobbin2Param (checked against the PPU's), and the job memory check
suite_ppu(spurs-job-extras job_extras.cpp)
suite_task_manual(spurs-job-extras job_extras_task.c)
ps3_add_spu_image(spurs-job-extras
    NAME ${SUITE_SPU}_job
    SOURCES ${SUITE_DIR}/spu/job_extras_job.c
    JOBBIN_WRAP
    CFLAGS -mspurs-job
    LDFLAGS -mspurs-job -Wl,-q
    LIBS spurs)
target_compile_definitions(spurs-job-extras PRIVATE
    "SUITE_JOB_HEADER=\"${SUITE_SPU}_job_bin.h\""
    "SUITE_JOB_BIN=${SUITE_SPU}_job_bin"
    "SUITE_JOB_BIN_SIZE=${SUITE_SPU}_job_bin_size"
    "SUITE_JOB_JOBHEADER=${SUITE_SPU}_job_jobheader_bin"
    "SUITE_JOB_JOBHEADER_HEADER=\"${SUITE_SPU}_job_jobheader_bin.h\"")
suite_self(spurs-job-extras)
