# Private PPU checkpoint encoder

`sdk/libps3dkprof/src/rolling/roll_ppu.{h,c}` implements the PPU/WINDOWS
accepted-state encoder used by the private rolling-output coordinator. It is
not linked into the SDK runtime and does not enable a capture-size option.
Combined SPU/AUTO/FIFO support and the runtime lifecycle remain separate work.

## Admission and state ownership

`pfp_plan` checks the complete arena layout before allocation. Its result gives
the context size, arena size, required alignment and worst-case encoded
checkpoint size. The caller allocates the immutable context, two arenas and
checkpoint storage before admitting data. The encoder allocates nothing,
performs no IO, takes no locks and never acknowledges a producer queue.

`pfp_init` accepts the genuine original 16-byte file header. It creates the
canonical chunk-zero checkpoint; it does not fabricate HEADER or CLOCKS
chunks. Original capability flags must be zero or WINDOWS (512).

`pfp_prepare` takes one complete original framed chunk and an immutable
accepted arena. It validates the framing, original sequence and source
semantics, applies the transition only to the inactive arena, and encodes the
canonical PFRCKP01 successor. A failure leaves the context, accepted arena and
input unchanged; the inactive arena and scratch may be dirty. Arena references
are offsets, so moving a successful arena does not invalidate its state.

The callback distinguishes semantic/unsupported input from capacity refusal.
A real TRAILER validates final equations and returns a terminal result with no
successor checkpoint. Terminal input does not invent missing final entries,
erase loss or turn an incomplete source into a complete capture.

## Bounded source contract

The encoder supports declarations, ordinary records, producer GAP/REGERR,
retirement and slot reuse, all original final-state forms, explicit frame
tokens and WINDOWS. Counter arithmetic preserves the inclusive 2^63 exact
count ceiling and the separate sequence ceiling. Window COUNT arithmetic uses
checked inclusive spans, including UINT64_MAX endpoints.

The rolling PPU profile requires each BEGIN span ID to be nonzero, below
2^63, and greater than the prior maximum for its incarnation. The ordinary
legacy reader allows some unique nonmonotone IDs; those inputs are explicitly
outside this bounded profile. END records after an unknown stack base retain
the ordinary reader's rules, including its permitted high span IDs. REGERR
payload fields do not acquire invented restrictions.

Closed spans are removed from the active stack pool. Tokens from closed or
abandoned spans remain until their actual owner/frame membership is known.
Frame membership is stored as exact runs, never a high-water approximation.
The shared identity limit is `frame runs + unresolved tokens` at the end of
each whole candidate chunk. The independently preallocated run and token
pools also enforce their own transient limits: a chunk may briefly contain a
run and a token which resolve together, without exceeding the final shared
limit. Both pools are charged in the arena layout.

Lifetime declarations, window metadata, frame holes and unresolved tokens can
exhaust fixed limits. The encoder refuses before accepting that chunk. The
coordinator and lifecycle owner must then preserve accepted output and report
the resource stop; this module cannot close a file or stop producer threads.

Each accepted-state arena contains a fixed header and bounded arrays for
incarnations, descriptors, stack witnesses, frame runs and tokens, plus the
metadata byte budget. `pfp_plan` includes alignment and checks every product
and sum. The encoded bound is:

```
132 + metadata_bytes + 120*incarnations + 80*stack_rows
    + 16*frame_identities
```

The in-memory pools are distinct from that wire bound. Runtime allocation must
also include the second arena, immutable context, coordinator, physical-store
scratch, checkpoint buffers and pending payload. No default attach footprint
or runtime memory option is established by this private component.

GCC 16.1.0 target layout measurements for this implementation are:

| Storage | PPU ILP32 bytes | PPU LP64 bytes |
|---|---:|---:|
| Immutable context | 72 | 88 |
| Arena header | 152 | 152 |
| Incarnation | 144 | 144 |
| Descriptor | 16 | 16 |
| Stack witness | 88 | 88 |
| Frame-run node | 24 | 24 |
| Unresolved token | 16 | 16 |

For these ABIs one arena is therefore
`align8(152 + 144*I + 16*D + 88*S + 40*F + M)`, with the capacity symbols
matching the planner fields above. The public planner result remains the
allocation authority; callers must not duplicate this measured formula.

## Verification

`run_ppu_encoder.py` builds the private API tests and a C fixture that consumes
only original source bytes. The fixture repeatedly moves successful arenas,
poisons their previous locations, starts scratch with different patterns and
checks context/input/accepted-state immutability. It does not receive Rust
checkpoints or a serialized Rust state.

`rolling_ppu_c_general` first validates independent original sources and
computes their direct-prefix checkpoints. It then compares every emitted C
checkpoint byte for byte and verifies retained suffix analysis, including
crossing spans, exact counters, frame membership, incomplete finals and
capacity boundaries.

`rolling_ppu_c_rejections` changes individual payload bytes and repairs the
original checksum. It requires rejection of each input the ordinary reader
rejects. Inputs admitted by the selected rolling profile must also be accepted
by C and produce the same checkpoint at every cut. The two deliberate BEGIN-ID
restrictions are checked separately against the rolling reader, with their
exact error rules; no arbitrary reader failure is treated as an exemption.
Failed cases retain their original input, producer output and checkpoints.

Run the ignored Rust integration tests with
`PS3DK_PROF_ROLLING_PPU_ENCODER` pointing at the helper built by the Python
runner. CI runs them explicitly, including ASan/UBSan controls. Cross-compiling
the private encoder for both PPU ABIs verifies target compilation; it does not
establish runtime, filesystem or console behavior.
