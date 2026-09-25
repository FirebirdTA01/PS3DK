# SPURS task entry points

SPU tasks include `<cell/spurs/task.h>` and may define the canonical entry:

```c
int cellSpursTaskMain(qword argTask, uint64_t argTaskset);
```

The header gives this function C linkage in both C and C++. The task runtime
passes the original 16-byte task argument and 64-bit taskset argument to it,
then passes its signed return value to the nonreturning `cellSpursTaskExit`.

The existing CRT still calls `void cellSpursMain(qword, uint64_t)`. A small
bridge providing that symbol lives in its own `libspurs_task.a` member. Put
application objects before the runtime archive when linking:

```sh
spu-elf-gcc -mspurs-task -nostartfiles \
  -T "$PS3DK/spu/ldscripts/spurs_task.ld" task.o \
  -L "$PS3DK/spu/lib" -lspurs_task -o task.elf
```

A legacy application definition of `cellSpursMain` satisfies the CRT without
extracting the bridge. This definition takes precedence when an application
defines both entries. Returning from the legacy entry retains the CRT's
existing `cellSpursExit` behavior. Providing neither entry fails linking;
the bridge does not supply a dummy task body.

Use normal archive extraction. Forcing the entire task runtime with
`--whole-archive` also forces the bridge and can collide with a legacy entry.
If application entries live in another archive, order or group it so its
legacy definition is selected before the default bridge.
In particular, a legacy `cellSpursMain` in an application archive listed
after `libspurs_task.a` does not override a bridge already selected from
the task runtime; that order selects the bridge instead.

The task linker script retains the entry at local-store address `0x3000`.
Startup selection still needs the explicit script and `-nostartfiles` above;
the entry bridge does not implement automatic driver mode selection. It also
does not add constructor dispatch. Retaining constructor sections in an ELF
does not by itself execute them.

The entry controls check C/C++ declaration and link ownership, ELF entry and
task flags, missing-entry failure, and host execution of argument/exit-code
forwarding. They do not establish SPURS scheduling or constructor behavior
on hardware or in an emulator.
