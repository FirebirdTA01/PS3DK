# SPU GCC 16: forward-porting the Cell SPE backend

This is a design and status document, for contributors, on the forward-port of
the PS3 SPU (Cell Broadband Engine SPE) GCC backend from GCC 9.5.0 - the last
release that carried it - onto GCC 16.1.0, so that PPU and SPU share one GCC
release line.  GCC 9.5 remains the shipped SPU compiler until the gates below
pass.  The companion plan document is `docs/design/gcc-transition.md` (stage 3).
Nothing here refers to any proprietary SDK.

## 1. Goal and scope

Restore the SPU backend to a modern GCC release (16.1.0) without changing the
PS3 SPU ABI.  The upstream SPU backend was removed; this port recovers it by
restoring the 9.5 backend sources and porting the project's patch series onto
the 16.1.0 tree, then rebuilding the SDK's SPU objects and re-running the SPU
regression rows against the 9.5 baseline.  Scope is narrow: the SPU compiler
and its ABI; the PPU compiler is unchanged.  9.5 stays shipped until the gates
are met.

## 2. What was restored, and how

The upstream removal is commit `2f2aeda98f3` (2019-09-03, "Remove Cell
Broadband Engine SPU targets"); 273 files, about 34,000 lines.  The backend
itself - `gcc/config/spu` - is 21,104 lines.

Restore, not revert: the backend was brought back by copying the 9.5 release
sources, not by undoing the upstream removal commit.  Restored from
`releases/gcc-9.5.0`:

- `gcc/config/spu`
- `gcc/common/config/spu`
- `libgcc/config/spu`

Still present in the GCC 16 tree, not part of the removal, and therefore not
restored:

- the top-level `configure` `spu` stanza
- `config.sub`
- `real.cc` `spu_single_format`
- `rs6000/spu2vmx.h`

`gcc/config.gcc` listed `spu` as an unsupported target.  That entry is removed,
and the following are re-added: the `cpu_type`, `target` and the
`--with-arch` / `--with-tune` (cell, celledp) stanzas.  `spu` is added to the
`gcc/configure` (`.ac`) nop list, and the `libgcc/config.host` stanzas are
re-added.

Register allocation is unchanged from 9.5: the SPU backend uses the old reload
pass (`TARGET_LRA_P` false).  GCC 16 still ships reload (about 18 ports use
it), so no LRA conversion is needed for GCC 16.  The SPU backend never used
`cc0`.  Converting to LRA remains an open item for later, before GCC 17
drops reload; see section 9.

## 3. API changes ported

Each row is one upstream change that a 9.5-era backend source no longer
compiles against, and what the port does about it.  "GCC <n>" is the release
the change landed in.

| Change | GCC version | What the SPU code does now |
|---|---|---|
| `.c` to `.cc` renames in `gcc/config/spu` | 12 | sources are C++ files |
| `spu.opt.urls` option-URL file is required beside every `.opt` | 14 | added |
| `FLOAT`/`DOUBLE`/`LONG_DOUBLE_TYPE_SIZE` macros are poisoned (no target may set them) | 15 | dropped; the default `TARGET_C_MODE_FOR_FLOATING_TYPE` gives SF/DF/DF, which is what the SPU ABI wants |
| `Report` option property removed | 11 | the `Report:` property is dropped from the option records the SPU backend declares |
| `params.h` and `TARGET_OPTION_DEFAULT_PARAMS` removed | 10 | replaced with `SET_OPTION_IF_UNSET` in the option override: `max-completely-peel-times` 4 when not unrolling, `max-pending-list-length` 128 |
| `resolve_overloaded_builtin` gained a `complain` argument | 15 | the three SPU overload diagnostics are emitted only when `complain` is set |
| the attribute table became `TARGET_GNU_ATTRIBUTES` | 14 | the SPU attributes are registered through `TARGET_GNU_ATTRIBUTES` |
| `smallest_int_mode_for_size` returns an `opt_scalar_int_mode` (`.require()`) | - | the return type is the `opt_scalar_int_mode` |
| `va_arg` uses `pass_va_arg_by_reference`, with `function_arg_info` | 10 | the SPU va-arg path is rewritten to `function_arg_info` |
| `emit_conditional_move` takes an `rtx_comparison` (not separate mode/cond) | 12 | the SPU `emit_conditional_move` signature is updated |
| a zero-operand insn generator is called with no arguments | - | the SPU zero-operand `define_insn` generators are called with no arguments |
| the vectorizer `init`/`add`/`finish`/`destroy` cost hooks were replaced by the `vector_costs` class | 12 | dropped; the default `vector_costs` behaves as the SPU's old `add_stmt_cost` did |
| argument hooks take `function_arg_info` (`function_arg`, `function_arg_advance`, `pass_by_reference`); `setup_incoming_varargs` additionally skips the advance for a C23 function with no named argument (`TYPE_NO_NAMED_ARGS_STDARG_P`) | 10, 13 | the SPU argument hooks are all rewritten to the new signature |
| legitimate-address hooks take a `code_helper` | 14 | the SPU legitimate-address hooks take a `code_helper` |
| `dbxelf.h` removed from `tm_file` lists | 13 (removed upstream) | dropped from the SPU `tm_file` |

## 4. Generic GCC fixes the port needed (two)

Two generic fixes, in shared GCC code, that the port needed.

- **Bit-field SUBREGs of hard registers.**  `store_bit_field_using_insv` and
  `extract_bit_field_using_extv` in `gcc/expmed.cc` created a lowpart SUBREG of
  a register without `validate_subreg` in their REG branch (their SUBREG branch
  already checked).  Since GCC 10+, `validate_subreg` checks hard registers via
  `simplify_subreg_regno` (using `can_change_mode_class`), and the SPU ABI
  forbids 8-byte to 16-byte mode changes, so any complex-float argument or
  return (SCmode in a hard register) asserted in `gen_rtx_SUBREG`.  The fix:
  give up (return false / NULL_RTX) when the subreg is invalid; the non-insv
  bit-field paths then handle it.  The `__real__` / `__imag__` code path is
  identical to 9.5.

- **The `Report` property.**  The `Report:` property was removed in GCC 11
  (section 3).  The option records this port adds therefore drop `Report:`, so
  the series applies against a 16.1.0 tree that has no `Report:` column.

## 5. Behaviour kept from 9.5 on purpose

Three behaviours are deliberately left as they were in 9.5, with an
explanation for each.

- **`-ftree-loop-distribute-patterns` stays off below `-O3` unless the user
  requests it.**  Since GCC 10 the pass is on at `-O2` / `-Os`, and it turns
  copy and clear loops into `memcpy` / `memset` calls.  Much SPU object code
  links without a C library and lives in the 256 KB local store; a PSL1GHT MARS
  task module failed to link when the distributed patterns emitted the
  `memcpy` reference.  Measured: the 16 port is equal to 9.5 at `-Os`,
  `-O2` and `-O3`.

- **C++ defaults to GNU C++17.**  GCC 16's default is GNU C++20.  The SPU
  compiler keeps 9.5's behaviour: when neither `-std=` nor `-ansi` is given,
  the compiler selects `-std=gnu++17`.  `libsupc++` keeps building as GNU
  C++20 (it is a library, not user code on the SPU target).  This matches the
  PPU compiler's rule, so PPU and SPU have the same user-facing default.

- **Decimal float: rejected.**  9.5 predefined the `__DEC32_*` macros but
  rejected `_Decimal32`; GCC 16 defines neither the macros nor the type.  Both
  reject the type; this is the SPU ABI behaviour and is kept.

## 6. The project patch series on GCC 16

`patches/spu/gcc-9.5.0/` is the 15-patch series against the 9.5 backend.  It
applies to the 16.1.0 tree in order:

| Patch | Result on 16.1.0 |
|---|---|
| 10 of the 15 | unchanged |
| 0003, 0011, 0012, 0016 | context offsets only (the hunks are unchanged) |
| 0013 (intptr types) | hand-ported: the `config.gcc` line moves with the `cpu_type` stanza |
| the `Report` property | removed from the option records the series adds (the property no longer exists) |
| 0014 (parenthesised vector literals in the C and C++ parsers) | applies unchanged |

The ABI after the series is checked by static asserts in the port's own test:
plain `char` signed; `wchar_t` 16-bit; `intptr_t` `int`; `uintptr_t`
`unsigned int`; `size_t` `unsigned long`; `(vec_int4)(1,2,3,4)` vector-literal
syntax; `spu_splats((uintptr_t)p)`.

## 7. Build configuration

- **newlib 4.4.0**, with `patches/spu/newlib-4.x/` (5 patches; 0005 passes the
  fourth thread argument to `main`).  newlib 4.4 has two implicit function
  declarations (`signal.c` `getpid`, `__dprintf.c` `write`); these are errors
  since GCC 14, so newlib itself is built with
  `-Wno-error=implicit-function-declaration` until it is rebuilt against a
  newer newlib (4.6 or later).
- **libstdc++** needs `--disable-libstdcxx-backtrace`: `std::stacktrace`
  requires `mmap`, which the SPU does not have.
- **`--with-newlib`** makes libstdc++ use its fixed newlib configuration;
  the crossconfig SPU block that 16 removed is not needed.
- **`--enable-multilib`** keeps the pic multilib (series patch 0010).

## 8. Validation status and gates

| Stage | Contents | Status |
|---|---|---|
| 0 | restore + `configure` | DONE |
| 1 | `cc1` builds | DONE |
| 2 | `libgcc`, `libc`, `libm`; a C program links | DONE (text within 1% of 9.5) |
| 3 | the 15-patch series, ABI, C++ with libstdc++, `gnu++17` default; a C++17 program links | mostly DONE (text 152,072 vs 139,088 bytes with 9.5, +9.3%). OPEN: C/C++17 regression rows, restoring `gcc.target/spu` tests, regenerating the series as `patches/spu/gcc-16.x/` |
| 4 | SDK rebuilt with the GCC 16 SPU compiler: all SDK SPU objects built by GCC 16; 24 SPU regression rows in RPCS3 against 9.5 | IN PROGRESS. OPEN: the first run had failures traced to a missing newlib patch and a missing pic multilib in the test build; the rerun is pending |

The size/speed A/B against 9.5 on the 256 KB local store is OPEN; it has not
been run as of this writing.

## 9. Open items

- **Stage 3 (table above).**  The C/C++17 regression rows, the
  `gcc.target/spu` testsuite restore, and regenerating the series as
  `patches/spu/gcc-16.x/`.
- **Stage 4 (table above).**  The failed first run (missing newlib patch,
  missing pic multilib) and the pending rerun.
- **Size/speed A/B against 9.5**  on the 256 KB local store.
- **LRA conversion before GCC 17.**  The SPU backend uses the old reload
  pass, which GCC 16 still ships (about 18 ports use it).  GCC 17 drops
  reload, so the backend would need `TARGET_LRA_P` with its constraints and
  secondary reloads reworked before then.  This is optional for the 16 port
  and is the main open risk for a future 17 port.  The SPU backend never used
  `cc0`, which simplifies the secondary-reload rework.
