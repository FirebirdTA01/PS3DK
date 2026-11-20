# Private rolling output coordinator

`sdk/libps3dkprof/src/rolling/roll_output.{h,c}` separates accepted chunks in
memory from committed rolling-file segments. It is not connected to profiler
initialization or any public capture option. A genuine family encoder, lifecycle
handoff and reader integration remain required before enabling rolling output.

The caller supplies a fresh, exclusively owned v2 OPEN store and preallocates:
two semantic arenas, three checkpoint buffers and one pending-payload buffer.
The coordinator allocates nothing and uses no locks. Calls are serialized by one
output consumer. Arenas use offsets rather than pointers into their own storage,
because acceptance swaps their roles. Store scratch and encoder context also
remain exclusively owned and disjoint from the supplied buffers.

The additional storage is `pfo_state_bytes() + 2*arena_bytes +
3*checkpoint_capacity + payload_capacity`, excluding the existing physical
store and scratch. The runtime must account for all of it at initialization.
The tiny payload capacity used by host tests is not a runtime default: integration
must reserve room for every encoder's largest complete framed chunk.

## Admission and publication

The caller's initial arena and checkpoint describe original chunk zero. Bind
checks that the physical store is newly formatted, open and empty, and that its
configured capacities cover the supplied buffers. It performs no IO.

For each complete original framed chunk:

1. Check its sequence, length, CRC and buffer separation. Check successor chunk
   and cumulative original-byte arithmetic before invoking the encoder.
2. Ask the trusted encoder to validate the candidate against the accepted arena,
   write a separate candidate arena and encode its canonical successor checkpoint.
   The encoder must not mutate the accepted arena, perform IO, acknowledge
   producer queues, or call the coordinator recursively.
3. Refuse an invalid transition or an oversized successor checkpoint before
   rotating the physical file. The callback cannot merely copy an opaque
   checkpoint and claim semantic validity; its family implementation is a
   separate acceptance obligation.
4. If the pending payload is full, commit only its previously accepted bytes,
   with the checkpoint from **before** those bytes. A failed commit poisons the
   coordinator and does not admit the new chunk.
5. Copy the new chunk into the owned pending buffer, swap the candidate arena
   and checkpoint into accepted state, and return success. No fallible IO or
   allocation follows this acceptance boundary.

Three checkpoint buffers keep segment-start, current accepted and prepared
candidate states separate. Preparing a candidate must not overwrite either
checkpoint needed if that candidate is refused or the old segment must rotate.

Success from `pfo_accept` means ownership in preallocated RAM, not persistence.
`pfo_flush` advances the committed frontier only after the physical transaction
returns success. A crash may lose the accepted but uncommitted tail. After an IO
error even a complete alternate root might be visible to recovery; the poisoned
writer makes no further IO and never silently resumes.

## Stop and terminal behavior

A semantic resource refusal preserves the accepted arena/frontier and latches
its explicit reason. Further candidates are refused. The outer lifecycle owner,
after producer quiescence, can flush the already accepted tail and publish
STOPPED_RESOURCE without inventing source finals.

Only original wire type 6 is TRAILER. Type 7 is RETIRED and does not terminate
the capture. The trusted encoder validates actual final equations before
accepting a trailer; its result has no successor checkpoint. Later chunks are
refused. Finishing flushes the terminal bytes and publishes SEALED. The physical
marker does not prove successful close, thread join, retention eligibility or
filesystem power-loss durability.

Invalid framing can be rejected without changing the accepted state. A semantic
encoder contract violation poisons the coordinator. These are internal error
states, not authorization to skip an offending original chunk and continue an
apparently complete capture.

## Evidence boundary

The host test uses an independent small sequence/sum semantic model and the
real physical store. It checks repeated rotation, before-payload checkpoints,
RAM versus committed frontiers, short IO, slot/root write and sync errors,
resource refusal before rotation, aliases, terminal distinctions and no IO
after poison. Mutation controls require the intended assertion to fail; a
compiler error or a different assertion is not counted as a successful control.

This tests ownership and physical publication. It does not implement or validate
the full C checkpoint encoder, producer acknowledgement call sites, shutdown
races, a live snapshot protocol, or PS3 filesystem behavior.
