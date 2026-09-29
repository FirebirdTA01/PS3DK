# a C++ job (function-local static, global constructor, virtual call) built
# with the -mspurs-job-initialize driver mode alone, as g++ links it
suite_ppu(spurs-job-cpp-driver job_chain.cpp)
ps3_add_spu_image(spurs-job-cpp-driver
    NAME ${SUITE_SPU}
    SOURCES ${SUITE_DIR}/spu/job_cpp_driver.cpp
    JOBBIN_WRAP
    CFLAGS -mspurs-job-initialize
    LDFLAGS -mspurs-job-initialize -Wl,-q)
target_compile_definitions(spurs-job-cpp-driver PRIVATE SUITE_JOB_FROM_HEADER)
suite_self(spurs-job-cpp-driver)
