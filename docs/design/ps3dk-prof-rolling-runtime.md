# Private rolling runtime

The private implementation uses `PS3DK_PROF_ROLLING`; its configuration and
result test API still requires `PS3DK_PROF_TEST_HOOKS`. Internal types do not
depend on that test API. There is no public rolling SDK option or default
admission. The LV2 PAL still rejects test hooks; host behavior and target
compilation do not establish target runtime behavior or filesystem durability.
Append output remains the default.

The private AUTO/FIFO selector now connects the real producer drain to the
schema2 encoder. One tagged selection chooses PPU, PPU/SPU, or managed
AUTO/FIFO output; rejected setters preserve the previous selection. The
AUTO/FIFO profile checks the archive's actual family flags before file creation
and refuses manual GPU attachment. This host acceptance seam is not installed
as a public options API.

The host protocol fixture runs the native FIFO provider and report-ring command
executor: 960 draws rotate through real rolling output, while a separate
capacity case refuses an OPS chunk before queue acknowledgement or MAIN extent
retirement. After automatic profiling stop, the owner restores its physical
command-buffer end and ordinary GPU commands continue. The fixture validates
application M2MF bytes and retained C checkpoints against the original capture.
It does not establish target filesystem behavior or durability.

## Configuration and storage

`ps3dk_prof_test_rolling_set` selects the PPU/WINDOWS encoder. The separate
`ps3dk_prof_test_rolling_combined_set` selects the PPU/SPU encoder and requires
every field of `options.ppu` to be zero. Its `pfm_config` is the sole semantic
capacity source. Both setters copy their inputs before initialization, use the
ordinary configuration gate, and leave prior configuration unchanged on error.
A successful disabled setting clears the selection. Setters after successful
initialization return EALREADY.

All capacities are explicit. Before opening a file, checked planners count the
existing core allocation and the rolling context, two arenas, checkpoint
buffers, payload, physical-store scratch and control objects against one fixed
memory ceiling. This private ceiling is at most 64 MiB. It excludes allocator
overhead, the configured drainer thread stack, and separately attached producer
pools such as AUTO/FIFO and report-ring memory. It is not a total profiler or
process memory limit. The accepted-output pools do not grow after READY;
producer attachment has its own allocation and bounds. The logical file-size bound includes configuration,
roots, retained slots and the replacement slot; it is not a disk-block
reservation or a cumulative-I/O limit.

Rolling output requires a FILE sink and exclusive creation of a `.profr` file.
The combined selector requires SPU support and refuses manual GPU domains.
Callbacks, ARC output, and the current wait/function/counter configurations are
not admitted. AUTO/FIFO encoders and readers do not by themselves enable those
families in this runtime. Original file flags are never masked to fit an
encoder.

The original HEADER and CLOCKS, plus PROFILE when present, are accepted and
committed before a THREAD consumer is created or READY is published. The same
header builder serves append and rolling output. A bootstrap failure does not
publish a usable capture or leave an unpublished worker waiting for its start
gate.

## Acceptance and automatic stopping

The output coordinator accepts complete original chunks. Its success transfers
ownership into preallocated memory and permits the producer's sequence,
read-index and event-ledger acknowledgements. A committed physical frontier is
a separate fact. A failed semantic or resource check cannot acknowledge the
candidate, even if earlier chunks were accepted.

Resource exhaustion records the private typed result -33. It does not become a
generic sink error or manufacture a terminal TRAILER. Stopping is serviced only
after emission has returned to a consumer boundary with family locks released.
The public APP drain and the THREAD consumer flush at their boundaries. The
rolling SPU lane-end inline path uses the existing public APP drain boundary;
it must not leave automatic stopping pending after an inline refusal.

Automatic stopping owns a separate claim from public shutdown. It freezes the
accepted frontier, closes the file and records the result. The first subsequent
public shutdown still joins or retrieves that result and publishes immutable
finalization; later shutdown calls return EALREADY. The private result getter
returns EBUSY before this public finalization. A lane may have completed its
output-free retirement even when its final record was refused: its return value
and returned post-join observations do not imply accepted capture output.

## Terminal and failure facts

A genuine accepted TRAILER permits a SEALED physical root. Resource exhaustion
instead produces STOPPED_RESOURCE for the accepted prefix. Successful tail
commit is observed before attempting the separate terminal write, so a failed
terminal sync does not erase knowledge of the preceding acknowledged root.
Quick exit performs only a best-effort coherent OPEN flush; it does not seal or
enroll retention.

The primary result preserves this order: physical/encoder failure, close
failure, resource exhaustion, source incompleteness, join failure, then
retention failure. IO, close and join facts are also recorded separately.
Retention is eligible only after successful terminal commit, close and join.
Cold inspection counts the original 16-byte header in its metadata limit. The
PPU encoder's metadata pool excludes those bytes; the combined encoder's pool
already includes them. Enrollment passes the selected encoder's limit with
that distinction preserved. A TRAILER changes terminal state without occupying
the retained metadata pool, so exact-capacity captures can still complete.
Source completeness and qualified timing remain reader decisions; physical
SEALED alone is insufficient.

The existing live protocol uses append offsets and cannot stream reused rolling
slots. Live startup refuses a rolling capture before allocating or starting its
consumer, using the immutable capture format published at successful init.
Ordinary append captures keep their existing live behavior.

## Acceptance tests

`runtime_ppu_test.c` and `runtime_lifecycle_test.c` exercise actual FILE output,
bootstrap ordering, APP/THREAD stopping, shutdown races, write/sync/close/join
failures and quick exit. `runtime_spu_test.c` uses the real SPU producer and
drainer through the I2 host port. Its ordinary append capture is validated first;
rolling checkpoints are then compared against the actual accepted original
chunks. Tests cover eviction-crossing zones, no post-READY allocation, resource
refusal before producer acknowledgement, and inline lane-end stopping before
the first public shutdown. Mutation runners require their named semantic
witnesses; compile errors, crashes and timeouts are not successful detections.

These tests establish host behavior only. Production options, target runtime
integration, AUTO/FIFO runtime admission and target storage measurements remain
separate work before this can be advertised as rolling SDK capture support.
