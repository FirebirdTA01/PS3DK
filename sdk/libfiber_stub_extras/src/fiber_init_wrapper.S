/* sdk/libfiber_stub_extras/src/fiber_init_wrapper.S
 *
 * Assembly wrapper for cellFiberPpuInitialize that exports both the
 * .opd function descriptor (D) and the text entry-point dot symbol
 * (T), matching the reference archive's export shape.
 *
 * We call into the C helper _fiberPpuInitializeImpl (in
 * fiber_init_wrapper.c) which does the actual sysmodule load +
 * _cellFiberPpuInitialize forward.  This two-file split keeps the
 * inline-asm noise out of the C unit while giving us exactly the
 * nm ' T ' symbol the verification gate expects.
 */

.section ".opd","aw"
.globl cellFiberPpuInitialize
cellFiberPpuInitialize:
    .quad .cellFiberPpuInitialize
    .quad .TOC.@tocbase
    .quad 0

.section ".text.cellFiberPpuInitialize","ax"
.globl .cellFiberPpuInitialize
.type .cellFiberPpuInitialize, @function
.cellFiberPpuInitialize:
    mflr    0
    std     0, 16(1)
    stdu    1, -128(1)
    bl      _fiberPpuInitializeImpl
    nop
    addi    1, 1, 128
    ld      0, 16(1)
    mtlr    0
    blr
.size .cellFiberPpuInitialize, .-.cellFiberPpuInitialize
