# hello-spu-simd — exercises the simd::* class wrappers in <simd>.
#
# The SPU side runs a battery of float4 / int4 / bool4 / double2 / llong2
# operations, packs the results into a 16-byte aligned buffer, DMAs it
# back to a PPU-side struct, and exits.  The PPU prints each lane and
# the expected value side-by-side so RPCS3 / hardware logs show pass/fail
# at a glance.
cmake_minimum_required(VERSION 3.20)
project(hello-spu-simd C CXX ASM)

include(ps3-self)

add_executable(hello-spu-simd source/main.c)
target_compile_features(hello-spu-simd PRIVATE c_std_11)

target_link_libraries(hello-spu-simd PRIVATE
    rt lv2 m
)

ps3_add_spu_image(hello-spu-simd
    NAME hello_spu_simd
    SOURCES spu/source/main.cpp
    LIBS sputhread simdmath m)

ps3_add_self(hello-spu-simd)
