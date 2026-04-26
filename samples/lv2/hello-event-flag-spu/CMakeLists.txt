# hello-event-flag-spu — PPU master + 5 PPU workers + 1 SPU worker
# coordinating via sys_event_flag_*.  Tests our SPU-side libsputhread
# (sys_event_flag_set_bit / set_bit_impatient / spu_thread_exit).
#
# Build:
#   cmake -S . -B build \
#     -DCMAKE_TOOLCHAIN_FILE=<repo>/cmake/ps3-ppu-toolchain.cmake
#   cmake --build build

cmake_minimum_required(VERSION 3.20)
project(hello-event-flag-spu C ASM)

include(ps3-self)

add_executable(hello-event-flag-spu source/event_flag.c)
target_compile_features(hello-event-flag-spu PRIVATE c_std_11)
target_include_directories(hello-event-flag-spu PRIVATE source)

target_link_libraries(hello-event-flag-spu PRIVATE
    c_stub rt lv2 m
)

ps3_add_spu_image(hello-event-flag-spu
    NAME spu_ef
    SOURCES spu/source/spu_ef.c
    CFLAGS -Ispu/source
    LIBS sputhread)

ps3_add_self(hello-event-flag-spu)
