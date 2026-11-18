# Rolling capture runtime

The implementation uses `PS3DK_PROF_ROLLING`. The managed AUTO/FIFO archive
also enables `PS3DK_PROF_ROLLING_PUBLIC`; manual PPU/SPU selectors remain private
host-test interfaces. Internal types do not depend on those test interfaces.
The LV2 PAL still rejects test hooks; host behavior and target
compilation do not establish target runtime behavior or filesystem durability.
Append output remains the default.

The AUTO/FIFO selector connects the real producer drain to the
schema2 encoder. One tagged selection chooses PPU, PPU/SPU, or managed
AUTO/FIFO output; rejected setters preserve the previous selection. The
AUTO/FIFO profile checks the archive's actual family flags before file creation
and refuses manual GPU attachment. This host acceptance seam is not installed
as the manual PPU/SPU options API.

## Automatic option and public result

With `PS3_PROF_AUTO=ON`, set `PS3_PROF_AUTO_ROLLING=64` in CMake to request a
64 MiB file ceiling. Empty keeps append output. The accepted integer range is
24..4096 MiB. CMake adds a constant C translation unit directly to the executable;
it does not add an application constructor or require calls after `cellGcmInit`.
The AUTO constructor at priority 100 reads this object before initialization.
An invalid explicit configuration prints one `rolling configuration refused`
TTY line and skips initialization, with no append fallback.

Applications needing other finite budgets may instead leave that cache option
empty and define `ps3dk_prof_automatic_rolling_options` with
`PS3DK_PROF_DEFINE_AUTOMATIC_ROLLING_OPTIONS` from `<ps3dk/prof_rolling.h>`.
The object has external C linkage and constant storage; it belongs in a direct
executable input, since a weak reference does not extract an ordinary archive
member. Defining both forms is a link error. `ps3dk_prof_set_rolling_options`
copies options under the pre-init gate; a rejected update preserves the prior
configuration. Calling it from main is too late for automatic startup.

The public options and result structures are respectively 192 and 128 bytes on
both PPU ABIs. Size, version and reserved fields are checked. The first public
profile is managed AUTO/FIFO with no active SPU lanes; mixed active SPU/AUTO and
manual PPU/SPU remain later profiles. Manual GPU attachment, callbacks, legacy
ARC automatic capture and legacy live rolling streams are refused.

The finite AUTO preset retains six physical segments plus one replacement slot,
with 2 MiB checkpoints, 1,048,596-byte payload capacity and a 64 MiB fixed-state
ceiling. The file uses about 21 MiB; a larger ceiling alone does not enlarge its
slots or semantic pools. Three retained slots were insufficient to keep useful
timed OPS through the tested finalization sequence, so the runtime test requires
retained raw RSX records with the six-slot preset. This is not a minimum retained
time guarantee. Semantic history and metadata have independent finite budgets;
reaching one stops at the last accepted whole chunk with `PS3DK_PROF_ERESOURCE`.
Epoch and journal capacities are nonzero even without SPU lanes because RSX
DOMAIN/EPOCH records use that shared support.

`ps3dk_prof_get_rolling_result` requires size 128 and version 1 and becomes
available after the first public shutdown has joined and published finalization.
It reports accepted and committed frontiers separately, physical state and
reason, and the primary result with separate IO/close/join results. Refusals
leave the destination unchanged. A SEALED root does not override a failed close,
join or retention result. Physical sessions use the initialization timebase plus
a path/build hash to distinguish exclusive captures; this is protection against
accidental mixing, not authentication. Original HEADER/CLOCKS checks remain.

The host protocol fixture runs the native FIFO provider and report-ring command
executor: 960 draws rotate through real rolling output, while a separate
capacity case refuses an OPS chunk before queue acknowledgement or MAIN extent
retirement. After automatic profiling stop, the owner restores its physical
command-buffer end and ordinary GPU commands continue. The fixture validates
application M2MF bytes and retained C checkpoints against the original capture.
It does not establish target filesystem behavior or durability.
The automatic variant uses the real priority-100 constructor and its default
core configuration, with only the host output directory, clock and worker
barrier supplied by the test port. It retains useful raw RSX operations after
rotation; omitting the constructor fails before main can use rolling output.

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
The private PPU/SPU selector requires SPU support. The public AUTO/FIFO selector
instead requires the managed AUTO/FIFO archive and refuses active SPU lanes.
Both refuse manual GPU domains.
Callbacks, ARC output, and the current wait/function/counter configurations are
not admitted. AUTO/FIFO admission uses its explicit profile and the archive's
family configuration. Original file flags are never masked to fit an encoder.

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

Resource exhaustion records `PS3DK_PROF_ERESOURCE` (-33). It does not become a
generic sink error or manufacture a terminal TRAILER. Stopping is serviced only
after emission has returned to a consumer boundary with family locks released.
The public APP drain and the THREAD consumer observe failures at their
boundaries, without flushing a partial payload merely because a drain pass
ended. The output coordinator commits when the next complete chunk will not
fit, and finalization flushes the remaining tail. Bootstrap and best-effort
quick exit also explicitly flush. Until a commit, accepted bytes live in the
fixed RAM payload buffer; an abrupt exit can lose that tail. Thus a physical
slot represents a payload batch, not one drainer wake. The rolling SPU lane-end
inline path uses the existing public APP drain boundary; it must not leave
automatic stopping pending after an inline refusal.

### Retained support in AUTO/FIFO rolling files

The managed AUTO/FIFO output uses combined checkpoint schema 3. Build rows,
clock anchors, closed windows and FIFO accounting groups are retained by their
original chunk positions and live dependencies, rather than for the entire
process lifetime. Producer totals and protocol cursors remain cumulative.
After each accepted source chunk, the prepared checkpoint compacts support at
its own next-chunk frontier. Original records remain in the pending payload
and immutable physical slots. Latest finalized rows, incomplete builds,
protocol cursors, the open/latest closed window and eight clock predecessors
remain as dependencies; finalized history alone cannot force a slot commit.
Byte capacity determines normal commit cadence. Capacity still limits how
much live support can coexist, and separately
bounded descriptor, lane and other identity pools can still refuse work.

The clock map is explicitly **retained-support** in the CLI and viewer. It uses
the ordinary qualification rules on retained anchors plus exactly eight
predecessor anchors when enough earlier anchors exist. Retiring a regression
does not clear its sticky refusal. Lifetime anchor counts are never relabelled
as retained counts. A qualified map supplies no extrapolation: spans outside
its retained interpolation support remain UNKNOWN.

Closed-window declarations retire as a prefix, while the latest closed window
and any open window remain. A late FIFO row can therefore outlive its window's
declaration. Its original bytes, counts and sequence remain; the reader labels
its window bounds unavailable and gives it no window-specific evidential
meaning. Missing zero or future IDs still refuse, and a late row cannot name
a frame beyond the retired prefix's last frame. An ID within the retired
region is only a claim with unavailable bounds, not proof that a skipped ID
was declared. The 16-row producer queue bounds occupancy, not delay in frames:
it drops newest rows while an older row awaits acceptance.

The schema-3 shared header is 128 bytes. Besides the existing cumulative
frontiers it carries the support floor at 80, retired window count at 88,
greatest retired window ID at 96, that window's last frame at 104 and original
END position at 112; 120 remains reserved zero. Raw BEGIN/END pairs retire
together, with exact chunk/byte conservation. The AUTO checkpoint records
retired row/anchor counts and STATUS splits. The FIFO wrapper records retired
accounting groups, membership runs and their original last-use positions.
Readers replay the same per-chunk compaction and check each retained physical
successor against that state. Each slot's checkpoint remains an independent
predecessor for its original payload; advancing another slot never rewrites it.
Cold file inspection checks framing only;
it cannot establish these semantic assertions.

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

The desktop `ps3dk-prof rolling` command admits files up to 64 MiB by default
(`--max-bytes N`) and separately allows 256 MiB for the combined reader's
conservative analysis-storage accounting (`--max-analysis-bytes N`, bytes).
Dense retained AUTO rows can exceed the library's 64 MiB default analysis
allowance even in a 21 MiB physical file. The CLI and viewer can use
`rolling_file::desktop_limits()`; explicit library limits remain unchanged.
That policy also admits 2^38 conservative analysis work units, configurable
with `--max-analysis-work N`. With F retained frames, O operations, A anchors,
B build-support rows, W FIFO wraps plus FIFO boundary runs and R AUTO boundary
runs, AUTO precharges 256 x (8(F+1)(O+B+W+R+1) + 4(O+1)(A+1)
+ 64(O+F+B+A+W+R+1)) work units: every frame rescans the operations, and
every operation endpoint scans the anchors. The clock-map solver and H-SPLIT
pay their own n log n budget on top. Analysis storage is linear:
4096 x (O+F+B+A+W+1) bytes for AUTO analysis, reserved together with the
live combined state. Neither bound grows with the square of the anchor count.
These units are not CPU instructions or a time limit. Both work and storage
checks remain enforced before analysis. A schema3 read also runs a validator
replay. The replay charges the same aggregate work budget, and its live
storage counts against the same storage ceiling.
These caps do not promise a bound on total process RSS or authenticate evicted
history. Budget exhaustion refuses the listing; it does not discard rows or
relax wire validation. The analysis allowance applies to combined PPU/SPU/AUTO/
FIFO captures; PPU-only captures retain their existing reader limits.

`runtime_ppu_test.c` and `runtime_lifecycle_test.c` exercise actual FILE output,
bootstrap ordering, APP/THREAD stopping, shutdown races, write/sync/close/join
failures and quick exit. `runtime_spu_test.c` uses the real SPU producer and
drainer through the I2 host port. Its ordinary append capture is validated first;
rolling checkpoints are then compared against the actual accepted original
chunks. Tests cover eviction-crossing zones, no post-READY allocation, resource
refusal before producer acknowledgement, and inline lane-end stopping before
the first public shutdown. Mutation runners require their named semantic
witnesses; compile errors, crashes and timeouts are not successful detections.

These tests establish host behavior only. Both PPU ABIs also compile and link
the installed AUTO archive with the generated constant options. Target storage
measurements and a reviewed target run remain separate release gates; host
tests and successful links do not establish them.
