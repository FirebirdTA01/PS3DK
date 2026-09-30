# hello-daisy - libdaisy data streams: a local stream on the PPU, one between
# two PPU threads, one inside an SPU thread, and atomic streams from the PPU
# to two SPUs and from one SPU to another, and a signal-notification stream
# between two SPUs' local stores.
cmake_minimum_required(VERSION 3.20)
project(hello-daisy CXX ASM)

include(ps3-self)

add_executable(hello-daisy source/main.cpp source/local_stream.cpp)
target_compile_features(hello-daisy PRIVATE cxx_std_11)
target_include_directories(hello-daisy PRIVATE source)
target_link_libraries(hello-daisy PRIVATE c_stub rt lv2)

ps3_add_spu_image(hello-daisy
    NAME spu_local
    SOURCES spu/local.cpp)

ps3_add_spu_image(hello-daisy
    NAME spu_atomic
    SOURCES spu/atomic.cpp)

ps3_add_spu_image(hello-daisy
    NAME spu_snr
    SOURCES spu/snr.cpp)

ps3_add_self(hello-daisy)
