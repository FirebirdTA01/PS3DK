# threadtest — PSL1GHT upstream sample ported to our CMake build.
#
# Source: PSL1GHT samples/sys/threadtest @ 5ba78ae, verbatim (trivial wrap —
# no shaders, no assets).  Original native Makefile replaced by this
# CMakeLists; build/link behaviour matches our other PPU samples.
#
# Build:
#   cmake -S . -B build -G Ninja \
#     -DCMAKE_TOOLCHAIN_FILE=<repo>/cmake/ps3-ppu-toolchain.cmake
#   cmake --build build

cmake_minimum_required(VERSION 3.20)
project(threadtest C)

include(ps3-self)

add_executable(threadtest source/main.c)
target_compile_features(threadtest PRIVATE c_std_11)

# PSL1GHT Makefile LIBS: -lrsx -lgcm_sys -lio -lsysutil -lrt -llv2 -lm
target_link_libraries(threadtest PRIVATE
    rsx gcm_sys io sysutil rt lv2 m
)

ps3_add_self(threadtest)
