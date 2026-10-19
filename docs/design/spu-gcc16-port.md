# SPU GCC 16: forward-porting the Cell SPE backend

This is a design and status document, for contributors, on the forward-port of
the PS3 SPU (Cell Broadband Engine SPE) GCC backend from GCC 9.5.0 - the last
release that carried it - onto GCC 16.1.0, so that PPU and SPU share one GCC
release line.  GCC 9.5 remains the shipped SPU compiler until the gates below
pass.  The port itself is the series in `patches/spu/gcc-16.x/`, applied by
`scripts/build-spu-toolchain.sh --gcc=16`.  The companion plan document is `docs/design/gcc-transition.md` (stage 3).

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
`cc0`.  Converting to LRA is an open item for whichever release removes
reload; see section 9.

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

## 4. Generic GCC fixes the port needed

Two fixes in shared GCC code (`gcc-16.x` patches 0017 and 0018):

- **Bit-field SUBREGs of hard registers.**  `store_bit_field_using_insv` and
  `extract_bit_field_using_extv` in `gcc/expmed.cc` created a lowpart SUBREG of
  a register without `validate_subreg` in their REG branch (their SUBREG branch
  already checked).  In GCC 16, `validate_subreg` checks hard registers through
  `can_change_mode_class`, and the SPU forbids 8-byte to 16-byte mode changes,
  so any complex-float argument or return (SCmode in a hard register) asserted
  in `gen_rtx_SUBREG`.  The fix gives up (returns false / `NULL_RTX`) when the
  subreg is invalid; the non-insv bit-field paths then handle it.

## 5. Behaviour kept from 9.5 on purpose

- **`-ftree-loop-distribute-patterns` stays off below `-O3` unless asked for**
  (patch 0038).  Since GCC 10 the pass runs at `-O2` / `-Os`, and it
  turns copy and clear loops into `memcpy` / `memset` calls.  Much SPU code
  links without a C library and lives in the 256 KB local store; a PSL1GHT MARS
  task module failed to link on the `memcpy` reference the pass emitted.

- **late-combine stays off unless asked for** (patch 0038).  The late-combine
  pass (new in GCC 15) dropped DImode-to-SImode truncations on SPU:
  `return (int)u64;` returned the high word.  It was found by bisecting a
  failing SDK library object by object, then toggling passes on that object.
  The underlying fault is OPEN; until it is understood, the pass is off.

- **C++ defaults to GNU C++17** (patches 0035, 0036).  GCC 16's default is GNU
  C++20.  When neither `-std=` nor `-ansi` is given, the SPU compiler selects
  `-std=gnu++17`, the same rule as the PPU compiler.  `libsupc++` keeps
  building as GNU C++20.

- **Decimal float: rejected.**  9.5 predefined the `__DEC32_*` macros but
  rejected `_Decimal32`; GCC 16 defines neither.  Both reject the type.

## 6. The project patch series on GCC 16

`patches/spu/gcc-16.x/` holds 38 patches:

| Patches | Contents |
|---|---|
| 0001-0016 | wiring the restored backend into the build, and the API ports of section 3 |
| 0017-0018 | the generic `expmed.cc` fixes of section 4 |
| 0019-0033 | the 15-patch `patches/spu/gcc-9.5.0/` series, in its original order: 14 applied as they were, 0013 (intptr types, now 0033) ported by hand |
| 0034 | the `Report` property dropped from the option records the 9.5 series adds |
| 0035-0036 | the GNU C++17 default of section 5 |
| 0037 | target builtins read `DECL_MD_FUNCTION_CODE` |
| 0038 | the pass defaults of section 5, set in the target optimization table so that `optimize` attributes and pragmas get them too |

The restored 9.5 backend itself is not in the series: the build script copies
`gcc/config/spu`, `gcc/common/config/spu` and `libgcc/config/spu` from
`releases/gcc-9.5.0` before applying it.

The ABI after the series is checked by static asserts: plain `char` signed;
`wchar_t` 16-bit; `intptr_t` `int`; `uintptr_t` `unsigned int`; `size_t`
`unsigned long`; `(vec_int4)(1,2,3,4)` vector-literal syntax;
`spu_splats((uintptr_t)p)`.

## 7. Build configuration

- **newlib 4.4.0**, with `patches/spu/newlib-4.x/` (5 patches; 0005 passes the
  fourth thread argument to `main`).  newlib 4.4 has two implicit function
  declarations (`signal.c` `getpid`, `__dprintf.c` `write`); these are errors
  since GCC 14, so newlib is built with
  `-Wno-error=implicit-function-declaration`.
- **libstdc++** needs `--disable-libstdcxx-backtrace`: `std::stacktrace`
  requires `mmap`, which the SPU does not have.
- **`--with-newlib`** makes libstdc++ use its fixed newlib configuration.
- **`--enable-multilib`** keeps the pic multilib (patch 0027).
- The GCC 16 build uses its own build root (`spu-gcc16`, and
  `spu-gcc16-<host>` for a cross-hosted build), so it never shares a tree with
  the 9.5 build; `--clean` removes only the selected root.

## 8. Validation status

| Stage | Contents | Status |
|---|---|---|
| 0 | restore + `configure` | DONE |
| 1 | `cc1` builds | DONE |
| 2 | `libgcc`, `libc`, `libm`; a C program links | DONE |
| 3 | the series, ABI asserts, C++ with libstdc++, `gnu++17` default | DONE |
| 4 | SDK rebuilt with the GCC 16 SPU compiler (built by the script from a clean tree); the SPU regression rows in RPCS3 | 25/25 PASS, the same as the 9.5 control, on the compiler built from the first version of the series (39 patches, before the option-table change); not yet rerun on the current series |

Size against 9.5, on that same compiler: SPU images +1.09%, the SDK SPU
libraries +2.27%.

The current series (38 patches) was checked separately: built by the script
from a clean tree; the option-default test passes in compiler mode; five probe
sources give the same assembly as the earlier compiler at `-O0`, `-O2`, `-Os`
and `-O3`; and `libc`/`libg` code is unchanged, while `libgcc` differs in one
member (`hardcfr.o`, whose `#pragma GCC optimize` no longer turns the two
passes back on).  The stage-4 rows have not been rerun on it.

## 9. Open items

- Restoring the `gcc.target/spu` testsuite.
- The late-combine fault (section 5).
- A Windows-hosted SPU GCC 16 cross build, and release packaging.
- Hardware runs of SPU code built by GCC 16.
- **LRA.**  The SPU backend uses the old reload pass, which GCC 16 still
  ships.  When upstream removes reload, the backend needs `TARGET_LRA_P` with
  its constraints and secondary reloads reworked.  The SPU backend never used
  `cc0`.
