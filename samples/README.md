# Samples

Minimal programs that exercise the toolchain, PSL1GHT, and Sony-ABI conformance.
Each sample has one concept; big examples go elsewhere.

## Current samples

| Sample | Validates | Depends on | Status |
|---|---|---|---|
| `hello-ppu-c++17/` | PPU C++17 front- and back-end, libstdc++ link, PPU thread id | Phase 1 complete | **green** |
| `hello-ppu-sony-abi/` | `.sys_proc_param` section layout (36-byte Sony 3.30+), malloc/calloc/realloc through libsysbase `_sbrk_r`, printf through `_write_r` | Phase 1 + Phase 3 milestone 1 | **green** |
| `hello-spu/` | SPU toolchain + intrinsics + DMA back to PPU, combined PPU/SPU build | Phase 2a complete | **green** |
| `ppu-spu-dma/` (TBD) | Full DMA mailbox exchange patterns | Phase 2a complete | not yet |
| `rsx-triangle-vs/` (TBD) | Existing PSL1GHT vertex shader pipeline still works | Phase 3 Beta | not yet |
| `rsx-textured-quad-fs/` (TBD) | New NV40-FP fragment shader assembler | Phase 3 M5 | not yet |
| `net-http-get/` (TBD) | Networking + mbedTLS HTTPS | Phase 4 + Phase 3 networking | not yet |

## What `hello-ppu-sony-abi` checks

It is the ABI oracle: if it regresses, our output drifted from Sony's expectations.

- **`.sys_proc_param` section size** is 0x24 (36 bytes), 8-byte aligned.
- **Magic** `0x13bcc5f6`.
- **Version** `0x00330000` (firmware 3.30+ layout).  PSL1GHT's historical
  version `0x00009000` is rejected by the retail PS3 loader on firmware
  3.30+; RPCS3 is permissive enough to let either through, so passing on
  RPCS3 is necessary but not sufficient.
- **Struct** has 9 fields including the trailing `crash_dump_param_addr`.
- **Allocator** — writes and reads 8 × 32 KiB blocks via `malloc` → free,
  zero-verified `calloc`, `realloc` grows.  End-to-end path:
  `malloc → _malloc_r → sbrk → _sbrk_r (libsysbase) → __syscalls.sbrk_r`
  (installed by PSL1GHT crt1).
- **stdio** — every `printf` routes through `_write_r` → `__syscalls.write_r`.

Run under RPCS3 with `--no-gui` to watch the stdout; a passing run ends
with `result: PASS`.

`.sys_proc_param` hex dump under `powerpc64-ps3-elf-objdump -s -j
.sys_proc_param hello-ppu-sony-abi.elf` should look like:

```
 xxxxx 00000024 13bcc5f6 00330000 ffffffff  ...$.....3......
 xxxxx 000003e9 00010000 00100000 00000000  ................
 xxxxx 00000000                             ....
```

Fields in order: size=36, magic, version=VERSION_330, sdk_version=UNKNOWN,
prio=1001, stacksize=0x10000, malloc_pagesize=1M, ppc_seg=DEFAULT,
crash_dump_param_addr=0 ("no callback" — see Phase 3 task #7).

## Building

```bash
source ../scripts/env.sh   # exports PS3DEV, PSL1GHT
cd hello-ppu-c++17
make
```

Artifacts:
- `<sample>.elf` — the PPU ELF
- `<sample>.self` — signed for RPCS3 / fake-signed for jailbroken hardware
- `build/` — intermediates

## Running

**RPCS3 (preferred for quick iteration):**
```bash
rpcs3 --no-gui <sample>.self
```

**Jailbroken hardware via ps3load:**
```bash
make run   # requires ps3load on PATH and PS3LOADIP pointing at your console
```

## Retest matrix after major rebuilds

After any rebuild of binutils / GCC+newlib / PSL1GHT, all three green
samples must still link and (where possible) run:

1. `hello-ppu-c++17` — our own toolchain smoke test (C++17 + PSL1GHT runtime).
2. `hello-ppu-sony-abi` — Sony-ABI oracle.  A regression here means we
   drifted from Sony's loader expectations even if RPCS3 still loads the
   binary.
3. PSL1GHT's own `sample/` tree (once built) — verifies downstream
   homebrew still compiles against the modernised headers.

A rebuild isn't "done" until all three still pass.

## Contributing a new sample

1. Create `samples/<name>/` with `Makefile`, `source/main.c` (or `.cpp`).
2. Use `include $(PSL1GHT)/ppu_rules` for PPU-only; add `spu/` subdir with
   `include $(PSL1GHT)/spu_rules` for SPU code.
3. Add a row in the table above with what it validates and which phase
   must be complete before it can build.
4. Keep samples minimal — one concept per sample.  Big examples go
   elsewhere.
5. If the sample exercises a Sony-ABI boundary, document the hex-dump
   shape it expects so future regressions are visible in `objdump -s`.
