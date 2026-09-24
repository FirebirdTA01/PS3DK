# Configuring the PPU newlib heap

The PPU runtime reserves one contiguous arena for newlib `malloc` and C++
`new` before application constructors run. The default remains **64 MiB**,
allocated with **1 MiB pages**. This setting does not configure SPU memory.

Put one definition at global scope in a C or C++ application source file:

```c
#include <sys/process.h>
PS3TC_HEAP_SIZE(UINT64_C(128) * 1024 * 1024);
```

The macro is also available from the lightweight `<sys/heap_config.h>`.
Its argument is a constant size **in bytes**, represented by `uint64_t` in
both PPU data models. Sizes round **up** to a multiple of 1,048,576 bytes:
1,048,577 requests a 2,097,152-byte arena. Zero, rounding overflow, sizes
outside the address or allocation syscall limits, and sizes greater than
the currently available user memory fail at startup. Allocation can still
fail after that check, for example if a contiguous region is unavailable.
A larger arena leaves less memory for other Lv-2 allocations.

There must be exactly one application definition. It supplies a strong
`__ps3tc_heap_size` symbol over the runtime's weak default. Ordinary object
files need no additional linker flag. If the definition is inside a static
library, force that member to be extracted at the final application link:

```text
-Wl,-u,__ps3tc_heap_config_anchor
```

Without the anchor, a linker need not extract an archive member merely to
replace an existing weak definition. The same interface works with LTO;
do not provide another definition through a separate build-system policy.

Startup failure writes one line to TTY channel 2 and terminates the process
with status 1. It reports requested and rounded byte counts, available bytes
(zero when not queried), an error code in hexadecimal, and a reason.
Validation errors use `EINVAL` or `ENOMEM`; syscall errors retain their
Lv-2 value. Application constructors do not run after this failure.

When the initialized arena first refuses a positive `sbrk` increment, it
returns `ENOMEM` and writes a line such as:

```text
PS3TC heap: exhausted increment=4096 break=67106816 capacity=67108864
```

`break` is the byte offset from the arena base; `capacity` is the rounded
arena size. This line is emitted at most once per process, even if malloc
retries or later frees memory. It diagnoses arena exhaustion, which differs
from failure to allocate the arena at startup. Both diagnostic paths use a
static buffer and the direct Lv-2 TTY syscall, with no `malloc`, stdio, or
POSIX `write` calls. TTY delivery itself is best effort.

`tests/sdk/librt-heap-test.sh` checks sizing, failure paths, extreme increments,
one-time diagnostics, C/C++ definitions, LTO and archive extraction on the
host. PPU runtime validation uses an isolated runtime overlay.
