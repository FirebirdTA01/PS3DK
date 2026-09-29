# a C++ job (function-local static, global constructor, virtual call) built
# with the -mspurs-job-initialize driver mode alone, as g++ links it
suite_ppu(spurs-job-cpp-driver job_chain.cpp)
ps3_add_spu_image(spurs-job-cpp-driver
    NAME ${SUITE_SPU}
    SOURCES ${SUITE_DIR}/spu/job_cpp_driver.cpp
    JOBBIN
    CFLAGS -mspurs-job-initialize
    LDFLAGS -mspurs-job-initialize -Wl,-q)
suite_self(spurs-job-cpp-driver)
