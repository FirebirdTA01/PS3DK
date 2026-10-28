    .section .toc,"aw",@progbits
.Ltoc0:
    .quad target_sym

    .section .text.clean_func,"ax",@progbits
clean_func:
    addis 9, 2, .toc@toc@ha
    ld 9, .toc@toc@l(9)
    blr
