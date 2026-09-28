# job extras from the SPU: urgent commands and calls, the grab limit,
# SetJobbin2Param (checked against the PPU's), and the job memory check
suite_ppu(spurs-job-extras job_extras.cpp)
suite_task_manual(spurs-job-extras job_extras_task.c)
# the jobs the chain runs: a raw job image
ps3_add_spu_image(spurs-job-extras
    NAME ${SUITE_SPU}_job
    SOURCES ${SUITE_DIR}/spu/job_extras_job.c
    JOBBIN
    CFLAGS -mspurs-job
    LDFLAGS -mspurs-job -Wl,-q
    LIBS spurs)
# a jobbin2 image, only as SetJobbin2Param's subject
ps3_add_spu_image(spurs-job-extras
    NAME ${SUITE_SPU}_jb2
    SOURCES ${SUITE_DIR}/spu/job_queue.c
    NOSTARTFILES
    JOBBIN_WRAP
    CFLAGS -mspurs-job-initialize
    LDFLAGS -mspurs-job-initialize -Wl,-q -Wl,--gc-sections
    LDSCRIPT ${PS3DK}/spu/ldscripts/spurs_job.ld
    LIBS spurs_jq)
target_compile_definitions(spurs-job-extras PRIVATE
    "SUITE_JOB_HEADER=\"${SUITE_SPU}_job_bin.h\""
    "SUITE_JOB_BIN=${SUITE_SPU}_job_bin"
    "SUITE_JOB_BIN_SIZE=${SUITE_SPU}_job_bin_size"
    "SUITE_JB2_HEADER=\"${SUITE_SPU}_jb2_bin.h\""
    "SUITE_JB2_BIN=${SUITE_SPU}_jb2_bin")
suite_self(spurs-job-extras)
