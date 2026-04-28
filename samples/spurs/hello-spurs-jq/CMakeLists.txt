# hello-spurs-jq - cell/spurs/job_queue*.h header validation.
#
# Compile-and-link probe that exercises every entry point declared in
# cell/spurs/job_queue.h, job_queue_port.h, job_queue_port2.h, and
# job_queue_semaphore.h against the existing libspurs_jq_stub.a NIDs.
# Doesn't run end-to-end (the SPU runtime is a later phase) - purpose
# is to surface header / struct ABI errors before the runtime work.
#
# Build:
#   cmake -S . -B build \
#     -DCMAKE_TOOLCHAIN_FILE=<repo>/cmake/ps3-ppu-toolchain.cmake
#   cmake --build build

cmake_minimum_required(VERSION 3.20)
project(hello-spurs-jq CXX)

include(ps3-self)

add_executable(hello-spurs-jq source/main.cpp)
target_compile_features(hello-spurs-jq PRIVATE cxx_std_17)

target_link_libraries(hello-spurs-jq PRIVATE
    spurs_jq_stub spurs_stub c_stub sysmodule_stub sysutil rt lv2 m
)

ps3_add_self(hello-spurs-jq)
