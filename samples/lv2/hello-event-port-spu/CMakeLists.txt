# hello-event-port-spu - exercise the SPU lv2 event-port syscalls
# (send_event / throw_event / receive_event / tryreceive_event /
# group_yield).  PPU sets up two event queues + a port + the SPU
# thread's port mappings, then drives a 6-step round-trip.
#
# Build:
#   cmake -S . -B build \
#     -DCMAKE_TOOLCHAIN_FILE=<repo>/cmake/ps3-ppu-toolchain.cmake
#   cmake --build build

cmake_minimum_required(VERSION 3.20)
project(hello-event-port-spu C ASM)

include(ps3-self)

add_executable(hello-event-port-spu source/event_port.c)
target_compile_features(hello-event-port-spu PRIVATE c_std_11)

target_link_libraries(hello-event-port-spu PRIVATE
    c_stub rt lv2 m
)

ps3_add_spu_image(hello-event-port-spu
    NAME spu_ep
    SOURCES spu/source/spu_ep.c
    LIBS sputhread)

ps3_add_self(hello-event-port-spu)
