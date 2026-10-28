    .section .text.noreloc_func,"ax",@progbits
    .globl noreloc_func
noreloc_func:
    li 3, 42
    blr
