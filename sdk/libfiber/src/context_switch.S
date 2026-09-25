/* SPU fiber context switch.
 *
 *   void __ps3dk_fiber_swap(CellFiberSpuContext *save, CellFiberSpuContext *load)
 *
 * Saves the registers the SPU ABI requires a callee to preserve - $80-$127,
 * the link register $0 and the stack pointer $1 - into *save, loads them
 * from *load, and branches to the loaded link register.  Everything else is
 * caller-saved, so a C caller sees an ordinary function call that returns
 * when some other context switches back to *save.
 *
 * Layout of the opaque 896-byte context (offsets used here and in
 * fiber_spu_context.c): $80-$127 at 0x000-0x2ff, $0 at 0x300, $1 at 0x310.
 */
        .text
        .align  3
        .global __ps3dk_fiber_swap
        .type   __ps3dk_fiber_swap, @function
__ps3dk_fiber_swap:
        stqd    $80,   0($3)
        stqd    $81,  16($3)
        stqd    $82,  32($3)
        stqd    $83,  48($3)
        stqd    $84,  64($3)
        stqd    $85,  80($3)
        stqd    $86,  96($3)
        stqd    $87, 112($3)
        stqd    $88, 128($3)
        stqd    $89, 144($3)
        stqd    $90, 160($3)
        stqd    $91, 176($3)
        stqd    $92, 192($3)
        stqd    $93, 208($3)
        stqd    $94, 224($3)
        stqd    $95, 240($3)
        stqd    $96, 256($3)
        stqd    $97, 272($3)
        stqd    $98, 288($3)
        stqd    $99, 304($3)
        stqd    $100, 320($3)
        stqd    $101, 336($3)
        stqd    $102, 352($3)
        stqd    $103, 368($3)
        stqd    $104, 384($3)
        stqd    $105, 400($3)
        stqd    $106, 416($3)
        stqd    $107, 432($3)
        stqd    $108, 448($3)
        stqd    $109, 464($3)
        stqd    $110, 480($3)
        stqd    $111, 496($3)
        stqd    $112, 512($3)
        stqd    $113, 528($3)
        stqd    $114, 544($3)
        stqd    $115, 560($3)
        stqd    $116, 576($3)
        stqd    $117, 592($3)
        stqd    $118, 608($3)
        stqd    $119, 624($3)
        stqd    $120, 640($3)
        stqd    $121, 656($3)
        stqd    $122, 672($3)
        stqd    $123, 688($3)
        stqd    $124, 704($3)
        stqd    $125, 720($3)
        stqd    $126, 736($3)
        stqd    $127, 752($3)
        stqd    $0,   768($3)
        stqd    $1,   784($3)

        lqd     $80,   0($4)
        lqd     $81,  16($4)
        lqd     $82,  32($4)
        lqd     $83,  48($4)
        lqd     $84,  64($4)
        lqd     $85,  80($4)
        lqd     $86,  96($4)
        lqd     $87, 112($4)
        lqd     $88, 128($4)
        lqd     $89, 144($4)
        lqd     $90, 160($4)
        lqd     $91, 176($4)
        lqd     $92, 192($4)
        lqd     $93, 208($4)
        lqd     $94, 224($4)
        lqd     $95, 240($4)
        lqd     $96, 256($4)
        lqd     $97, 272($4)
        lqd     $98, 288($4)
        lqd     $99, 304($4)
        lqd     $100, 320($4)
        lqd     $101, 336($4)
        lqd     $102, 352($4)
        lqd     $103, 368($4)
        lqd     $104, 384($4)
        lqd     $105, 400($4)
        lqd     $106, 416($4)
        lqd     $107, 432($4)
        lqd     $108, 448($4)
        lqd     $109, 464($4)
        lqd     $110, 480($4)
        lqd     $111, 496($4)
        lqd     $112, 512($4)
        lqd     $113, 528($4)
        lqd     $114, 544($4)
        lqd     $115, 560($4)
        lqd     $116, 576($4)
        lqd     $117, 592($4)
        lqd     $118, 608($4)
        lqd     $119, 624($4)
        lqd     $120, 640($4)
        lqd     $121, 656($4)
        lqd     $122, 672($4)
        lqd     $123, 688($4)
        lqd     $124, 704($4)
        lqd     $125, 720($4)
        lqd     $126, 736($4)
        lqd     $127, 752($4)
        lqd     $0,   768($4)
        lqd     $1,   784($4)
        bi      $0
        .size   __ps3dk_fiber_swap, .-__ps3dk_fiber_swap

/* First resume of a context prepared by cellFiberSpuContextInitialize.
 *
 * Initialize leaves the entry function in $80, its argument in $81 and the
 * context's own address in $82 - callee-saved registers, so they survive
 * the entry call - and the stack pointer 32 bytes below the top of the
 * supplied stack (the back-chain quadword and the link-register slot the
 * entry function's prologue writes).  This code uses no stack of its own:
 * the only stack the runtime touches is that 32-byte reserve.
 *
 * When the entry function returns, the fiber run is over: clear the
 * running-fiber pointer and switch back to the context that called
 * cellFiberSpuContextRun (a tail branch - swap never returns here).
 */
        .text
        .align  3
        .global __ps3dk_fiber_start
        .type   __ps3dk_fiber_start, @function
__ps3dk_fiber_start:
        ori     $3, $81, 0              /* arg: the 64-bit value in $81 */
        bisl    $0, $80                 /* entry(arg) */

        ila     $4, __ps3dk_fiber_current
        lqd     $5, 0($4)
        cwd     $6, 0($4)               /* insert-word control for that address */
        il      $7, 0
        shufb   $5, $7, $5, $6
        stqd    $5, 0($4)               /* __ps3dk_fiber_current = NULL */

        ila     $4, __ps3dk_fiber_run_caller
        lqd     $5, 0($4)
        rotqby  $4, $5, $4              /* the pointer into the preferred slot */
        ori     $3, $82, 0              /* save into this fiber's own context */
        br      __ps3dk_fiber_swap      /* swap(self, run_caller) */
        .size   __ps3dk_fiber_start, .-__ps3dk_fiber_start
