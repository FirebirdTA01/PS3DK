# Next session — finish SPU spurs-job runtime bring-up

Pickup prompt for resuming the `cellSpursJobMain2` end-to-end work.
Two BINARY2 gates have now been cracked or partly cracked.  Frames
the remaining gap, what's already wired, what to try first, and how
to validate.

> **2026-04-28 update:** Session 3 cracked the BIN2 magic gate.
> Setting `binaryInfo[0..3] = "bin2"` (`0x62696E32` BE) passes the
> SPU dispatcher's first descriptor check at LS `0x23a4..0x23cc` in
> the `libsre.prx`-embedded SPU image.  The halt code flips from
> `0x80410a0b` (JOB_DESCRIPTOR) to `0x80410a17`
> (`CELL_SPURS_JOB_ERROR_MEMORY_SIZE`).  Reported memory budget at
> `jobChain+0x84` is `0x0003F700` = 260864 bytes (>234*1024 max).
> Crucially, varying `CellSpursJobHeader.sizeStack` (256→16 qwords)
> does NOT change that value — so the dispatcher reads the memory
> budget from elsewhere, almost certainly from a metadata header
> the wrapped binary at `eaBinary` is supposed to start with.  Our
> `eaBinary` currently points to a flat raw image; the dispatcher
> parses the `_start` xori-dance bytes as wrapper metadata and gets
> garbage.  Cracking the wrapper layout is the new frontier.
>
> Tracked ABI doc updated: `docs/abi/spurs-job-entry-point.md` § 6.1
> ("`binaryInfo[10]` for `CELL_SPURS_JOB_TYPE_BINARY2`") + § 6.2
> ("BINARY2 wrapper (open question)").  Full RE narrative for this
> session lives in this file's "Session 3" entry.

> Companion docs:
> - `docs/abi/spurs-job-entry-point.md` — normative ABI spec (tracked)
> - `docs/spurs-job-binary2-re.md` — full RE journey log (gitignored)
> - `~/.claude/projects/.../memory/project_spurs_job_runtime.md` — terse status memory

---

## TL;DR — what's working, what isn't

**Toolchain side: complete and verified.**  Our SPU GCC + binutils
patches add `-mspurs-job` / `-mspurs-job-initialize` / `-mspurs-task`
driver flags that propagate through to `--spurs-job` linker options
that stamp `e_flags = 1/2/3` into the SPU ELF header.  `readelf -h`
on a `-mspurs-job`-built ELF reports `Flags: 0x1`.  Sample
`samples/spurs/hello-spu-job/` builds cleanly with `CFLAGS -mspurs-job`
and `LDFLAGS -mspurs-job -Wl,-q`.  Raw image (post-`spu-elf-objcopy
-O binary`) is byte-identical to what the reference SDK's
`job_elf-to-bin --format binary` produces from a `e_flags=1` ELF.

**Runtime side: blocked.**  The PPU sample halts the chain with
`statusCode=0x80410a0b` (matches the SDK header
`CELL_SPURS_JOB_ERROR_JOB_DESCRIPTOR`).  However:

- That error code is **never returned** by the real PS3 firmware
  libraries.  Byte-pattern + `lis r3,0x8041 + ori r3,r3,0xa0b`
  instruction-pair search across the entire decrypted `libsre.prx`
  and `libspurs_jq.prx` shows zero occurrences.
- The error code is **never returned** by RPCS3 source either —
  only present as an enum entry and a logging `STR_CASE`.  No
  `return CELL_SPURS_JOB_ERROR_JOB_DESCRIPTOR` path anywhere in
  master (commit `3b21833`) or in the user's installed v0.0.40-19217
  binary commit `3b9cc0bc` (cellSpurs.cpp diff between them = empty).
- The dispatcher SPU code (`spurs_jm2.elf`) ships as **empty stub**
  in both RPCS3's dev_flash and the cell-sdk's `images/` dir.  Its
  `.text` is zero-filled; symbols + debug info only.  Real
  implementation must be loaded by RPCS3 at runtime from somewhere
  we haven't located.

So `0x80410a0b` is most likely either uninitialized memory at
`&jobChain->error`, or written by an SPU dispatcher we haven't
located.  Continuing static disassembly is unlikely to crack it —
the next move is **live observation under RPCS3's debugger**.

---

## Concrete next-session checklist

### 1. Confirm what value is actually in `jobChain->error`

Add a raw-byte dump to the PPU sample so we know the field is what we
think it is rather than a struct-offset misread.

In `samples/spurs/hello-spu-job/source/main.cpp`, before the polling
loop, add:

```cpp
#include <inttypes.h>
// Right after cellSpursRunJobChain returns ok:
const uint8_t *jc_bytes = (const uint8_t *)jc;
std::printf("  jc raw bytes [0x70..0xa0]:\n");
for (int row = 0; row < 3; ++row) {
    std::printf("    %02x:", 0x70 + row * 16);
    for (int col = 0; col < 16; ++col)
        std::printf(" %02x", jc_bytes[0x70 + row * 16 + col]);
    std::printf("\n");
}
```

Repeat the dump inside the polling loop right after `info.isHalted == 1`.
The interesting offset is `0x80` (RPCS3's `CellSpursJobChain.error`);
`0x88` is `cause`.  Rebuild + rerun + paste output.  If the bytes at
`0x80..0x83` aren't `80 41 0a 0b`, our struct interpretation is
wrong.  If they are, we need (2) below.

### 2. RPCS3 debugger live inspection

If (1) confirms the bytes truly are `80 41 0a 0b`, fire up RPCS3's
GUI debugger:

- Launch RPCS3 GUI (binary is `/usr/sbin/rpcs3`).
- Boot game/SELF: drag-drop `samples/spurs/hello-spu-job/hello-spu-job.fake.self`
  or use `Boot SELF`.
- After the chain starts running, open
  `Debugger → CPU Debugger`, switch to PPU thread.
- Set a breakpoint on `cellSpursGetJobChainInfo` entry (search
  by symbol or by the function's PPU EA from the loaded SELF's
  symbol table).
- When breakpoint fires, inspect memory at `jobChain` argument's EA
  + 0x80 (the `error` field).  Watch what writes that location
  (RPCS3 has memory-write breakpoints).
- The writer's PPU PC will tell us which RPCS3 module / which loaded
  SPRX is actually setting that field.  That's the direct lead.

Alternative: RPCS3 supports a GDB stub on TCP localhost:2345 (set
in `Settings → Debug → GDB server`).  Connect with our
`powerpc64-ps3-elf-gdb` and use scripted commands.  That side-steps
the GUI entirely.

### 3. Find the actual SPU dispatcher (if needed)

If (2) reveals the writer is an SPU thread, we still need the real
`spurs_jm2` SPU code.  Likely sources:

- The PUP firmware payload — RPCS3 may extract it during install.
  Check `~/.cache/rpcs3/cache/spu-*` for any `spurs_jm2`-shaped blob.
- A real game's SPRX cache.  Pick any title that uses spurs job-chain
  (most retail games do) and look in its cache dir.
- `spu-elf-objdump --start-address=0xa10` against any .elf RPCS3
  loaded into LS during a real game's spurs run.

### 4. If the writer turns out to be RPCS3 HLE we missed

RPCS3 may set `jobChain->error` from a path we didn't grep.  Pull a
fresh `RPCS3.log.gz` after running our sample with `Settings → Debug
→ Log Level: Trace`, then grep for `cellSpurs`, `JobChain`,
`workload`, `0x80410`, our SELF name, and the workload ID.  The
trace log shows every HLE function entry; whichever one writes the
error will surface.

### 5. Once we know the actual format

Update `samples/spurs/hello-spu-job/source/main.cpp` to write the
correct `binaryInfo[10]` bytes (the JOBBIN code-emit pipeline doesn't
need to change — only the descriptor encoding step).  Rebuild,
re-run.

Expected SUCCESS output:

```
hello-spu-job: bring-up
  Spurs2 up
  BINARY2 jobbin fill: ...
  RunJobChain ok; polling for sentinel ...
  [round 0] isHalted=0 statusCode=0x0 ...
  Joined ok

=== SPU job sentinel buffer ===
  s_out[0] (magic)             = 0xc0ffee99  (expected 0xc0ffee99)
  s_out[1] (jobContext.dmaTag) = N
  s_out[2] (eaBinary lo)       = 0xNNNNNNNN
  s_out[3] (eaBinary hi)       = 0x00000000

SUCCESS
```

If sentinel matches, the SPU job ran end-to-end.  Update the
"Status (2026-04-27)" note in `docs/abi/spurs-job-entry-point.md` § 6
to remove the open-question banner and add the BINARY2 layout to the
spec.

---

## Reference — what's where

| Artefact                                                       | Purpose                                          |
|----------------------------------------------------------------|--------------------------------------------------|
| `sdk/libspurs_job/`                                            | SPU job runtime: `_start` trampoline + `cellSpursJobInitialize` + `spurs_job.ld` |
| `sdk/include-spu/` *(SIMD, separate)*                          | (unrelated; kept here only for orientation)      |
| `samples/spurs/hello-spu-job/`                                 | Sample under test; current state halts at descriptor rejection |
| `cmake/ps3-self.cmake`                                         | `JOBBIN` flag on `ps3_add_spu_image` runs `spu-elf-objcopy -O binary` post-link |
| `patches/spu/binutils-2.42/0001-spu-elf-spurs-job-flags.patch` | linker `--spurs-job` etc.                         |
| `patches/spu/gcc-9.5.0/0003-spu-spurs-job-driver-flags.patch`  | gcc `-mspurs-job` etc.                            |
| `docs/abi/spurs-job-entry-point.md`                            | tracked normative ABI spec                       |
| `docs/spurs-job-binary2-re.md`                                 | gitignored RE journey log (read me first!)       |
| `~/source/repos/RPCS3/`                                        | RPCS3 source clone, master branch                |
| `~/.config/rpcs3/dev_flash/sys/external/lib*.prx`              | decrypted firmware libraries (146 .prx files)    |
| `~/.config/rpcs3/dev_flash/sys/external/spurs_jm2.elf`         | empty SPU dispatcher stub (zero `.text`)         |
| `cell-sdk/475.001/cell/target/images/spurs_jm2.elf`            | also-empty SDK image stub                        |

## Reference — known firmware error codes

From byte-pattern + `lis r3,0x8041 + ori r3,r3,IMM` scan of decrypted
`libsre.prx` (511 lis sites) and `libspurs_jq.prx` (134 lis sites).
The IMMs that exist:

| Code         | Name                  | libsre sites | libspurs_jq sites |
|--------------|-----------------------|-------------:|------------------:|
| `0x80410a01` | AGAIN                 |            0 |                 6 |
| `0x80410a02` | INVAL                 |            5 |                16 |
| `0x80410a09` | PERM                  |            0 |                 1 |
| `0x80410a0a` | BUSY                  |            1 |                 8 |
| **`0x80410a0b`** | **JOB_DESCRIPTOR** | **0**        | **0**             |
| `0x80410a0f` | STAT                  |            3 |                 9 |
| `0x80410a10` | ALIGN                 |            9 |                18 |
| `0x80410a11` | NULL_POINTER          |            5 |                22 |

`0x80410a0b` is conspicuously absent — the SDK header defines the
constant but the runtime never emits it.  This is the smoking gun
that tells us the value our test reads is *not* coming from a real
firmware return path.

## Reference — `cellSpursJobHeaderSetJobbin2Param` NID

`0x97a2f6c8` (located in `libsre.prx` at file offset `0x1e1a0` in a
NID export table; need to walk the parallel address table to find
the function's PPU EA in libsre).  Disassembling this function would
show what byte layout the helper PRODUCES into `binaryInfo[10]` —
which is what the runtime READS.  Try this if (1)/(2) above don't
crack it directly.
