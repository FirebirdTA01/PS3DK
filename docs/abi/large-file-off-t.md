# Large files (> 2 GiB) on the PPU: `off_t` width and the `*64` family

Scope: in the default PPU ABI (ILP32), `off_t` is 32 bits and there is no
64-bit `struct stat`, so the non-`64` POSIX file interface cannot describe a
file larger than 2 GiB.  This note records why that stays, what to use
instead, and the defect to fix.  The LP64 multilib (`-mlp64`) is not affected:
there `off_t` and `st_size` are 64 bits.

## Measurements

A compile-time probe against the installed headers (`_Static_assert`, default
ABI and `-mlp64`):

| | ILP32 (default) | LP64 (`-mlp64`) |
|---|---|---|
| `sizeof(off_t)` | 4, signed | 8 |
| `sizeof(struct stat::st_size)` | 4 | 8 |
| `sizeof(_off64_t)` | 8 | 8 |

1. **`off_t` is `long` by a typedef we ship, not a configure flag.**  newlib's
   `machine/_types.h`, added by
   `patches/ppu/newlib-4.x/0004-newlib-libc-sys-lv2-reent-locks.patch`, has
   `typedef long _off_t;`, so the width follows `long`: 4 bytes in ILP32, 8
   in LP64.  `samples/toolchain/hello-ppu-mlp64-types/` checks the same
   widths.  Changing it means changing that typedef and rebuilding libc,
   libstdc++ and every portlib.

2. **The `*64` family is mixed.**  In
   `patches/ppu/newlib-4.x/0003-libsysbase-ps3-syscall-wrappers.patch`:

   | symbol | offset/size type | 64-bit? |
   |---|---|---|
   | `lseek64` | `_off64_t` offset | **yes** |
   | `fstat64` | `struct stat *` | no, same struct as `fstat` |
   | `stat64` | `struct stat *` | no, same struct as `stat` |

   In `runtime/lv2/librt/fstat.c`, `__librt_fstat64_r` calls
   `__librt_fstat_r` and `__librt_stat64_r` calls `__librt_stat_r`, so
   `fstat64` returns the same 32-bit `st_size` as `fstat`.  `fseeko64` and
   `ftello64` are not provided.  The only 64-bit POSIX primitive is `lseek64`.

3. **The `cellFs*` family is 64-bit.**
   `sdk/include/cell/fs/cell_fs_file_api.h` types `CellFsStat::st_size` as
   `uint64_t`; LV2 reports sizes in 64 bits.

## The defect

`runtime/lv2/librt/fstat.c` (`convert_lv2stat`) does
`dst->st_size = src->st_size`: a 64-bit LV2 size into the 32-bit signed
`off_t`.  POSIX requires the non-`64` functions to fail with `EOVERFLOW` when
a size does not fit `off_t`; we truncate instead.  Because `off_t` is signed, a
file of 2 to 4 GiB reports a negative `st_size`, and a larger one its low 32
bits.

## Decision

**Keep `off_t` 32-bit in ILP32, document the limit, stop the silent
truncation.**

- `fstat` and `stat` should fail with `-1` and `errno = EOVERFLOW` when the
  LV2 size does not fit `off_t` (follow-up below).
- For sizes above 2 GiB use the `cellFs*` interface (64-bit `st_size`); for
  64-bit offsets use `lseek64`.  The non-`64` POSIX functions stay 32-bit by
  design.

## Why `off_t` does not become 64-bit

A wider `off_t` changes the typedef in newlib's `machine/_types.h`, which is a
libc ABI break: the `struct stat` layout and the `lseek` / `ftruncate`
signatures change width, and every static archive built against a 4-byte
`off_t` (libc, libstdc++, every portlib, the `runtime/lv2` glue) would
mismatch a library built with 8 bytes.  The 64-bit paths already exist:
`cellFs*` for sizes and `lseek64` for offsets.

## Follow-up (implementation)

**Chosen:** in `runtime/lv2/librt/fstat.c`, before the `st_size`
assignment, fail with `errno = EOVERFLOW` and `-1` when
`src->st_size > INT32_MAX` (the largest ILP32 `off_t`), in both
`__librt_fstat_r` and `__librt_stat_r`; state the bound in a comment in
`sdk/include/sys/stat.h`.  No typedef change, no new type, no ABI change.

**Rejected:** a separate `struct stat64` with a 64-bit `st_size` for
`stat64` / `fstat64`.  It changes newlib's machine types, needs a libc
rebuild and changes the ABI of every archive using those symbols, while
`cellFs*` and `lseek64` already cover files above 2 GiB.
