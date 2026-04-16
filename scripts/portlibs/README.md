# Portlibs Recipes

One file per library. Ordered prefix (`NNN-libname.sh`) controls build order.

## Recipe contract

Each recipe is a bash script sourced by `scripts/build-portlibs.sh`. The outer
script pre-exports:

- `CC`, `CXX`, `AR`, `RANLIB`, `STRIP` — cross tool names
- `HOST_TRIPLE` — `powerpc64-ps3-elf`
- `CFLAGS`, `CXXFLAGS` — `-O2 -mcpu=cell -fPIC`
- `PORTLIBS` — install prefix (`$PS3DEV/portlibs/ppu`)
- `PKG_CONFIG_PATH` — includes `$PORTLIBS/lib/pkgconfig`

The recipe must:

1. Fetch the source tarball (cached in CWD).
2. Verify SHA-256.
3. Extract if not already extracted.
4. `cd` into the source dir.
5. Configure/build with cross tools.
6. `make install` into `$PORTLIBS`.

Recipes run with `set -euo pipefail`. Current dir is `$PS3_BUILD_ROOT/portlibs`.

## Planned recipes (phase 4 scope)

| Order | Name | Version | Notes |
|---:|---|---|---|
| 001 | zlib | 1.3.1 | Compression primitive; everything else depends on it |
| 002 | libpng | 1.6.43 | After zlib |
| 003 | libjpeg-turbo | 3.0.3 | SIMD on PPU via VMX if we decide to enable |
| 004 | freetype | 2.13.2 | Needs zlib, libpng |
| 005 | libogg | 1.3.5 | |
| 006 | libvorbis | 1.3.7 | Needs libogg |
| 007 | libtheora | 1.1.1 | Needs libogg, libvorbis |
| 008 | FLAC | 1.4.3 | Needs libogg |
| 009 | mpg123 | 1.32.x | |
| 010 | libxmp | 4.6.0 | |
| 011 | libmikmod | 3.3.13 | |
| 012 | libzip | 1.10.x | Needs zlib |
| 020 | mbedTLS | 3.6.0 | Unlocks HTTPS |
| 021 | libssh2 | 1.11.0 | Needs mbedTLS |
| 022 | libcurl | 8.9.x | Needs mbedTLS |
| 030 | SDL2 | 2.30.x | Custom PS3 audio + video backends |
| 099 | finalize | — | Regenerate pkg-config manifest, strip |

## Non-standard: SDL2

SDL2 requires custom PS3 video and audio drivers. The recipe applies patches
from `patches/portlibs/SDL2/` that implement:

- `src/video/ps3rsx/` — RSX GCM video backend
- `src/audio/ps3cell/` — `cellAudio` output driver

This is more than a simple port — expect ~1000 LOC of new code under those
subdirs. Validated via `samples/net-http-get/` (uses SDL2 window) and the
eventual `samples/sdl2-audio-demo/`.
