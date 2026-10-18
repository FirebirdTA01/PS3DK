# Toolchain Design Notes

This document explains the current compiler-version pins and the rationale behind them. The short version is in `README.md`; this file keeps the compiler background out of the quickstart.

## PPU compiler

The PPU target uses GCC 16.1.0 as `powerpc64-ps3-elf` (`patches/ppu/gcc-16.x`).

Earlier releases shipped GCC 12.4.0, the highest line that rebased onto the original PPU patch set without a broader backend rewrite. The move to GCC 16 is described in [design/gcc-transition.md](design/gcc-transition.md). The GCC 16 driver builds C++ as GNU C++17 unless a `-std=` option is given, so C++20 and C++23 are opt-in; C builds as GNU C23, where an implicit function declaration is an error. libstdc++ and libgcc are built with the POSIX thread model over the runtime's pthreads.

The PPU side also carries the CellOS-specific pieces needed by this SDK:

- target recognition for `powerpc64-ps3-elf`
- compact OPD and TOC behavior expected by the LV2 runtime
- newlib/libgloss integration for the PS3 syscall layer
- multilib support for the default ELF64+ILP32 ABI and the opt-in LP64 ABI

## SPU compiler

The SPU target currently uses GCC 9.5.0 as `spu-elf`.

GCC 9.5.0 is the last upstream GCC release that still ships the Cell SPE backend. GCC 10 removed `gcc/config/spu/`, `libgcc/config/spu/`, the SPU intrinsic headers, and the SPU test suite. Staying on 9.5.0 keeps the backend intact while still providing full C++17 for SPU code.

SPU C++20 support is early and partial: some constexpr extensions, aggregate-init fixes, comparison groundwork, and experimental `-std=c++2a` are present, but the major C++20 library and language surface landed after the backend was removed upstream. In practice, SPU usability is constrained more by the 256 KiB local-store budget than by the C++ dialect. The common SPU profile stays pragmatic: `-Os`, no RTTI, no exceptions, and careful library selection.

## Runtime and library pins

The current runtime pins are:

- binutils 2.42 for PPU and SPU
- newlib 4.4.0 for PPU and SPU
- GDB 14.2 for PPU debugging
- current portlibs for the PPU userland libraries

Newlib is bumped when upstream changes justify the rebase or the PS3 libsysbase glue needs material work. GDB is bumped when RPCS3 gdbstub behavior or upstream debugger improvements make it useful.

## Forward-port work

The long-running compiler work is to forward-port the SPE backend to a newer GCC line, then keep PPU and SPU on the same major version. That work is tracked separately from the day-to-day SDK stub and header work because it touches GCC backend internals, libgcc build rules, SPU intrinsic validation, and the test suite.
