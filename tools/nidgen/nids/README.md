# NID Database

YAML files in this directory define the imports/exports of PS3 system
libraries. One file per library. The nidgen CLI consumes them to:

- generate PPU assembly stub files (`.section .lib.stub.<library>`),
- build stub archives (`lib<library>_stub.a`) for link-time resolution,
- verify computed FNIDs against stored NID values,
- drive the coverage-report tool.

## File naming

`<library>.yaml`, matching the Sony SDK's library prefix:

- `sys_lv2.yaml` — lv2 kernel syscalls (`sys_ppu_thread_*`, `sys_spu_*`, etc.)
- `cellGcmSys.yaml` — RSX GCM runtime
- `cellNetCtl.yaml` — network control
- `cellPad.yaml` — controller input
- `cellAudio.yaml` — audio output
- `cellSysutil.yaml` — system utility (callbacks, save data)
- `cellFs.yaml` — filesystem
- `cellSpurs.yaml` — SPU thread scheduler

## Schema

See `schema.yaml` for the full JSON Schema. Minimum structure:

```yaml
library: cellNetCtl            # PSL1GHT lib prefix
version: 1                     # schema version
module: sys_net                # Sony SPRX module the library lives in
exports:
  - name: cellNetCtlInit
    nid:  0xbd5a59fc           # authoritative NID from Sony stub .a
    signature: "int cellNetCtlInit(void)"
  - name: cellNetCtlGetInfo
    nid:  0x8ce13854
    signature: "int cellNetCtlGetInfo(int code, CellNetCtlInfo *info)"
```

## Where do NIDs come from?

Sony's official PS3 SDK ships stub `.a` archives under `target/ppu/lib/`. Each
archive's member objects declare symbols (the function names) and embed the
NID as a constant in the `.lib.stub.*` section. We extract the (name, NID)
pair by:

1. `ar t libcellnetctl_stub.a` → list member objects.
2. `powerpc64-ps3-elf-nm --defined-only libcellnetctl_stub.a` → enumerate
   symbols.
3. `powerpc64-ps3-elf-readelf -x .lib.stub.cellNetCtl <member>.o` → dump the
   stub section and parse out the NID field.

The `nidgen verify` command cross-checks that, for each entry, the computed
FNID (SHA-1(name || suffix)[0..4], little-endian) matches the stored NID.
Sony's build pipeline uses the same algorithm to originally assign NIDs, so
these should always match for symbols Sony named in the SDK.

## Legal / clean-room note

The YAML files here contain only **names, NIDs, and signatures** — no Sony
source code or header contents. Names are recovered from public ABI (ELF
symbol tables and exported ordinals); NIDs are computed hashes of those names.
Signatures are reimplemented from the Sony header text by clean-room
paraphrase, not copy.

The Sony SDK mount at `reference/sony-sdk/` is never committed.

## Populating a new library

1. Identify the Sony stub archive (`target/ppu/lib/lib<name>_stub.a`).
2. Enumerate its defined symbols and their `.lib.stub.*` NIDs (see above).
3. For each symbol, reimplement the C signature from the Sony header (do not
   copy verbatim).
4. Write the YAML and run `nidgen verify nids/<name>.yaml` — every line should
   report "ok".
5. Run `nidgen stub nids/<name>.yaml > <name>.s` to emit the assembly and
   `nidgen archive ...` to build the `.a` archive.
