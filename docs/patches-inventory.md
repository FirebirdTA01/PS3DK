# ps3dev Patches Inventory & Rebase Baseline

Audit of the four patches shipped by ps3dev/ps3toolchain (master), with a line-by-line read of what each one does and how hard it will be to rebase onto the modern versions selected for this project.

Source: `src/ps3dev/ps3toolchain/patches/`

| Patch | Lines | Rebase target | Difficulty | Notes |
|---|---:|---|---|---|
| `binutils-2.22-PS3.patch` | 90 | binutils 2.42 | Low | 20 years of binutils between these versions. Expect most hunks to be obsolete; rewrite rather than rebase. |
| `gcc-7.2.0-PS3.patch` | 727 | GCC 12.4.0 (PPU) / GCC 9.5.0 (SPU) | Medium | Touches 7 files. `rs6000.c`→`rs6000.cc` rename is the biggest friction point. |
| `newlib-1.20.0-PS3.patch` | 13,180 | newlib 4.4.0.20231231 | Medium-High | Massive — this is where all PS3 `libsysbase`/`libgloss` glue lives. Mostly new-file additions (low conflict risk) but lots of surface area. |
| `gdb-8.3.1-PS3.patch` | 403 | GDB 14.2 | Low | PPU-only debugging is the goal; SPU combined-debug support was orphaned upstream post-2019 and is out of scope. |

---

## `gcc-7.2.0-PS3.patch` breakdown (727 lines)

Files touched:

| File | Kind | Hunks | Notes |
|---|---|---|---|
| `Makefile.in` | modify | 1 (1 line) | `install-libiberty` cleanup. Likely obsolete or already fixed upstream. |
| `gcc/config.gcc` | modify | 1 (~10 lines) | Adds `powerpc64-ps3-elf)` case setting `tm_file`, `tmake_file`, `extra_options`. |
| `gcc/config/rs6000/cell64lv2.h` | **new file** | 1 (587 lines) | Core PS3 target header. Bulk of the patch. |
| `gcc/config/rs6000/rs6000.c` | modify | 2 (~25 lines) | Ifdef'd `POWERPC_CELL64LV2` hook for `TARGET_VALID_POINTER_MODE`. **Renamed to `rs6000.cc` in GCC 12** — hunks must be relocated. |
| `gcc/configure` | modify | 1 | Generated file. **Do not hand-patch** — re-run `autoreconf -i` in `gcc/`. |
| `gcc/configure.ac` | modify | 1 (~4 lines) | Adds `powerpc64-ps3-elf*` and `*-ps3-elf*` cases to ld emulation name selection (`-melf64ppc`). |
| `libcpp/lex.c` | modify | 1 (1 line) | `data = *((const vc *)s);` → `data = __builtin_vec_vsx_ld (0, s);`. Likely a GCC 7.x VSX intrinsic workaround. **Revert first in GCC 12** — probably unnecessary now. |
| `libgcc/config.host` | modify | 1 (2 lines) | Empty `powerpc64-ps3-elf)` case so libgcc builds fall through. |

### Rebase plan for GCC 12.4.0 (PPU)

Split the monolithic patch into a patch series under `patches/ppu/gcc-12.x/`:

1. `0001-config-gcc-add-powerpc64-ps3-elf-target.patch` — the `config.gcc` stanza, re-applied onto GCC 12's rewritten `powerpc64*-*-*` block. Expected ~15 lines.
2. `0002-add-cell64lv2-target-header.patch` — the new `gcc/config/rs6000/cell64lv2.h` file, updated for GCC 12 macro landscape (new `TARGET_OS_CPP_BUILTINS`, removed obsolete macros, verify `DEFAULT_ABI`, `RS6000_ABI_NAME`). Expected ~600 lines (updated content).
3. `0003-rs6000-cell64lv2-hooks.patch` — the `rs6000.c` hunks relocated into GCC 12's `rs6000-logue.cc` / `rs6000.cc`. Consider creating a dedicated `rs6000-cell64lv2.cc` instead to avoid polluting the common file. Expected ~30 lines.
4. `0004-configure-ac-ps3-elf-emulation.patch` — the `configure.ac` hunks for ld emulation. Regenerate `configure` via `autoreconf -i`. Expected ~5 lines.
5. `0005-libgcc-config-host-ps3.patch` — the `libgcc/config.host` empty case, plus any new tmake_file entries if PS3 needs its own crtstuff. Expected ~5 lines.
6. `0006-ps3-libstdcxx-crossconfig.patch` — **new for GCC 12** — add `powerpc64-ps3-elf` arm to `libstdc++-v3/crossconfig.m4` (rewritten in GCC 10 for C++20). Disable `libbacktrace`, disable `filesystem TS` initially, declare as newlib target. Regenerate. Expected ~30 lines.
7. `0007-libcpp-lex-cellbe-vsx.patch` — **conditional** — only if the GCC 12 `libcpp/lex.c` still has the same VSX issue. Most likely unnecessary.

Revert `Makefile.in` hunk unless it's still needed — test without it first.

**Total expected patch size after rebase:** ~700–800 lines across 5–7 files.

### Rebase plan for GCC 9.5.0 (SPU)

SPU side retains more of the original 7.2.0 shape because GCC 9's `rs6000.c` is still a single file, and the target hook layout is closer to 7.2.0. The SPU build targets `spu` (not `spu-elf` — binutils 2.42 still uses bare `spu` as the default triple alias), so the same `rs6000.c` hunks do **not** apply for SPU. SPU needs its own tiny patch series:

1. `patches/spu/gcc-9.5.0/0001-spu-ps3-libgcc-tweaks.patch` — small changes to `libgcc/config/spu/` for PS3 runtime expectations, forces `-fno-asynchronous-unwind-tables` as default, provides PS3 SPU spec file entry. Expected ~50 lines.

The existing ps3dev SPU scripts (`scripts/006-gcc-newlib-SPU.sh`) show target=`spu` (not `spu-elf`) and reuse the same GCC patch as PPU — meaning the PPU patch's `rs6000/*` hunks silently don't apply to SPU builds. For our project we'll keep SPU patches entirely separate.

---

## `binutils-2.22-PS3.patch` breakdown (90 lines)

Binutils 2.22 dates from 2012. A 2012→2024 rebase across 20+ major binutils releases is not a rebase — it's a rewrite. The original patch likely contains:

- Config triple additions (`powerpc64-ps3-elf`, `spu`) → probably already upstream or handled by modern `config.sub`.
- BFD tweaks → need fresh audit against binutils 2.42's `bfd/config.bfd` and `bfd/elf64-ppc.c`.
- gas/ld assembler-macro adjustments → likely obsolete.

**Rebase plan:** Start from scratch with binutils 2.42. Read the 2.22 patch only as a clue to what the original authors found necessary. Expected new patch size: ≤50 lines, possibly zero (binutils 2.42 likely supports `powerpc64-ps3-elf` via the existing `powerpc64-*-elf` catch-all with appropriate `--with-cpu=cell`). Confirm with a dry build.

For SPU, binutils 2.42 ships `spu-elf` target intact — no patch required unless we need custom default linker script embedding (preferred alternative: ship `spu_ps3.ld` externally).

---

## `newlib-1.20.0-PS3.patch` breakdown (13,180 lines)

The big one. This patch is dominated by **new file additions** for PS3-specific `libgloss` and `libc` glue:

- `newlib/libc/sys/ps3/` — PS3 CRT0, heap setup, errno, environ
- `libgloss/libsysbase/` — PS3-specific POSIX syscall wrappers (open, close, read, write, lseek, fstat, unlink, isatty, getpid, kill, sbrk, `_exit`)
- `libgloss/ps3/` — low-level lv2 syscall stubs in assembly
- `newlib/configure.host` — add `*-ps3-*` host case
- `newlib/libc/machine/powerpc/` — minor Cell-specific tweaks if any
- `libgloss/configure.ac` / `configure` — add ps3 subdir

### Rebase plan for newlib 4.4.0.20231231

Split into a patch series under `patches/ppu/newlib-4.x/`:

1. `0001-add-ps3-host.patch` — `configure.host`, `libgloss/configure.ac` entries. ~50 lines.
2. `0002-libsysbase-ps3-stubs.patch` — port the `libgloss/libsysbase` additions forward. Mostly still applies; newlib 4.x kept libsysbase shape stable. Largest hunk. ~3000 lines.
3. `0003-libgloss-ps3-asm.patch` — low-level lv2 syscall asm stubs. ~500 lines.
4. `0004-crt0-heap-setup.patch` — `newlib/libc/sys/ps3/` directory additions. ~2000 lines.
5. `0005-reent-stubs.patch` — reentrancy wrappers for lv2 TLS. ~300 lines.
6. `0006-pthread-stubs.patch` — weak symbols for future `std::thread` gthr wiring. ~200 lines. **New** — not in the 1.20 patch.

### Key friction points

- Newlib's POSIX 2017/2019 additions (`strlcpy`, `strlcat`, `utimensat`, `pthread_*` stubs) may collide with PS3 stub names. Prefer weak symbols.
- `libgloss/libsysbase/syscall_support.c` changed signatures in newlib 3.x. Port carefully.
- `_reent` struct layout is stable, but some members added. Verify CRT0 init matches.

SPU side: `patches/spu/newlib-4.x/0001-libgloss-spu-ps3.patch`. Thin (~200 lines) — `libgloss/spu/` already has most of the mailbox/signal glue upstream; just needs PS3-specific event wait and mbox wrappers.

---

## `gdb-8.3.1-PS3.patch` breakdown (403 lines)

PPU-only debugging. Targets:

- GDB target definition for `powerpc64-ps3-elf`
- Minor PPU syscall knowledge for lv2
- No SPU combined-debug (orphaned post-2019)

### Rebase plan for GDB 14.2

Start fresh. GDB 14.2 already has excellent PowerPC64 support; the PS3 delta is small:

1. `patches/ppu/gdb-14.x/0001-ps3-ppu-target.patch` — register `powerpc64-ps3-elf` as a recognized target, hook lv2 syscall table if debug info wants it. ~50 lines.

Expect minimal patching. Main integration work is documenting how to connect GDB 14.2 to RPCS3's gdbstub (RPCS3 implements a standard PPC GDB remote protocol).

---

## Summary of expected patch size after rebase

| Component | Current (ps3dev) | After rebase |
|---|---:|---:|
| binutils | 90 | ~0–50 |
| GCC (PPU 12.4.0) | 727 | ~700–800 |
| GCC (SPU 9.5.0) | 727 (shared) | ~50 |
| newlib (PPU 4.4.0) | 13,180 | ~6,000 |
| newlib (SPU 4.4.0) | (shared) | ~200 |
| GDB (PPU 14.2) | 403 | ~50 |

Total new-world patch surface: roughly **~7,000 lines** across all targets, versus **~14,400** on the ps3dev baseline. Most of the reduction comes from newlib additions that are no longer needed (newer upstream newlib already has them, especially POSIX surface).

---

## Rebase ordering (execution sequence)

1. **Binutils 2.42** for both PPU and SPU — fastest to prove the triple wiring works.
2. **Newlib 4.4.0** PS3 host cases added, stubs ported — this blocks the GCC stage-2 build.
3. **GCC 12.4.0** for PPU — rebase `config.gcc`, `cell64lv2.h`, configure.ac, then attempt stage-1 build.
4. **GCC 9.5.0** for SPU — minor libgcc tweaks; backend already intact.
5. **GDB 14.2** for PPU — small, can be done last.

Each component gets its own patch series under `patches/{ppu,spu}/<component>/` with a `series` file controlling apply order.
