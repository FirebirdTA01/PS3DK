cmake_minimum_required(VERSION 3.20)
project(hello-ppu-mlp64-types C)

include(ps3-self)

# Default ILP32 hybrid build.
add_executable(hello-ppu-mlp64-types-ilp32 source/main.c)
target_link_libraries(hello-ppu-mlp64-types-ilp32 PRIVATE sysutil m)
ps3_add_self(hello-ppu-mlp64-types-ilp32)

# -mlp64 multilib opt-in build.  Currently expected to fail at link
# until the multilib install tree is completed under powerpc64-ps3-elf/lib/lp64/
# and the SPRX stub archives are rebuilt with lp64-aware variants.
# See README.md.
add_executable(hello-ppu-mlp64-types-lp64 source/main.c)
target_compile_options(hello-ppu-mlp64-types-lp64 PRIVATE -mlp64)
target_link_options(hello-ppu-mlp64-types-lp64 PRIVATE -mlp64)
target_link_libraries(hello-ppu-mlp64-types-lp64 PRIVATE sysutil m)
ps3_add_self(hello-ppu-mlp64-types-lp64)
