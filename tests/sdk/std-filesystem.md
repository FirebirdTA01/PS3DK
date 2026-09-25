# PPU C++17 filesystem regression

`std-filesystem-test.cpp` is a target runtime probe for the filesystem link
failure reported by the EMP team. It needs GCC patch 0036 applied to a fresh
source/build tree, rebuilt libstdc++ for **both** PPU multilibs, and librt rebuilt
with `pathconf.c`. It also needs newlib patch 0016 so `remove()` can remove
directories; without it the directory-removal assertions fail with EISDIR.
Editing installed headers alone does not repair the archive.
The existing patch-application stamp can skip newly added patches in an old
extracted tree; use a fresh build root for release validation.

For each ABI, inspect the installed `bits/c++config.h`: `_GLIBCXX_HAVE_OPENAT`,
`_GLIBCXX_HAVE_FDOPENDIR`, `_GLIBCXX_HAVE_UNLINKAT`, `_GLIBCXX_HAVE_SYMLINK`,
`_GLIBCXX_HAVE_READLINK`, `_GLIBCXX_USE_FCHMODAT`, and `_GLIBCXX_USE_FCHMOD` must
be undefined. All must be off even when configure cannot link executables.

With `PS3DK` naming the rebuilt SDK, a minimal link command is:

```sh
"$PS3DK/ppu/bin/powerpc64-ps3-elf-g++" -std=c++17 -mcpu=cell -mhard-float \
    -I"$PS3DK/ppu/include" tests/sdk/std-filesystem-test.cpp \
    -L"$PS3DK/ppu/lib" -lrt -o filesystem.elf
```

For LP64 add `-mlp64` and use `-L"$PS3DK/ppu/lib/lp64"`. Convert the ELF with
`sprxlinker --lp64` for LP64 (`sprxlinker` without that flag for ILP32), then
`make_self` and run it on RPCS3 or hardware. The LP64 flag inserts caller TOC
restoration required by LP64 import stubs; omitting it can fault before main.
The probe creates a fresh, process-specific directory under `/dev_hdd0/tmp`
and removes only its own named files and directories. It refuses to reuse an
existing directory. Success prints `FILESYSTEM_RESULT failures=0`.

The runtime assertions require an ordinary directory iterator to yield a
known file; they also exercise status, current_path, copy_file, permissions,
file/directory creation and removal, and pathconf limits and lookup errors.
Symlink creation and reading must report `std::errc::function_not_supported`
through `error_code`, matching GCC 12's fallback, without creating a link.

`librt-pathconf-test.sh` separately runs the pathconf implementation on the
host, renamed so host libc cannot accidentally satisfy its calls. This tests
selector and lookup behavior; target values and filesystem behavior require
the target probe. A successful link alone is not runtime validation.
