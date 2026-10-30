# hello-debug-rpcs3: debug a PPU program in RPCS3 with the SDK's GDB.
#
#   cmake -S . -B build -G Ninja -DCMAKE_TOOLCHAIN_FILE=<PS3DK>/cmake/ps3-ppu-toolchain.cmake
#   cmake --build build                         # builds the SELF
#   cmake --build build --target debug-hello-debug-rpcs3   # boots RPCS3 and attaches GDB
#
# See docs/debugging-rpcs3.md for the RPCS3 limits (PPU interpreter only, no detach).
cmake_minimum_required(VERSION 3.20)
project(hello-debug-rpcs3 C)

include(ps3-self)

add_executable(hello-debug-rpcs3 source/main.c)
target_compile_features(hello-debug-rpcs3 PRIVATE c_std_11)
# Unoptimised with full debug info, so every local is visible in GDB.
target_compile_options(hello-debug-rpcs3 PRIVATE -O0 -g -Wall -Wextra -Werror)
target_link_libraries(hello-debug-rpcs3 PRIVATE sysutil rt lv2 m)

ps3_add_self(hello-debug-rpcs3)
ps3_add_debug_rpcs3(hello-debug-rpcs3 BREAK accumulate)
