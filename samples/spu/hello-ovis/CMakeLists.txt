# hello-ovis - SPU code overlays with libovis.
#
# Two SPU programs share four overlaid functions (spu/op_*.c):
#   ovis_manual  sections listed in spu/manual.xml, two regions (add/mul and
#                fib/gcd share LS), mapped by hand with the mapper macros
#   ovis_auto    every function wrapped (OVIS_AUTO), one region for all four,
#                mapped on each call
# The PPU builds each program's overlay table, drops the overlay segments
# from its image, runs it and checks what it reports.
cmake_minimum_required(VERSION 3.20)
project(hello-ovis C ASM)

include(ps3-self)

add_executable(hello-ovis source/main.c)
target_compile_features(hello-ovis PRIVATE c_std_11)
target_link_libraries(hello-ovis PRIVATE ovis c_stub rt lv2)
target_include_directories(hello-ovis PRIVATE spu)

set(_ops spu/op_add.c spu/op_mul.c spu/op_fib.c spu/op_gcd.c)

ps3_add_spu_image(hello-ovis
    NAME ovis_manual
    SOURCES spu/manual.c ${_ops}
    OVIS_CONFIG spu/manual.xml)

ps3_add_spu_image(hello-ovis
    NAME ovis_auto
    SOURCES spu/auto.c ${_ops}
    OVIS_AUTO ${_ops})

ps3_add_self(hello-ovis)
