# Roadmap

This project is already usable as a Linux-hosted PS3 toolchain and SDK. The remaining work is about breadth, validation, and release packaging.

## SDK surface

The SDK is moving subsystem by subsystem from compatibility shims into native headers and archives under `sdk/`. The NID/FNID database drives stub archive generation, and `docs/coverage.md` tracks export coverage against the known reference SDK library surface.

Near-term SDK work focuses on:

- finishing high-value cell headers that still block external sample imports
- continuing the network and NP sample coverage sweep
- tightening ABI checks for caller-allocated structs and cross-SPRX pointer fields
- replacing fallback PSL1GHT paths with native archives where the ABI is understood

## Toolchain

PPU uses GCC 12.4.0 today. Future PPU bumps are routine patch-set rebases across newer GCC target-hook and middle-end API changes.

SPU uses GCC 9.5.0 today because it is the last upstream GCC release with the Cell SPE backend. The forward-port track resurrects that backend for a newer GCC line, modernizes it against current backend APIs, and eventually gives PPU and SPU a unified compiler version.

## Runtime and samples

The native LV2 runtime, compact OPD path, CMake SELF helper flow, and install manifest are the default path for new samples. The sample tree continues to grow as each SDK family lands. Samples are intended to demonstrate practical build and link shapes, not to serve as a release-quality test matrix by themselves.

## Releases

Linux remains the primary development and source-build host. Windows-hosted release artifacts are produced by cross-building from Linux with Mingw-w64 and packaging the `.exe` toolchain, host tools, CMake helpers, portlibs, headers, archives, and samples into a self-contained zip.

Release mechanics and version-string rules live in `docs/VERSIONING.md`.

## Relationship to upstream PSL1GHT (deliberate divergences)

Upstream's "v3" RFC (ps3dev/PSL1GHT issue #67, 2017, still open) has four items. Three are already this project's design: the NID/FNID database with generated stub archives (`tools/nidgen`), `cell*`/`Cell*`/`CELL_*` naming for SDK-owned APIs, and `sys_*` snake_case syscalls. The remaining item is a deliberate divergence, recorded here so it is not revisited by accident:

- **Duplicated C headers.** The RFC proposes deleting PSL1GHT's copies of standard headers and pushing the implementations into newlib. This project does the opposite: `sdk/include/` overrides the vendored headers and the newlib patch set carries what the PS3 needs, because SDK-API parity (the official `cell/*` surface) will never live in upstream newlib and must be owned here.
- **Legacy names.** The RFC's position is that renamed symbols need no aliases (old code stays on the old PSL1GHT). This project keeps the legacy PSL1GHT names compiling through deprecated aliases behind a negative gate (`__PS3DK_NO_PSL1GHT_COMPAT__`), so existing homebrew builds and warns instead of breaking.
- **Vendored PSL1GHT** is pinned at `eca3f99`; upstream's in-flight NID rename (PR #169) is tracked but not chased while it is a draft.

The one v3 must-have this project does not yet have is SPRX/PRX module generation; it is a design-first item on the board, not started.
