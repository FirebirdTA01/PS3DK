# Header regression guards

Run `python3 tests/sdk/header-regression-test.py --require-toolchain` with
`PPU_CC` and `PPU_CXX` set to PPU compiler paths, or with a toolchain under
`PS3DEV/ppu/bin`, `PS3DK/ppu/bin`, or on `PATH`.
`--output <new-directory>` retains commands, diagnostics and objects.

The five fixtures preserve checks from the header-visibility branch:

| Guard | Checks |
|---|---|
| in_addr | Five include combinations of netinet/in.h, cell/libnetctl.h and netex/libnetctl.h; a four-byte struct |
| pthread | Both sys/types.h/pthread.h orders and pthread.h alone; strict C99/C11, GNU C11 and C++17 types/initializers |
| process parameters | SYS_PROCESS_PARAM exposed through cell/spurs.h and sys/dbg.h separately |
| virtual memory | State/policy constants, statistics offsets and all 13 sys_vm function signatures |
| SPU thread events | Event/DMA constants and four canonical plus four legacy function signatures |

The runner compiles this checkout's `sdk/include` before the selected compiler's
installed support headers. It makes 78 object compiles across ILP32 and LP64,
using GNU C99, C11 and C++17 except for the pthread modes listed above. All use
`-Wall -Wextra -Werror`. Ten additional negative compiles introduce a duplicate
in_addr, unavailable pthread type, missing process macro, wrong VM state value
or wrong SPU event value, once per ABI. Each must fail with the relevant
diagnostic after the corresponding unmodified header has passed.

These are declaration/compile guards, not link, syscall, thread or hardware
tests. The synthetic defects demonstrate sensitivity, not historical failures.
Without both target compilers, the default invocation explicitly reports SKIP;
`--require-toolchain` makes that a failure. A configured invalid compiler is
always an error. Native host compilers are not substitutes for these ABI checks.
