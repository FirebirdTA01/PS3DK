    .section ".text.start","ax",@progbits
    .globl _start
    .type _start,@function
    .align 2
_start:
    lis     2, __toc@highest
    ori     2, 2, __toc@higher
    rldicr  2, 2, 32, 31
    oris    2, 2, __toc@h
    ori     2, 2, __toc@l

    lis     12, main@highest
    ori     12, 12, main@higher
    rldicr  12, 12, 32, 31
    oris    12, 12, main@h
    ori     12, 12, main@l
    ld      0, 0(12)
    ld      2, 8(12)
    mtctr   0
    bctrl
1:
    b       1b
    .size _start, . - _start
