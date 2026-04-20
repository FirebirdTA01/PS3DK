# rsx_legacy — absorbed from PSL1GHT's librsx

These sources originated in PSL1GHT's `ppu/librsx/` tree and implement
the `rsx*` command-emitter surface (`rsxSetFoo`, `rsxDrawVertexArray`,
`rsxLoadFragmentProgramLocation`, ...).  They're reproduced here
verbatim under PSL1GHT's original MIT-style license (see LICENSE in
this directory) so that libgcm_cmd.a can ship a complete
NV40-command-emitter surface without depending on PSL1GHT's `librsx.a`.

This directory is intentionally a **source-level snapshot** — the
same files compiled into libgcm_cmd.a that PSL1GHT used to compile
into librsx.a.  Samples link `-lgcm_cmd` alone (no `-lrsx`) and get
the full 298-symbol surface.

The long-term direction is to replace individual emitters with native
`cellGcm*` equivalents living under `sdk/libgcm_cmd/src/` (not in this
subdirectory), phasing out the `rsx*` names at callsites in
`sdk/include/cell/*` as each native emitter lands.  Until that
migration finishes, the absorbed sources provide full coverage.
Reference cell SDK layout is the target: one `libgcm_cmd.a`, no
separate rsx library.

## License

MIT (see LICENSE).  Copyright (c) 2011 PSL1GHT Development Team.
