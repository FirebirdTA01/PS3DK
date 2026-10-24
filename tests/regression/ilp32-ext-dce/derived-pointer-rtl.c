/* Start immediately before ext_dce to preserve the pointer+offset RTL.
   Input upper bits are arbitrary. The first zero-extension makes <1> a
   valid Pmode pointer; <2> derives from it by arithmetic, not a copy.
   Inputs used by the runtime test do not wrap when four is added.
   Removing the first extension invalidates REG_POINTER on BOTH <1> and
   <2>. The final zero-extension must still produce zero upper bits. */
unsigned long long __RTL (startwith ("ud_dce")) derived_pointer (unsigned long long x)
{
(function "derived_pointer"
  (param "x" (DECL_RTL (reg/v:DI <0>))
             (DECL_RTL_INCOMING (reg:DI 3)))
  (insn-chain
    (block 2
      (edge-from entry (flags "FALLTHRU"))
      (cnote 1 [bb 2] NOTE_INSN_BASIC_BLOCK)
      (cinsn 2 (set (reg:DI <0>) (reg:DI 3)))
      (cinsn 3 (set (reg/f:DI <1>)
                   (zero_extend:DI (subreg:SI (reg:DI <0>) 4))))
      (cinsn 4 (set (reg/f:DI <2>)
                   (plus:DI (reg/f:DI <1>) (const_int 4))))
      (cinsn 5 (set (reg:DI <3>)
                   (zero_extend:DI (subreg:SI (reg/f:DI <2>) 4))))
      (cinsn 6 (set (reg/i:DI 3) (reg:DI <3>)))
      (cinsn 7 (use (reg/i:DI 3)))
      (edge-to exit (flags "FALLTHRU"))
    )
  )
  (crtl (return_rtx (reg/i:DI 3)))
)
}
