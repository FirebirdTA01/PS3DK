    .section .toc,"aw",@progbits
.Ltoc0:
    .quad target_sym

    .section .text.mid_func,"ax",@progbits
mid_func:
    addis 9, 2, (.toc+4)@toc@ha
    lwz 9, (.toc+4)@toc@l(9)
    blr
