# librsx - SDK-owned legacy RSX compatibility archive

These sources originated in PSL1GHT's `ppu/librsx/` tree and implement
the `rsx*` command-emitter surface (`rsxSetFoo`, `rsxDrawVertexArray`,
`rsxLoadFragmentProgramLocation`, ...).  They're reproduced here
verbatim under PSL1GHT's original MIT-style license (see LICENSE in
this directory) so the SDK can ship ILP32 and LP64 `librsx.a`
compatibility archives without depending on PSL1GHT's prebuilt archive.

This directory is intentionally a **source-level snapshot**: the same
files that PSL1GHT used to compile into `librsx.a`, with the small PS3
Custom Toolchain fixes already carried in `sdk/libgcm_cmd/src/rsx_legacy/`.
Samples that still link `-lrsx` get the same 299-symbol surface from
SDK-owned native archives.

The long-term direction is to replace individual emitters with native
`cellGcm*` equivalents living under `sdk/libgcm_cmd/src/` (not in this
subdirectory), phasing out the `rsx*` names at callsites in
`sdk/include/cell/*` as each native emitter lands.  Until that
migration finishes, this archive preserves compatibility for legacy
`-lrsx` link lines and prevents LP64 builds from falling back to
PSL1GHT's ILP32-only `librsx.a`.

## License

MIT (see LICENSE).  Copyright (c) 2011 PSL1GHT Development Team.
