#ifndef PPU_COMPARE_CARRY_ABI_H
#define PPU_COMPARE_CARRY_ABI_H

#ifndef PPU_COMPARE_CARRY_ABI_BYTES
#error "The build must declare the expected pointer/long width for each TU"
#endif
_Static_assert(PPU_COMPARE_CARRY_ABI_BYTES == 4 || PPU_COMPARE_CARRY_ABI_BYTES == 8,
               "expected ABI width must be 4 or 8");
_Static_assert(sizeof(void*) == PPU_COMPARE_CARRY_ABI_BYTES, "wrong pointer ABI");
_Static_assert(sizeof(long) == PPU_COMPARE_CARRY_ABI_BYTES, "wrong long ABI");
_Static_assert(sizeof(int) == 4, "wrong int ABI");

#endif
