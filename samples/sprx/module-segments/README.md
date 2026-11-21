# sample-sprx-module-segments

A relocatable module laid out the way retail Lv-2 needs it (issue #137), and
a loader that checks every step on its way through load, start, calls, stop
and unload.

## What the module does

`sprx_segments` exports two libraries, `seg_calc` and `seg_state`, beside
the crt's management record: three export records, each 28 bytes. A 44-byte
export record hangs `sys_prx_start_module` on a console as soon as there is
more than one record.

`module_start` reads its `.data` global, writes it and reads it back, does
the same with a `.bss` global and a 64-word `.bss` block, and sums a
`.rodata` table. It reports every value through `argp`. `module_stop` writes
`.data` again and reports what it saw. With one writable and executable
segment, the first of those stores froze the process on a console; the
module is now linked as a code segment (`0x00400005`) and a data segment
(`0x00600006`), and `prx-gen check` refuses the old layout.

## What the loader prints

One line per step, flushed as it goes, so the last line on the TTY shows
where a console stopped:

```
SPRX_SEG begin step=load
SPRX_SEG try path=/app_home/sprx_segments.sprx rc=0x...
SPRX_SEG PASS step=load id=0x... path=...
SPRX_SEG begin step=start
SPRX_SEG PASS step=start rc=0x00000000 modres=0
SPRX_SEG PASS step=data-write got=0x5eed0001 want=0x5eed0001
...
SPRX_SEG PASS step=unload rc=0x00000000
SPRX_SEG_ALL_PASS steps=20 abi=ilp32
```

A failed check prints `SPRX_SEG FAIL step=<name> ...` and the run ends with
`SPRX_SEG_RESULT FAIL failed=<n>`.

The loader looks for its module next to itself (the directory of `argv[0]`)
and in `/app_home`, first as `.sprx` and then as `.fake.sprx`. A `.self`
loads the real-signed `.sprx`; a `.fake.self` is refused the `.sprx`
(`0x80010009` on a console) and loads the `.fake.sprx`.

## Build outputs

Both ABIs are built. Everything lands in the build directory:

| ABI | Module | Loader |
|---|---|---|
| ILP32 | `sprx_segments.{prx,sprx,fake.sprx}` | `sprx-segments-loader.{self,fake.self}` |
| LP64 | `sprx_segments_lp64.{prx,sprx,fake.sprx}` | `sprx-segments-loader-lp64.{self,fake.self}` |

Copy a loader and its module into the same directory. The regression row
`tests/regression/sprx-segments` stages the `.fake.self` / `.fake.sprx`
pairs for RPCS3 (rows `sprx-segments` and `sprx-segments-lp64`).

RPCS3 accepts the old layout too, so a pass there shows only that the new
layout still loads and runs; the console is the real test.
