# GCM command segments

A command segment is a small command buffer outside the main ring. The ring
runs it with `cellGcmSetCallCommand(offset)` and the segment ends with
`cellGcmSetReturnCommand`. Segments let a frame's drawing be built ahead of
time, by the PPU or by an SPU job, and then published with one word in the
ring.

This page covers the SDK pieces, the rules for using them and what has been
measured. Each rule says what backs it: a host test, an RPCS3 run of a
sample, or nothing yet (**UNVERIFIED**). Console rows come from one run per
signed SELF on a PS3 through the ps3dk-run loader on 2026-11-10; the evidence
records are named `hw-s-<sample>`.

## The pieces

| Piece | Where | What it does |
|---|---|---|
| `cellGcmSetupContextData(ctx, addr, size, callback)` | `<cell/gcm.h>`, libgcm_cmd | Points a `CellGcmContextData` at any buffer. Every `cellGcm*` and `rsx*` emitter then writes there instead of the ring. `size` is in bytes; the last word is kept free (`end = addr + size - 4`). When an emitter needs more room it calls `callback(ctx, words)`; a non-zero return means the emitter writes nothing for that reservation. |
| Segment helper `cellGcmSegment*` | `<cell/gcm/gcm_segment.h>`, libgcm_cmd | Allocation-free segment objects over caller memory: Init, Begin/End (PPU writer), BeginExternal/EndExternal (another writer, such as an SPU), Call, IsRetired, WaitRetired, GetUsedBytes, GetOverflowWords. |
| SPU writer | `<cell/gcm/gcm_spu_cmd.h>` (SPU include), libgcm_spu_cmd | The `cellGcm*` calls on the SPU, writing into local store, plus `cellGcmSpuPutSegment` to DMA the result to a main-memory segment. |
| SPU prepare helpers | `<cell/gcm/gcm_spu_prepare.h>` (PPU, header only) | Resolve the Cg-handle calls once on the PPU into blocks the SPU calls take. |

Samples: `samples/gcm/hello-ppu-gcm-call-return`,
`samples/gcm/hello-ppu-gcm-nested-call`, `samples/gcm/hello-spu-gcm-segment`.
Host tests: `tests/sdk/gcm-segment-test.sh`, `tests/sdk/gcm-spu-cmd-test.sh`.

## Segment lifecycle (PPU writer)

```c
CellGcmSegment seg[2];
cellGcmSegmentInit(&seg[k], buffer_k, 64 * 1024, CELL_GCM_LOCATION_MAIN, LABEL_k);

CellGcmSegment *s = &seg[frame & 1];
if (cellGcmSegmentWaitRetired(s, 4000, 500, NULL) != CELL_OK) fail();   /* <= 2 s */
CellGcmContextData *c = cellGcmSegmentBegin(s);
cellGcmSetClearColor(c, 0xff2040c0);
cellGcmSetClearSurface(c, CELL_GCM_CLEAR_R | CELL_GCM_CLEAR_G | CELL_GCM_CLEAR_B);
if (cellGcmSegmentEnd(s) != CELL_OK) fail();      /* overflow is refused here */
cellGcmSegmentCall(ring, s);                      /* CALL + retirement label */
```

* **Init** takes RSX-visible main memory: mapped with
  `cellGcmMapMainMemory` (or inside the `cellGcmInit` IO region), with
  `CELL_GCM_LOCATION_MAIN`. It records the IO offset and sets the segment's
  label to 0. `CELL_GCM_LOCATION_LOCAL` is refused with
  `CELL_GCM_SEGMENT_ERROR_INVALID_VALUE` (CALL offsets are IO offsets, see
  Limits); like every Init refusal it leaves the segment and its label
  untouched. Host test: `gcm-segment-test.sh`.
* **Begin** sets up the segment context with `cellGcmSetupContextData` and
  a callback that never wraps or flushes. It refuses (NULL) a segment that is
  open or still in flight.
* **End** appends the RETURN into the kept last word. If any emitter was
  refused since Begin it returns `CELL_GCM_SEGMENT_ERROR_OVERFLOW`, and the
  segment cannot be called until it is rebuilt. Bytes used, RETURN included,
  are in `cellGcmSegmentGetUsedBytes`; refused words in
  `cellGcmSegmentGetOverflowWords`.
* **Call** writes, into the ring, the CALL and then a back-end label
  (`cellGcmSetWriteBackEndLabel`) carrying the segment's next serial. The
  five words are one reservation: if the ring lacks room, its callback is
  asked once for all five (it may wrap or flush). In the observed AUTO build
  (`libgcm_cmd_prof_auto.a`) the reservation is nine words: the five plus
  the automatic profiler's 4-word operation-label release, asked for at once.
  If the callback refuses, or returns without making room, Call returns
  `CELL_GCM_SEGMENT_ERROR_BUSY`, emits no CALL or label, does not call the
  profiler's observer, and leaves the segment unchanged, closed and callable. It
  cannot undo anything the ring's own callback did to the ring before
  refusing (a flush or a wrap, say). Host test:
  `gcm-segment-test.sh` (1 to 4 free words with a refusing callback; a
  wrapping callback puts all five words after the wrap).
* **WaitRetired** polls that label at most `maxPolls` times, sleeping between
  polls, and returns `CELL_GCM_SEGMENT_ERROR_TIMEOUT` instead of waiting
  forever. Serials compare modulo 2^32.

Overflow is never silent: a refused command leaves nothing behind, the count
is kept, and the helper will not CALL the segment. An emitter that reserves
in several pieces (some Cg bridge calls) can have written its first pieces
before a later one is refused; the segment is still marked overflowed. Host
test: `gcm-segment-test.sh` (a 32-word segment keeps 15 two-word commands,
refuses 10 words, End reports OVERFLOW, Call refuses, nothing is written
past the buffer; a control build whose callback wraps like a ring callback
must fail those checks).

### Another writer (SPU, DMA)

`cellGcmSegmentBeginExternal` claims a retired segment and returns its
buffer; the PPU's Begin/End refuse it meanwhile.
`cellGcmSegmentEndExternal(seg, usedBytes)` closes it after checking that
`usedBytes` is a multiple of 4 inside the buffer and that the last word is
the RETURN.

## Coherence recipe for SPU-built segments

`hello-spu-gcm-segment` uses this order every frame:

1. PPU: `cellGcmSegmentWaitRetired` — the RSX is done with the old contents.
2. PPU: `cellGcmSegmentBeginExternal`, write the job record, `sync`, then
   wake the SPU (mailbox).
3. SPU: build the commands in local store, `cellGcmSpuEnd`,
   `cellGcmSpuPutSegment` (DMA, then wait for its tag), and only then DMA the
   completion record.
4. PPU: poll the completion record (bounded), `sync`, then
   `cellGcmSegmentEndExternal` and `cellGcmSegmentCall`.

The SPU waits for the segment's tag before it writes the completion, so the
PPU cannot see the completion before the segment's bytes are in memory. The
PPU's `sync` orders its later reads and the CALL after the completion it
saw; `cellGcmFlush` publishes the ring.

Measured on RPCS3 and on the console (`hw-s-spu-segment`): 120 SPU-built
frames, 320 bytes each (the measured size matched the bytes written), 0
progress mismatches, the SPU's one-shot overflow check refused 26 words, and
the SPU thread joined.

## Limits and alignment (B3)

Rules, all measured on a PS3 unless marked:

- Chain sub-segments with `cellGcmSetJumpCommand`; use one CALL level only (a nested CALL stalls the front end).
- CALL and JUMP offsets are IO offsets. A local-memory offset is fetched from the equal IO offset, so segments
  live in main memory mapped with `cellGcmMapMainMemory` (or the `cellGcmInit` IO region).
- Never set up a GCM context in RSX local memory. Writing segments straight into local memory hung the PPU
  (`hw-s-call-return-local`: the last crumb is the phase start, before the first segment was finished); copying
  the same segments in with 32-bit stores did not (`hw-s-localcopy`). The cellGcm and rsx emitters use 8-byte
  `std` stores into the command buffer: a disassembly of `libgcm_cmd.a` finds them in 43 emitters in both ABIs
  (for example `rsxClearSurface`, `rsxSetVertexProgramConstants`, `rsxDrawVertexArray`). That the hang is an
  alignment fault from a misaligned 8-byte store to RSX local memory is INFERRED, not measured.

| Rule | Backing |
|---|---|
| The CALL word carries a 4-byte-aligned offset below 512 MiB (`offset & 0x1ffffffc`, flag `0x2`). | Encoding; RPCS3's FIFO masks the same bits. |
| The helper requires 128-byte alignment and a size that is a multiple of 128, at least 128 bytes. | Helper rule (keeps segments whole SPU DMA lines). |
| A 1 MiB segment (NOPs plus a label) runs and retires. | RPCS3 (desktop build, 2026-11-10, ILP32): `GCM_SEG_LARGE ok bytes=1048576 used=1048576`. Console (PS3, HEN, 2026-11-10, signed ILP32 SELFs, evidence `hw-s-*`): `GCM_SEG_LARGE ok bytes=1048576 used=1048576 polls=2`, exit 0 (`hw-s-call-return-large`). |
| Segments in main memory work. | Host test, and RPCS3 (desktop build, 2026-11-10, ILP32): 120 frames from two 64 KiB segments in `cellGcmMapMainMemory` memory, 0 progress mismatches; `hello-spu-gcm-segment` 120 SPU-built frames. Console (PS3, HEN, 2026-11-10, signed ILP32 SELFs, evidence `hw-s-*`): `GCM_SEG_PASS variant=main frames=120`, 360-byte segments, 0 progress mismatches, exit 0 (`hw-s-call-return`); `GCM_SPUSEG_PASS frames=120 ... exact_fit=yes` (`hw-s-spu-segment`). |
| Segments in RSX local memory. | **Not supported: CALL and JUMP offsets are IO offsets.** RPCS3 (desktop build, 2026-11-10, ILP32): `LOCAL-IO-ALIAS`, all 120 CALLs to a local-memory offset ran the decoy placed at the equal IO offset. Console (PS3, HEN, 2026-11-10, signed ILP32 SELFs): the same on silicon (`hw-s-localcopy`): segments built in main memory, copied into RSX local memory with 32-bit stores and CALLed at local offset 0x01400000 ran the decoy at IO offset 0x01400000 in all 120 frames (`outcome=LOCAL-IO-ALIAS`, exit 0). **Segments live in mapped main memory.** Those runs used probe variants (`-local`, `-localcopy`) that built local segments through the helper. `cellGcmSegmentInit` now refuses `CELL_GCM_LOCATION_LOCAL` with `CELL_GCM_SEGMENT_ERROR_INVALID_VALUE`: `-localcopy` is gone and `-local` checks the refusal (`GCM_SEG_LOCAL refused ... untouched=yes`). Host test: `gcm-segment-test.sh`. |
| Nested CALL (a segment that CALLs another segment). | RPCS3 (desktop build, 2026-11-10, ILP32): not supported. The ring CALL and a single-level CALL both worked (`GCM_NEST_CONTROL ring=ok single_call=ok`); the nested CALL logged `FIFO: CALL found inside a subroutine` 21 times, then `Dead FIFO commands queue state has been detected!` and RPCS3 froze emulation, so the sample could not print its label report. Console (PS3, HEN, 2026-11-10, signed ILP32 SELFs, evidence `hw-s-*`): **stalls**. `GCM_NEST_LABELS A=2 I=1 B=0 R=1` (I=1 and R=1 are the control's values): the outer segment ran up to the inner CALL and nothing after it, the bounded wait on label 209 expired, the post-probe flip timed out (`NESTED-STALLED`), and the loader came back without READY (`hw-s-nested`). **One CALL level only.** Join sub-segments with `cellGcmSetJumpCommand` instead (`hello-ppu-gcm-call-return-jump`). |
| A JUMP from a CALLed segment to a second sub-segment, which RETURNs. | RPCS3 (2026-11-10) and console (`hw-s-jump`): `GCM_SEG_PHASE name=jump frames=120 calls=120 jumps=120 ... progress_mismatches=0`, exit 0. **Chain sub-segments with JUMP; one CALL level only.** |

## State across CALL and RETURN (B4)

| Rule | Backing |
|---|---|
| Render state set inside a segment (scissor, clear colour) is still set after RETURN: the ring's clear after the CALL uses the segment's scissor and colour. | RPCS3 (desktop build, 2026-11-10, ILP32): the yellow (PPU sample) and cyan (SPU sample) indicator boxes appear in the screenshots: the ring clear after RETURN used the scissor and colour the segment left. Console: the yellow indicator was on screen in the first console run of `hello-ppu-gcm-call-return` (observed on the display; no capture). |
| A surface (render target) set inside a segment applies to the segment's clears. | RPCS3 (desktop build, 2026-11-10, ILP32): `hello-ppu-gcm-call-return` sets the surface inside every segment and draws into both buffers. Console (PS3, HEN, 2026-11-10, signed ILP32 SELFs, evidence `hw-s-*`): the same sample completed 120 flipped frames at 1920x1080 (`hw-s-call-return`). |
| Command (front-end) labels written inside a segment arrive. | Host test, and RPCS3 (desktop build, 2026-11-10, ILP32): every per-slot progress label matched its frame in both samples. Console (PS3, HEN, 2026-11-10, signed ILP32 SELFs, evidence `hw-s-*`): 0 progress mismatches in `hw-s-call-return` and `hw-s-spu-segment`. |
| Back-end retirement labels belong in the ring after the CALL (the helper does this), not inside the segment: a segment can be reused only after the front end has also left it. | Design rule. |
| Flip (`cellGcmSetFlip`) and wait-flip inside a segment. | **UNVERIFIED**: the samples keep flips in the ring. Keep them there. |
| Reports and timestamps inside a segment. | **UNVERIFIED**. |
| The FIFO wrap callback never runs for a segment: a segment context's callback refuses instead. Do not write ring-only commands that rely on the ring callback into a segment. | Host test. |

## Labels

The samples use labels 200-215, named in
`samples/gcm/common/gcm_segment_labels.h`. They avoid the indices the SDK
itself uses: libPSGL (65-68, 255) and the profiler (labels 240-247, of which
the samples use 240-243 and the automatic profiler 246-247; reports 0-511 and
1024-2047). Applications use reports 512-1023. The helper takes the label
index from the caller.

## SPU writer calls

All take a `CellGcmContextData *` (on the SPU, the writer's context type) and
emit the same words as the PPU call of the same name. Each call reserves its
whole command at once: written whole or refused whole. A measuring context
(`cellGcmSpuSetupMeasureContext`) writes nothing and counts the exact words,
so a job can size its buffer exactly (`cellGcmSpuGetUsedBytes` + 4 for the
RETURN).

Refusals are sticky: the overflow count and the measured count saturate at
0xffffffff instead of wrapping, so no later refusal can bring the overflow
count back to zero, and `cellGcmSpuEnd` then returns
`CELL_GCM_SPU_ERROR_OVERFLOW`. `cellGcmSpuGetUsedBytes` still returns a
`uint32_t`: it computes the byte count with 64-bit arithmetic and saturates
at 0xffffffff when the count does not fit in 32 bits. A writing context over fewer
than 4 bytes has no room for the RETURN: it starts with an overflow count of
1, refuses every call, and End returns `CELL_GCM_SPU_ERROR_OVERFLOW` without
writing anything. `ctx->ended` is one of `CELL_GCM_SPU_CONTEXT_OPEN`,
`_ENDED` and `_NO_ROOM`. Host test: `gcm-spu-cmd-test.sh` (two refused
0x80000000-word reservations, a 2^32-byte measurement, 0- to 3-byte
contexts).

Sizes are computed without wrapping. A command whose size does not fit in
32 bits (`cellGcmSetVertexProgramConstants` with a count such as 0xf0f0f0ff,
`SetVertexProgramParameterBlock` with more than 0x3fffffff rows), or whose
count does not fit its 11-bit method field (`SetVertexProgramParameter` with
0x7ff or more words, a `SetFragmentProgramParameter` patch padded past 0x7ff
words), is refused before any write: a writing context writes nothing and
its overflow count saturates, a measuring context reports 0xffffffff bytes.

| SPU call | PPU counterpart it matches |
|---|---|
| `cellGcmSetNopCommand`, `SetJumpCommand`, `SetCallCommand`, `SetReturnCommand` | same names |
| `cellGcmSetWriteBackEndLabel`, `SetWriteCommandLabel`, `SetWaitLabel` | same names |
| `cellGcmSetClearColor`, `SetClearSurface`, `SetScissor`, `SetViewport` | same names |
| `cellGcmSetColorMask`, `SetColorMaskMrt`, `SetBlendEnable`, `SetBlendFunc`, `SetBlendEquation` | same names |
| `cellGcmSetDepthTestEnable`, `SetDepthMask`, `SetDepthFunc`, `SetCullFaceEnable`, `SetFrontFace` | same names |
| `cellGcmSetPolygonOffsetFillEnable`, `SetPolygonOffset` | same names |
| `cellGcmSetVertexDataArray`, `SetVertexData4f`, `SetFrequencyDividerOperation`, `SetInvalidateVertexCache` | same names |
| `cellGcmSetDrawArrays`, `SetDrawIndexArray` | same names (all batch splits) |
| `cellGcmSetVertexProgramConstants`, `SetVertexProgramParameterBlock` | same names |
| `cellGcmSetVertexProgramParameter(ctx, const CellGcmSpuVertexParameter *, values)` | `cellGcmSetVertexProgramParameter(ctx, CGparameter, values)`; resolve with `cellGcmSpuPrepareVertexParameter` |
| `cellGcmSetVertexProgram(ctx, words, count)` | `cellGcmSetVertexProgram(ctx, CGprogram, ucode)`; words from `cellGcmSpuPrepareVertexProgram` |
| `cellGcmSetFragmentProgram(ctx, words, count)` | `cellGcmSetFragmentProgram(ctx, CGprogram, offset)`; words from `cellGcmSpuPrepareFragmentProgram` (the ucode offset is fixed when preparing) |
| `cellGcmSetFragmentProgramParameter(ctx, const CellGcmSpuFragmentParameter *, values, offset)` | `cellGcmSetFragmentProgramParameter(ctx, CGprogram, CGparameter, values, offset)`; resolve with `cellGcmSpuPrepareFragmentParameter` |
| `cellGcmSetUpdateFragmentProgramParameter` | same name |
| `cellGcmSetTexture`, `SetTextureControl`, `SetTextureFilter`, `SetTextureAddress`, `SetInvalidateTextureCache` | same names (`CellGcmSpuTexture` has the 24-byte `CellGcmTexture` layout) |

The PSL1GHT spellings are thin aliases of the calls above:
`rsxSetBlendFunc`, `rsxSetBlendEquation`, `rsxSetScissor`, `rsxLoadTexture`,
`rsxTextureControl`, `rsxTextureFilter`, `rsxTextureWrapMode`,
`rsxInvalidateTextureCache`, `rsxDrawIndexArray` (PSL1GHT argument order).
`gcm-spu-cmd-test.sh` checks every call and alias word for word against the
PPU emitter (70 cases, including the measuring, refusal and exact-fit paths),
and requires a mutant with one wrong method address to fail.

`cellGcmSetSurface` is not in the SPU writer: set the surface in the ring
before the CALL, as `hello-spu-gcm-segment` does.
