# Samples

Minimal programs that exercise the toolchain and PSL1GHT. Each sample has
instructions for what it validates and when it should compile/run.

| Sample | Validates | Depends on |
|---|---|---|
| `hello-ppu-c++17/` | PPU C++17 front- and back-end, libstdc++ link, PPU thread id | Phase 1 complete |
| `hello-spu/` | SPU toolchain + intrinsics + DMA back to PPU, combined PPU/SPU build | Phase 2a complete |
| `ppu-spu-dma/` (TBD) | Full DMA mailbox exchange patterns | Phase 2a complete |
| `rsx-triangle-vs/` (TBD) | Existing PSL1GHT vertex shader pipeline still works | Phase 3 Beta |
| `rsx-textured-quad-fs/` (TBD) | New NV40-FP fragment shader assembler | Phase 3 M5 |
| `net-http-get/` (TBD) | Networking + mbedTLS HTTPS | Phase 4 + Phase 3 networking |

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

## Contributing a new sample

1. Create `samples/<name>/` with `Makefile`, `source/main.c` (or `.cpp`).
2. Use `include $(PSL1GHT)/ppu_rules` for PPU-only; add `spu/` subdir with
   `include $(PSL1GHT)/spu_rules` for SPU code.
3. Add a row in the table above.
4. Keep samples minimal — one concept per sample. Big examples go elsewhere.
