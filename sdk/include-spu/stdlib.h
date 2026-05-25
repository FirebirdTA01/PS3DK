/* stdlib.h wrapper (SPU) -- chains to newlib SPU sysroot via
 * #include_next, then adds the memalign declaration that
 * binary-compatible SPU samples expect in <stdlib.h>.
 * newlib provides the implementation (malloc.h) but not the
 * stdlib.h declaration.
 */
#ifndef _PS3DK_SPU_STDLIB_WRAPPER_H
#define _PS3DK_SPU_STDLIB_WRAPPER_H

#include_next <stdlib.h>

#ifdef __cplusplus
extern "C" {
#endif

void *memalign(size_t, size_t);

/* CELL_SPU_LS_PARAM(heap_size, stack_size) -- emit symbols that the SPU
 * CRT reads to size the heap and stack.  Expands to inline assembly
 * placing:
 *   _cell_spu_ls_param in .data (16 bytes: heap, stack, 0, 0)
 *   a .cell_spu_ls_param section (8 bytes: heap, stack, no label)
 * Two-level stringify so expressions like 16*1024 survive as literals.
 */
#define CELL_SPU_LS_PARAM_S_(x) #x
#define CELL_SPU_LS_PARAM_S(x)  CELL_SPU_LS_PARAM_S_(x)

#define CELL_SPU_LS_PARAM(heap, stack)                              \
    __asm__(".pushsection .data\n\t"                                 \
            ".align 4\n\t"                                           \
            ".globl _cell_spu_ls_param\n\t"                          \
            "_cell_spu_ls_param:\n\t"                                \
            ".long " CELL_SPU_LS_PARAM_S(heap) "\n\t"               \
            ".long " CELL_SPU_LS_PARAM_S(stack) "\n\t"              \
            ".long 0\n\t"                                            \
            ".long 0\n\t"                                            \
            ".popsection\n\t"                                        \
            ".pushsection .cell_spu_ls_param,\"aw\",@progbits\n\t"  \
            ".long " CELL_SPU_LS_PARAM_S(heap) "\n\t"               \
            ".long " CELL_SPU_LS_PARAM_S(stack) "\n\t"              \
            ".popsection")

#ifdef __cplusplus
}
#endif

#endif /* _PS3DK_SPU_STDLIB_WRAPPER_H */
