# (vector-type)(e1, ..., en) literals and (vector-type)(scalar) splats,
# checked lane by lane on an SPU thread (no -Werror: the short-list check
# draws the intended "incorrect number of vector elements" warning)
suite_ppu(spu-vector-literals vector_literals.cpp)
ps3_add_spu_image(spu-vector-literals
    NAME ${SUITE_SPU}
    SOURCES ${SUITE_DIR}/spu/vector_literals.c
    CFLAGS -O2 -Wall -Wextra)
suite_self(spu-vector-literals)
