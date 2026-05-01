# Next session — finish SPU spurs-job MEMORY_SIZE investigation (v2)

Pickup prompt for the next iteration on `cellSpursJobMain2` end-to-end
bring-up.  Sessions 1-7 (logged in `docs/spurs-job-binary2-re.md`)
cleared the toolchain side and the JOB_DESCRIPTOR (BIN2 magic) gate
but stalled on `CELL_SPURS_JOB_ERROR_MEMORY_SIZE (0x80410a17)`.

> Companion docs:
> - `docs/abi/spurs-job-entry-point.md` — tracked normative ABI spec
> - `docs/spurs-job-binary2-re.md` — gitignored RE journey (Sessions 1-7)
> - `~/.claude/projects/.../memory/project_spurs_job_runtime.md` — terse status

---

## TL;DR — what's working, what's open

**Working end-to-end:**
- Toolchain: `-mspurs-job` GCC + binutils flags, e_flags=1, all
  required ELF metadata (`.SpuGUID`, `.note.spu_name`,
  `_cell_spu_ls_param`, `.cell_spu_ls_param`, 0x80-aligned LOAD-data,
  `_start` at vaddr 0x10).  Reference `job_elf-to-bin --format=jobbin2`
  (run via wine) ACCEPTS our SPU ELF cleanly.
- Descriptor BIN2 magic: `binaryInfo[0..3] = "bin2"` (0x62696E32 BE)
  + `.long 0x62696e32` baked into `.before_text` at LS 0x20.  The
  JOB_DESCRIPTOR (0x80410a0b) gate is cleared.
- libspurs_job.a + spurs_job.ld + cmake `JOBBIN` flag + sample
  `samples/spurs/hello-spu-job/` all wired and building clean.

**Open: MEMORY_SIZE (0x80410a17) gate.**  Reported value at
`jobChain+0x84` is constant `0x0003F700` across every descriptor
variation tested (sizeBin 1/33/76/109, sizeStack 16/256, eaBinary
±0x100, embed flat-raw vs ELF vs jobbin2-wrapped).  The only escape
is `binaryInfo[4..7] = 0`, which trips `INVALID_BINARY` (0xa1f)
instead.

---

## Where the firing site is

Decoded in Session 7 via a patched RPCS3 SPU LLVM recompiler that
dumps every block containing `iohl r3, 0xa17`:

```
SPU LS 0x13f4 (in libsre.sprx-embedded SPU image, base 0x25988 in libsre.prx)

  ceqi  r16, r3, 1           ; r16 = (r3 == 1)?
  brz   r16, 0x1410           ; if r3 != 1, skip error
  lqr   r17, 0x48f8           ; load reported value
  ilhu  r3, 0x8041
  iohl  r3, 0xa17              ; r3 = 0x80410a17 = MEMORY_SIZE
  rotqbyi r4, r17, 8           ; r4 = high lane of r17 = 0x3F700
  brsl  lr, 0xc68              ; call error reporter
```

`r3` here is the return value of helper `LS 0x3f90`.  That helper
checks the dispatcher's persistent state at LS slots `0x4bb8`,
`0x4bc8`, `0x4bd8` (a workload-tracking table) and returns 1 when
the high halfword of `($7 = LS 0x4bd8) & fsmh_mask_from(LS 0x4bb8 / LS 0x4bc8)` is zero.

The `cause = 0x10010c00` reported in `jobChain.cause` is in OUR
PPU app's BSS LOAD segment (vaddr 0x10010000..0x10051318), NOT in
libsre — it likely points at our `s_job` descriptor.

---

## Why every patch attempt failed

Three patches were tried in Session 7, all confirmed firing in the
RPCS3 log but NONE silenced the error:

1. **File-level patch** of decrypted libsre.prx (changing the brz
   at LS 0x13f8 to unconditional br): NO EFFECT.  RPCS3 reads the
   encrypted `.sprx` at runtime and ignores the `.prx`.
2. **JIT-time patch** in `compile()` (modifying `_func.data`):
   pattern matched and patch fired (logged "patched LS 0x13f8
   brz->br"), but MEMORY_SIZE still appeared in jobChain.error.
3. **Error reporter neutering**: replace first instruction of LS
   0xc68 (`ori $12, $3, 0`) with `bi $0` to make the SPU error
   reporter return immediately.  Patch fired (logged "patched LS
   0xc68 ori->bi"), but error still appeared.

**Conclusion**: the error code is being written to PPU memory by a
path we haven't located yet.  Exhaustive byte-level scan of every
firmware module confirms NO construction of 0x80410a17:

- 0 hits of the literal 4 bytes `80 41 0a 17` anywhere.
- 0 hits of PPU `lis r3, 0x8041 + ori r3, r3, 0xa17` in libsre.prx
  (1 hit in libspurs_jq.prx, but that module isn't loaded).
- 0 hits of PPU `lis r3, 0x8041 + addi r3, r3, 0xa17`.
- 0 hits of any 4-byte-aligned PPU constant-construction
  instruction with imm `0x0a17` in libsre.prx.
- 4 SPU `iohl r3, 0xa17` sites in libsre.prx (LS 0x131c, 0x1404,
  0x5214, 0x52fc), but our patches to neutralise them had no effect.
- RPCS3 PPU HLE source: only `STR_CASE` for logging; no
  `return CELL_SPURS_JOB_ERROR_MEMORY_SIZE` path; only one
  `jobChain->error` write site, and it's a clear-to-zero on
  RunJobChain entry.

---

## Concrete next-session attack plan

### 1. RPCS3 GDB stub memory-write watchpoint (most direct)

RPCS3 has a built-in GDB server enabled by default at
`127.0.0.1:2345` (config key: `Misc → GDB Server`).  Standard
GDB-remote protocol with extensions for SPU.

Steps:
1. Boot the sample SELF in RPCS3:
   `/home/firebirdta01/source/repos/RPCS3/build/bin/rpcs3 --no-gui /home/firebirdta01/source/repos/PS3_Custom_Toolchain/samples/spurs/hello-spu-job/hello-spu-job.fake.self`
2. While running, attach GDB:
   `powerpc64-ps3-elf-gdb -ex 'target remote :2345'`
3. Find the runtime address of `&jobChain.error`.  The PPU sample
   already prints the JC bytes; from the post-Run dump we know
   `&jobChain` is on the heap (via `new`).  Grep RPCS3.log for the
   `cellSpursCreateJobChainWithAttribute(jobChain=*0x...)` trace
   line, that gives the PPU EA of jobChain.  `&jobChain.error` =
   that + 0x80.
4. Set memory-write watchpoint:
   `(gdb) watch *(int *)0x<jobChain_addr+0x80>`
5. `(gdb) continue` and let the chain run.  When the value
   `0x80410a17` is written, GDB halts the offending CPU thread
   (PPU or SPU).  Inspect PC + registers to identify the writer.

Implementation in RPCS3: see `rpcs3/Emu/GDB.cpp` and
`rpcs3/Emu/GDB.h`.  The `Z2/z2` packets handle write watchpoints
(see `gdb_thread::cmd_set_breakpoint`).  RPCS3's GDB stub may have
limitations on memory watchpoints — verify support before relying.

### 2. Build a known-good SDK sample under wine (parallel attack)

The SDK sample `cell-sdk/475.001/cell/samples/sdk/spu_library/libspurs/job/job_hello/`
uses `cellSpursCreateJobChainWithAttribute` exactly like our sample.
Its job descriptor's `binaryInfo[10]` is filled from a
`_binary_job_job_hello_jobbin2_jobheader` symbol that the wrapping
pipeline emits.  If we can build this sample, we get a known-good
descriptor to compare against.

Blocker: `wine spu_elf-to-ppu_obj.exe --format=jobbin2 in.elf out.o`
spawns `ppu-lv2-gcc` (the proprietary PPU toolchain) which we don't
have.  Workaround: the tool's output is just an ELF object file
with the wrapped binary embedded as `.jobbin2_<name>` and a
`_binary_*_jobbin2_jobheader` symbol.  Reimplementing that step is
straightforward once we know the byte layout of the
`CellSpursJobHeader` struct it emits — and the wrapped binary it
emits we already produce cleanly via `wine job_elf-to-bin
--format=jobbin2 in.elf out.bin`.  Compare:

- Run `wine job_elf-to-bin --format=jobbin2 hello_spu_job.bin /tmp/job-current.bin`
  (already works — outputs a 1216-byte stripped ELF).
- The PPU-side `_binary_*_jobbin2_jobheader` symbol that the
  full SDK pipeline would emit is a `CellSpursJobHeader` literal
  (48 bytes).  Find its layout by either:
  (a) Disassembling `spu_elf-to-ppu_obj.exe`'s
  `__ZN15ExternalCommand15generateJobBin2ESsSsb` (already located at
  PE address 0x405690; "is not valid Job ELF" string at 0x42dff8 is
  printed by this function).  See its assembly to learn what bytes
  it writes into the `binaryInfo[10]` field.
  (b) Or running the tool against a SDK sample's pre-wrapped binary
  if we can find one (no ELF SPU jobs ship pre-built in the SDK).
  (c) Or installing the proprietary PPU toolchain and running the
  full SDK build.

### 3. The slot-table init mystery

The helper at LS 0x3f90 returns 1 when LS slot 0x4bd8's high halfword
masked by state from LS 0x4bb8/0x4bc8 is zero.  The init at LS
0x3dc0..0x3dec sets:
- `LS 0x4bb8 = 0`
- `LS 0x4bc8 = ?` (not seen yet — find its init)
- `LS 0x4bd8 = -1` (= all 0xFF bytes)
- `LS 0x4be8 = 0`, `LS 0x4bf8 = 0`

Maybe LS 0x4bc8 stays 0 in our case but should be populated by
some `cellSpursCreateJobChainWithAttribute` → SPU registration
path.  Trace what writes LS 0x4bc8 first time on a successful run.
Sites: LS 0x3d5c, 0x3e64, 0x3f68, 0x437c, 0x43b4 (`stqr` to 0x4bc8
in the dispatcher SPU image).  The function at LS 0x3d20 is the
"add a workload to the table" routine; called once from LS 0x1294.
What conditions reach LS 0x1294?

---

## Reference — artefacts ready for the next session

| Path | Contents |
|------|----------|
| `samples/spurs/hello-spu-job/` | Sample halts at MEMORY_SIZE; PPU side dumps `s_job[0..0x40]` + `jobChain[0x70..0xa0]` so the dispatcher's view is observable.  Build via cmake; `hello-spu-job.fake.self` ready to boot. |
| `docs/abi/spurs-job-entry-point.md` § 6.1-6.2 | Tracked binary contract for `binaryInfo[10]` BINARY2 layout. |
| `docs/spurs-job-binary2-re.md` Sessions 1-7 | RE journey log (gitignored).  Session 7 has the full SPU disasm of the firing site + helper functions. |
| `~/.config/rpcs3/dev_flash/sys/external/libsre.prx` | Decrypted libsre (read via `rpcs3 --decrypt`).  Dispatcher SPU image at file offset `0x25988` (= LS 0).  Use for offline disassembly only — RPCS3 ignores .prx at runtime. |
| `/tmp/spurs-dispatcher/disp2.bin` | 64KB extraction of the dispatcher SPU image at the correct base.  `spu-elf-objdump -b binary -m spu --adjust-vma=0` works on it. |
| `~/source/repos/RPCS3/rpcs3/Emu/Cell/SPULLVMRecompiler.cpp` | Modified locally: `compile()` now force-logs functions containing `iohl r3, 0xa17` or `iohl r3, 0xa0b` (BE 0x60850b83 / 0x60850583, LE bswap 0x830b8560 / 0x83058560).  Existing diagnostic; keep it. |
| `~/source/repos/RPCS3/build/bin/rpcs3` | Locally-built RPCS3 with the diagnostic patch.  Boot SELFs through this binary, not `/usr/sbin/rpcs3`, to keep the dump filter active. |
| `/tmp/job-current.bin` | A clean wrapped jobbin2 of our SPU ELF (produced via `wine job_elf-to-bin --format=jobbin2`).  1216 bytes, stripped ELF shape, "bin2" baked into `.before_text` at LS 0x20. |

## Reference — the firing-site disassembly (LS 0xefc..0x141c)

Function entry at LS 0xefc.  `r80` (callee-saved) is set from `r3`
(first arg) at LS 0xf00; everything that mentions `$80`/`$81` traces
back to that first arg.  The full validation chain:

```
0xefc:  function entry; saves callee-saved regs; r80 ← r3 input
0xf18-0xf28: more arg saves; r81 ← r80
0xf54:  brsl 0x3bd0 (call mem-size validator helper, returns r3 = KB count)
0xf64:  brhz r2, 0x1328 (early success exit if helper r3 byte == 0)
0xfa0:  brsl 0x3f90 (call helper that returns 0 OK / 1 → MEMORY_SIZE)
0xfa4:  ceqi r9, r3, 1
0xfa8:  brz r9, 0x1328 (early success exit if helper r3 != 1)
0xfac+: byte-14-of-$81 check (needs to be 0 or 1)
0xfb8:  brhz r10, 0x1008 (taken if $81.byte[14] != 0)
0x1010: brhz r18, 0x1314 (taken if $81.byte[14] != 1) ← MEMORY_SIZE site 1
... fall through if $81.byte[14] == 1 ...
0x13f4: ceqi r16, r3, 1; brz r16, 0x1410
0x1404: iohl r3, 0xa17 ← MEMORY_SIZE site 2 (this is the one we hit)
```

Two duplicate copies of this validator exist around LS 0x52xx
(probably for tasks vs jobs — same logic, different LS slots).

---

## Things to NOT spend time on

- **Patching libsre.prx file**: confirmed ineffective; RPCS3 uses
  the encrypted `.sprx`.
- **Changing descriptor `sizeStack` / `sizeBinary` / `sizeScratch`**:
  confirmed independent of MEMORY_SIZE.  The 0x3F700 reported value
  is constant.
- **Bin2 magic placement / wrapping format**: that's working; the
  JOB_DESCRIPTOR gate is cleared.  Don't regress.
- **Rebuilding the SPU image with different layouts**: layout is
  good per the wrapping tool's acceptance.

## Questions to answer in the next session

1. **What writes `0x80410a17` to PPU memory?**  GDB watchpoint
   should resolve this.  If it's a PPU write site, check whether
   the byte search missed a multi-step construction.  If SPU, check
   whether there's a second error-DMA path the LS 0xc68 patch
   didn't cover.
2. **What initialises LS slot 0x4bc8 in a successful run?**  Trace
   the workload-registration path that should fire when our chain
   is created.  May reveal that some `cellSpurs*` API we're not
   calling (e.g. `cellSpursAddWorkload` directly) is required for
   BINARY2 jobs.
3. **What does an SDK-emitted `_binary_*_jobbin2_jobheader` look
   like?**  Compare its bytes to our hand-rolled descriptor to find
   the field we're getting wrong.
