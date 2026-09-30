# tests/simdmath-inline/CMakeLists.txt
#
# Validates that the libsimdmath per-arch inlines (installed by
# sdk/libsimdmath/Makefile) are reachable from a plain #include <simdmath.h>
# WITHOUT linking -lsimdmath.  This is the exact acceptance the card calls
# out: "samples call sincosf4()/divf4()/rsqrtf4() + one fast form and link
# without -lsimdmath".
#
# PPU side  : spawn the embedded SPU ELF, fetch the SPU's exit code.
# SPU side  : C file compiled with the SPU toolchain, includes <simdmath.h>,
#             calls sincosf4(), divf4(), rsqrtf4(), divf4fast(),
#             checks values vs the scalar sinf/cosf/1.0f/sqrtf,
#             exits 0 if every lane matches within 1.5 ULP, 1 otherwise.
#
# This is the SPU test the card describes.  It deliberately does NOT link
# -lsimdmath - so if the inlines are not actually visible, the link fails.

cmake_minimum_required(VERSION 3.20)
project(simdmath-inline C ASM)

include(ps3-self)

add_executable(simdmath-inline source/main.c)
target_compile_features(simdmath-inline PRIVATE c_std_11)
target_link_libraries(simdmath-inline PRIVATE rt lv2 m)

# The whole point of this test: no simdmath in LIBS.  The SPU image must
# resolve sincosf4/divf4/rsqrtf4 from the inline headers, not from an
# archive.
ps3_add_spu_image(simdmath-inline
    NAME simdmath_inline_spu
    SOURCES spu_source/main.c
    LIBS sputhread)     # no simdmath

ps3_add_self(simdmath-inline)
