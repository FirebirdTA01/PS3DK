# spumars - PSL1GHT upstream sample ported to our CMake build.
#
# Source: PSL1GHT samples/spu/spumars @ 5ba78ae, verbatim.
# Original native Makefiles replaced by this CMakeLists.

cmake_minimum_required(VERSION 3.20)
project(spumars C ASM)

include(ps3-self)

add_executable(spumars source/main.c)
target_compile_features(spumars PRIVATE c_std_11)

# PSL1GHT Makefile LIBS: -lspumarstask -lspumars -lrt -llv2 -lm
target_link_libraries(spumars PRIVATE
    spumarstask -lspumars rt lv2 m
)

ps3_add_spu_image(spumars
    NAME spu
    SOURCES spu/source/main.c
    # MARS task_map_elf requires the task text segment at LS 0x3a00.
    # Match PSL1GHT spu_rules TASK_LDFLAGS, rather than a thread image at 0.
    LDFLAGS -Wl,-Ttext-segment=0x3a00 -Wl,--local-store=0x3a00:0x3FFFF
    LIBS spumarstask spumars sputhread)

ps3_add_self(spumars)
