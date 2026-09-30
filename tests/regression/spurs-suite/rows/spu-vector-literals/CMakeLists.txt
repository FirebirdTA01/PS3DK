# (vector-type)(e1, ..., en) literals and (vector-type)(scalar) splats,
# checked lane by lane on an SPU thread through both the C and the C++ front
# end (no -Werror: the short-list checks draw the intended "incorrect number
# of vector elements" warning)
suite_ppu(spu-vector-literals vector_literals.cpp)
ps3_add_spu_image(spu-vector-literals
    NAME ${SUITE_SPU}
    SOURCES ${SUITE_DIR}/spu/vector_literals.c ${SUITE_DIR}/spu/vector_literals_cpp.cpp
    CFLAGS -O2 -Wall -Wextra)
suite_self(spu-vector-literals)
