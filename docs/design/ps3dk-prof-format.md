# ps3dk-prof capture format v1, with the writer contract

Normative, v1.

MUST, MUST NOT and REFUSED are normative. The verdict a reader
gives for each violation is written beside the rule:

| Verdict | Meaning |
|---|---|
| **MALFORMED** | No analysis is done. |
| **INCOMPLETE** | The structure is sound, but the capture is not certified. |
| **INVALID(thread)** | Measurement disqualification for that incarnation. |

Slice 1 covers PPU only. There are no SPU records, there is exactly one clock domain, and there is no compression.

## 1. Primitives

- **Byte order and layout:**
  - Every integer is little-endian. The PPU writer byte-swaps.
  - There is no padding and no alignment anywhere in the file.
- **Strings:**
  - A string is a `u16 len` followed by `len` bytes of UTF-8 that contain no NUL.
  - `len <= 512`. A larger value is MALFORMED.
  - Invalid UTF-8 is MALFORMED.
- **CRC32:** IEEE 802.3 (reflected polynomial 0xEDB88320, init 0xFFFFFFFF, final XOR 0xFFFFFFFF).
- **FNV-1a-64:** offset basis 0xcbf29ce484222325, prime 0x100000001b3. It is computed over the stated byte strings.
- **NONE:** written as `0` for incarnation ids and span ids. Neither id space ever uses 0 for a real value.

## 2. File structure

```
file   = magic(8) "PS3DKPRF"  u16 major=1  u16 minor=0  u32 file_flags=0  chunk*
chunk  = u32 type  u32 len  u64 chunk_seq  payload[len]  u32 crc
```

- **Header field checks:**

  | Condition | Verdict |
  |---|---|
  | magic mismatch | MALFORMED |
  | major != 1 | MALFORMED |
  | minor > 0 | Readable. The reader warns. |
  | file_flags != 0 | MALFORMED |

- **Chunk fields:**
  - `len <= 1048576`.
  - `crc` covers the bytes from `type` through the end of `payload`.
  - `chunk_seq` is 0 for the first chunk and goes up by exactly 1 for each chunk after it.
- **Chunk-level checks:**

  | Condition | Verdict |
  |---|---|
  | `len` over the cap | MALFORMED |
  | `chunk_seq` out of order | MALFORMED |
  | CRC mismatch on a chunk that is fully present | MALFORMED |
  | file ends inside a chunk, and every earlier chunk is valid | INCOMPLETE ("torn tail") |
  | unknown chunk type | MALFORMED (v1 is strict) |

- **Chunk order:**
  - HEADER must be chunk 0 and CLOCKS chunk 1, and there is exactly one of each.
  - After them come any number of DESCRIPTORS, THREADS, RECORDS and RETIRED chunks.
  - TRAILER comes last, at most once.
  - Breaking any of these rules is MALFORMED.
- **After the TRAILER:**
  - Any byte after the TRAILER chunk is MALFORMED.
  - So is a second TRAILER.
  - The writer publishes nothing after the trailer (section 7).

## 3. Chunk payloads

| type | name | payload |
|---|---|---|
| 1 | HEADER | strings: app, build_id (raw bytes, len <= 512), sdk_version, compiler_identity, flags, workload_tag; then u8 abi (1=ILP32, 2=LP64), u8 consumer_mode (1=APP, 2=THREAD), u16 reserved=0, u64 session_id, u32 ring_records, u32 max_threads (<= 1024), u32 max_descriptors (<= 65536) |
| 2 | CLOCKS | u32 count (must be 1); u32 domain (1=PPU_TB), u32 reserved=0, u64 freq_hz (> 0), u64 tb_at_init |
| 3 | DESCRIPTORS | u32 count (>= 1); per entry: u32 index, u64 id, u8 scope (1=cpu, 2=gpu-complete), u8 reserved=0, u32 line, strings name (len >= 1), logical_tag (may be empty), file |
| 4 | THREADS | u32 count (>= 1); per entry: u32 incarnation (!= 0), u32 slot (< max_threads), u64 os_thread_id, string name |
| 5 | RECORDS | u32 incarnation, u32 n (>= 1), then n x 32-byte records; `len` must equal 8 + 32n |
| 6 | TRAILER | section 6 |
| 7 | RETIRED | u32 count (>= 1); per entry: one incarnation final entry (section 6) |

Rules. Every violation in this list is MALFORMED unless it says otherwise:

- **Descriptors:**
  - `index < max_descriptors`, and each index is declared once per file.
  - Each `id` is unique per file. The writer refuses collisions (section 5.3).
  - `id` = FNV-1a-64 over (build_id bytes, 0x00, file, 0x00, line as u32 LE, name).
- **Logical key:** the pair (name, logical_tag), compared as exact bytes. Several descriptors may share a logical key;
  A/B refuses to match on a duplicated key, and that is not a format error.
- **Incarnations:**
  - Each incarnation is declared once per file and never reused within a capture.
  - At most `max_threads` incarnations are live at any point of the stream. The count goes up at THREADS and down at
    RETIRED.
  - **One live incarnation per slot.** An incarnation holds its `slot` from its THREADS entry until its RETIRED entry.
    A slot is released ONLY by that RETIRED entry; a final entry in the TRAILER releases nothing, because nothing
    follows the TRAILER. A THREADS entry that names a slot still held by a live incarnation is MALFORMED. Only after
    the holder's RETIRED entry may a new incarnation take the slot.
- **Declaration before use:** a RECORDS chunk, or a field that names an incarnation or a descriptor index, may appear
  only after the THREADS or DESCRIPTORS chunk that declares it. Section 5.4 states the drain ordering that guarantees
  this.
- **Final entries:** every incarnation gets exactly one final entry, in a RETIRED chunk or in the TRAILER, never both. A
  RECORDS chunk after its incarnation's RETIRED entry is MALFORMED. An incarnation that has no final entry when the file
  ends is INCOMPLETE.

## 4. Records (32 bytes each, inside RECORDS)

```
off 0  u8  kind     off 1  u8  depth    off 2  u16 flags    off 4  u32 a
off 8  u64 seq      off 16 u64 b        off 24 u64 c
```

| kind | name | a | b | c | flags | sequenced |
|---|---|---|---|---|---|---|
| 1 | BEGIN | desc index | tb | span id (!= 0) | 0 | yes |
| 2 | BEGIN_PARENT | token owner incarnation or 0 | parent span id or 0 | token frame id (0 if a == 0) | bit0 TOKEN (set iff a != 0) | yes |
| 3 | END | desc index | tb | span id | bit0 TRUNCATED_AT_STOP | yes |
| 4 | FRAME | 0 | tb | frame id | 0 | yes |
| 5 | GAP | 0 | first lost seq | last lost seq | 0 | **no** |
| 6 | REGERR | error (1=TABLE_FULL, 2=ID_COLLISION) | logical-key hash | descriptor id it would have had | 0 | yes |

- **Field rules (each violation is MALFORMED):**
  - Unknown kind.
  - Nonzero reserved bits or fields.
  - `depth` is nonzero on a kind other than BEGIN, BEGIN_PARENT or END.
  - **BEGIN_PARENT placement:** a BEGIN_PARENT that is not the record right after its BEGIN, in the same RECORDS
    chunk, with `seq` = BEGIN.seq + 1 and the same `depth`. A lone BEGIN or a lone BEGIN_PARENT is MALFORMED.
- **Sequenced records (rules 1 to 3 are MALFORMED if broken):**
  1. **Range:** `seq` is in 0 .. 2^63 - 1, and a value of 2^63 or more is MALFORMED.
  2. **Order:** across all of an incarnation's RECORDS chunks, in file order, seq starts at 0 and goes up by exactly
     1. The only exception is a hole covered by a GAP placed right before the next sequenced record, with
     `b` = previous seq + 1 (or 0 if there is none yet), `c` = next seq - 1, and `b <= c`.
  3. **GAP placement:** a GAP with no following sequenced record in the same chunk is MALFORMED. Two adjacent GAPs are
     MALFORMED.
  4. **Loss:** a GAP's `seq` field must equal its `b`. Any GAP makes the incarnation INVALID(thread).
- **Spans and the stack-validity interval:**
  - **Rules that are always knowable,** each MALFORMED when broken:
    - Span ids are unique among an incarnation's written BEGINs. The writer gives every BEGIN a fresh id, including
      BEGINs that are dropped (section 5.1).
    - `depth == 0` if and only if the BEGIN_PARENT `b` (the parent) is 0.
  - **The reconstructed stack:**
    - Per incarnation, the reader keeps a PARTIAL STACK of the spans it has seen begin and not end, each with its depth.
    - It also keeps a BASE state, KNOWN or UNKNOWN. It starts KNOWN with the stack empty.
    - KNOWN means the writer's stack was empty below the partial stack. UNKNOWN means records lost in a GAP may have
      left spans below it that the reader cannot see.
  - **Checks while the partial stack is not empty (either base state).** Since the last GAP no record has been lost, so
    the stack is exact from its bottom up. Each rule is MALFORMED when broken:
    - BEGIN: `depth` = the top's depth + 1, and the parent is the top's span id.
    - END: it matches the top (span id, desc index and depth), and pops it.
  - **Checks while the partial stack is empty:**

    | Record | Base KNOWN | Base UNKNOWN |
    |---|---|---|
    | BEGIN | `depth` must be 0, or MALFORMED | Any `depth`; the parent is not checked |
    | END | MALFORMED | Tolerated, and marked AFFECTED (its BEGIN was lost or came before the GAP) |
  - **GAP:**
    - Any GAP sets the base to UNKNOWN.
    - Every span on the partial stack at that point is AFFECTED, and the stack is cleared. A lost END may have closed
      it, so it is not checked again.
    - Spans begun while the base is UNKNOWN are also AFFECTED.
  - **Resynchronisation:** a BEGIN with `depth` 0 while the base is UNKNOWN and the partial stack is empty means the
    writer's stack was empty. The base becomes KNOWN, and the strict rules above apply from that record on.
  - **End of stream:** spans still on the partial stack are reported OPEN, and also AFFECTED if the base is UNKNOWN.
    That alone is not an error.
  - **Scope of AFFECTED:** the label is used only for listing, because any GAP already makes the incarnation
    INVALID(thread).
- **Frames and tokens (wire rules):**
  - **Frames:**
    - The frame owner is the incarnation that emits FRAME records.
    - Frame ids start at 0 and go up by 1 for each FRAME of that owner.
    - A hole in the frame ids is MALFORMED unless the owner had loss. A hole is allowed only when a GAP came after the
      previous FRAME, and the ids still strictly increase: a repeated or decreasing id is MALFORMED even after a GAP.
    - **A frame id is at most its own seq.** Ids 0 to f were each assigned to a FRAME together with a distinct sequence
      (sections 5.1 and 5.6), the last of them this record's own, so `f` <= the FRAME record's `seq`. A larger id is
      MALFORMED, with or without loss. It follows that no frame id reaches 2^63, and no id can repeat by overflow.
  - **Tokens:** a token (BEGIN_PARENT `a` != 0) names an owner incarnation and a frame id f.
    - The owner must have been declared earlier in the file, or the record is MALFORMED.
    - The FRAME record of (owner, f) may appear anywhere in the file, before or after the token, because rings drain
      independently. The reader therefore resolves token references only after reading the whole file (section 9).
  - `ps3dk_prof_frame()` is called on the owner thread, so an owner is one incarnation.
- **depth:** the number of spans open on that thread before this BEGIN, at most 255. A BEGIN that would be at depth 256
  is REFUSED (section 5.2).
- **tb:** the raw 64-bit timebase.
  - Within an incarnation, `tb` must not decrease in seq order.
  - A decrease is not MALFORMED. It is a CLOCK_ANOMALY: the incarnation is INVALID(thread) and the anomaly is reported.

## 5. Writer contract (normative for `libps3dkprof`)

### 5.1 Sequences, loss and refusal

These definitions apply to every case below:
- A **logical event** is BEGIN+BEGIN_PARENT (2 slots), END (1), FRAME (1) or REGERR (1). A STOP batch (section 7) is
  k ENDs.
- **Assigned:** each slot of an event that is not refused gets the next sequence of its incarnation.
  `produced_count` = the number of sequences assigned, which equals the last assigned sequence + 1.
- **Dropped:** an event that has been assigned its sequences but does not fit in the ring is DROPPED.
  - Its sequences join the incarnation's pending loss range [f, l]. They are counted in `dropped_count` and are never
    written.
- **GAP publication:** the next event that fits, together with one GAP slot, is published as GAP{f, l} followed by that
  event, with ONE release store of the write index.
  - Space is reserved for every slot of the publication (the GAP plus the event, or the whole STOP batch) before any
    slot is written.
  - The consumer therefore never sees a partial event or a GAP without its successor.
- **Dropping changes publication only, never the producer's stack:**
  - A dropped BEGIN is still pushed, and its span id is used up.
  - Later records carry the true depth and parent, which is what makes section 4's partial-stack rules exact.
  - A refused BEGIN is not pushed (sections 5.2 and 5.3).
- **Refused:** a REFUSED event is assigned NO sequence. It is not produced, it is counted in `refused_count`, and it
  makes the incarnation INVALID(thread) through the trailer.
- **Refusal causes:**
  - the depth cap;
  - the sequence limit;
  - an event after STOP that is not a STOP-batch END;
  - every zone of a descriptor whose registration failed (section 5.3).
- **Sequence boundary (pinned):**
  - Sequence 2^63 - 1 is legal.
  - A 1-slot event assigned 2^63 - 1 makes `produced_count` = 2^63, which is legal. Every later event of that
    incarnation is REFUSED with cause SEQ_LIMIT.
  - A 2-slot BEGIN whose first slot would get 2^63 - 1 (needing 2^63 for its BEGIN_PARENT) is REFUSED WHOLE. No
    sequence is assigned, `produced_count` stays 2^63 - 1, and SEQ_LIMIT is set.
  - So `produced_count` is at most 2^63, and the reader treats a larger value as MALFORMED.
  - Once SEQ_LIMIT is set, the incarnation records nothing more. Its open spans end OPEN.

### 5.2 Depth

- A BEGIN at depth 256 is REFUSED.
- Its matching END call is consumed silently: the runtime keeps a refused-nesting counter, so later ENDs still pair up.

### 5.3 Descriptors and registration errors

- **Publication:** registration writes the complete entry, then publishes the descriptor count with a release store,
  and only then returns the index to the call site. Records carry only published indices.
- **Refusals:**
  - A full table is REFUSED with TABLE_FULL.
  - An id equal to a published id of a different (file, line, name) is REFUSED with ID_COLLISION.
  - A refused zone is never merged into another descriptor.
- **What a refusal does:**
  - It increments the calling incarnation's `regerr_count` UNCONDITIONALLY. That counter lives in the incarnation's
    control block, never in the ring, so it survives even when the ring is full.
  - It then tries a REGERR event (`b` = FNV-1a-64(name, 0x00, logical_tag)), which may itself be dropped.
  - The call site caches the failure, so each later use of it REFUSES its zone (`refused_count` += 1 per BEGIN, and its
    END is consumed silently).
- **Combined failure:** table-full plus ring-full therefore still shows as `regerr_count > 0` plus `dropped_count > 0`
  in the trailer. It can never look clean.

### 5.4 Single consumer and drain ordering

- **APP mode:**
  - The DRAIN THREAD is the thread that called `ps3dk_prof_init()`, recorded at init.
  - `ps3dk_prof_drain()` and `ps3dk_prof_shutdown()` from any other thread return `PS3DK_PROF_EPERM` and do nothing.
  - A drain-busy flag is taken by compare-and-swap at entry. A reentrant call (for example from inside a sink callback)
    or an overlapping call returns `PS3DK_PROF_EBUSY` and does nothing.
- **THREAD mode:**
  - The internal drain thread is the only drainer, and an application `ps3dk_prof_drain()` returns `PS3DK_PROF_EBUSY`.
  - `ps3dk_prof_shutdown()` may be called by one thread. A once-flag taken by compare-and-swap makes any second call
    return `PS3DK_PROF_EALREADY`.
  - Shutdown hands the final drain to the drain thread and joins it.
- **Order inside one drain:**
  1. **Ring snapshot:** acquire-load every live ring's write index.
  2. **Tables:** acquire-load the descriptor count and the thread table, and emit DESCRIPTORS and THREADS for every
     entry not yet written.
  3. **Records:** emit RECORDS up to the snapshot from step 1, never splitting a published publication across chunks.
  4. **Retirement:** for each incarnation that has retired and is now fully drained, emit its RETIRED entry. Only then
     does its slot become free for a NEW incarnation id.
     - The RETIRED entry is the ONLY release of a slot, on the wire as in the runtime. The writer must not emit a
       THREADS entry for a new incarnation on that slot before the old incarnation's RETIRED entry; a reader refuses
       such a file as MALFORMED (section 3).
  - Every record visible in step 1 was published after its descriptor and its incarnation, so step 2 declares them first.
  - The FINAL drain does not take these snapshots. It uses the frozen frontier of section 7, taken once, so the final
    drain can never grow beyond it.
- **Registration after STOP:** thread registration and retirement take the thread-table mutex, a cold path. Shutdown
  sets the stop latch while holding that mutex. Every registration after that is REFUSED, so the set of incarnations is
  fixed at the latch and no slot is reused afterwards.
  - Descriptor registration is a profiler entry point, so once the latch is set it triggers the thread's STOP batch
    and is REFUSED (section 7).
- **Ring ownership:** each ring has one producer (its incarnation) and one consumer (the drainer).
- **Incarnation ids:**
  - Ids are u32, start at 1 and go up by 1 per registration.
  - When the ids are exhausted, registration is REFUSED: the thread is not registered and the global
    `unregistered_refusals` counter goes up.

### 5.5 Timestamps

- **The read:** `asm volatile("" ::: "memory")`, then the timebase read (`__builtin_ppc_get_timebase()`, or `mftb`),
  then `asm volatile("" ::: "memory")`.
- **What the barriers do and do not do:**
  - They are COMPILER barriers: the compiler may not move a memory access across the zone edge.
  - They do NOT constrain arithmetic that touches only registers. Such work can be scheduled across the edge, and if its
    result is unused it can be eliminated.
  - They are not a hardware serialization.
- **Benchmark contract:** a measured kernel must:
  - take its inputs through an opaque definition such as `asm volatile("" : "+r"(x))`;
  - consume its results inside the zone, with `asm volatile("" :: "r"(x))` or a volatile store.
- **Acceptance check:** acceptance inspects the generated code of every overhead and cross-check build. The two timebase
  reads must bracket the work, and the work must not have been removed. This is the plan's boundary-code inspection,
  extended to dependency and anti-DCE handling.

### 5.6 Frames and tokens

- **`ps3dk_prof_frame()`,** called on the owner thread, is one 1-slot event:
  - It assigns the owner's next frame id together with the event's sequence, then publishes the FRAME event or drops
    it into loss.
  - Only after that does it store the owner's `current_frame` (frame id + 1; 0 means no frame yet) with release.
  - A REFUSED FRAME (after the latch, or at the sequence limit) assigns no frame id and leaves `current_frame` unchanged.
- **`ps3dk_prof_frame_token()`,** called on the owner thread:
  - It acquire-loads `current_frame` and returns {owner incarnation, frame id}, or the NONE token before the first frame.
  - A token is a plain value, and handing it to other threads is the application's job.
- **The resulting guarantee:** a token never names an unassigned frame id. The FRAME event carrying that id was
  published or dropped before the token could exist.
  - Therefore, in a TRANSPORT COMPLETE capture where the owner has no loss and is quiesced, that FRAME record is in the
    file. The owner's ack comes after all its publications, and the frozen frontier drains everything below the ack
    (section 7).
  - A missing FRAME is therefore either loss, an incomplete capture, or impossible (section 9).

## 6. TRAILER and final entries

```
TRAILER = u32 end_flags  u32 n_final  final_entry[n_final]
          u64 sink_error_count  i32 first_sink_error  u32 reserved=0
          u64 unregistered_refusals  u64 descriptor_count
final_entry = u32 incarnation  u8 state  u8 limit_flags  u16 reserved=0
              u64 produced_count  u64 written_count  u64 dropped_count
              u64 refused_count  u64 regerr_count  u64 open_spans
```

- **UNKNOWN** is the u64 value 0xFFFFFFFFFFFFFFFF. It is never a legal count, because counts are at most 2^63.
- **The 2^63 ceiling.** 2^63 is the inclusive REPRESENTATION ceiling of the exact cumulative counters
  `produced_count`, `written_count`, `dropped_count`, `refused_count` and `regerr_count`: a value above it is
  MALFORMED. The ceiling is not a claim that every field can reach it; each field is also bound by its own
  consistency rules below. `refused_count` and `regerr_count` may sit at the ceiling, subject to those rules.
- **`open_spans` is an ACTIVE-STACK snapshot, not a cumulative counter.** It is the number of spans open on the
  producer's stack at one defined point:
  - for an ACKED entry (state 2): immediately before its STOP batch, which is the number of spans that batch closes
    with TRUNCATED_AT_STOP (section 7);
  - for a RETIRED entry (state 1): when `ps3dk_prof_thread_end()` ran, before it cleared the stack (section 7).
  - Depth is capped at 255 (section 5.2: a BEGIN at depth 256 is REFUSED and not pushed), so at most 256 spans can be
    open: `open_spans` <= 256, or MALFORMED. In state 3 it is UNKNOWN, as every non-exact count there.
  - `open_spans` is the PRODUCER's snapshot. It is distinct from the OPEN spans a reader reconstructs from the records
    (section 4), which loss can hide or leave unmatched. The two are not cross-checked in this version, with or
    without loss: a STOP batch can be dropped into loss or refused at the sequence limit, so the records alone do not
    determine the snapshot in every state.
- **end_flags:** bit0 NOT_QUIESCED, bit1 SEQ_LIMIT, bit2 SINK_ERROR_BEFORE_TRAILER. Every other bit must be 0, or the
  trailer is MALFORMED.
- **state:**

  | Value | Name | Meaning |
  |---|---|---|
  | 1 | RETIRED | The thread retired. |
  | 2 | ACKED | The thread acknowledged STOP. |
  | 3 | NOT_QUIESCED | The thread did neither by the frontier. |

  RETIRED entries (chunk type 7) always have state 1.
- **limit_flags:** bit0 SEQ_LIMIT, bit1 COUNTS_UNKNOWN. Every other bit must be 0.
- **Where final values come from:**
  - **States 1 and 2:**
    - The producer's last counter change happens before the release store of its retire or ack word.
    - The drainer reads the counters only after an acquire load has seen that store. Nothing changes afterwards
      (section 7), so every field is exact and final.
    - UNKNOWN must not appear, and `limit_flags` bit1 = 0.
  - **State 3:** the producer may still be running, so nothing in its control block is final. The entry carries:
    - `written_count`: exact, the number of its sequenced records in the file, which the frozen frontier fixes;
    - `produced_count`, `dropped_count`, `refused_count`, `regerr_count` and `open_spans`: UNKNOWN;
    - `limit_flags` = 0x02 (COUNTS_UNKNOWN, with SEQ_LIMIT not asserted).
    Any other value in these fields is MALFORMED.
  - **`descriptor_count`:** the number of descriptors declared in the file, which the frozen frontier fixes. It is not
    the live table count.
  - **`unregistered_refusals`:** one atomic read taken at the frontier. Unregistered threads are never quiesced, so the
    value is informational and is excluded from the consistency checks.
- **Consistency checks (each violation is MALFORMED):**
  - **Every entry:** `written_count` = the number of sequenced records of that incarnation in the file.
  - **State 1 and 2 entries only:**
    - `produced_count` = `written_count` + `dropped_count`, and `produced_count` <= 2^63.
    - `dropped_count` = (the sum of that incarnation's GAP ranges) + the terminal tail. The terminal tail is
      `produced_count` - (last written seq + 1), or `produced_count` if nothing was written.
    - A terminal tail > 0 means the last events were lost with no GAP to follow them. That is terminal loss.
    - `regerr_count` >= the number of REGERR records written for that incarnation. The counter is incremented
      unconditionally and the REGERR record is best effort (section 5.3), so the counter covers every record; a
      larger counter is legal, because REGERR records can be lost.
    - `open_spans` <= 256.
  - **State 3 entries:** no arithmetic check. Loss and terminal loss for them are UNKNOWN, and so are `regerr_count`
    and `open_spans`: neither is checked.
  - **`descriptor_count`:** equals the number of descriptors declared.
  - **end_flags bit0:** set if and only if some entry has state 3.
  - **end_flags bit1:** set if and only if some final entry has `limit_flags` bit0. "Some final entry" covers every RETIRED
    chunk entry as well as the TRAILER's entries, so an earlier retired thread that hit its limit still sets it.
  - **end_flags bit2:** set if and only if `sink_error_count` > 0.
- **Empty versus lost (states 1 and 2):**
  - Empty: `produced_count` = 0 with nothing written.
  - Lost first-and-only record: `produced_count` = 1, `written_count` = 0, `dropped_count` = 1. That is terminal loss,
    never "empty".

## 7. Lifecycle: stop latch, quiescence, the frozen frontier, sealing

- **One capture per process.**
  - `ps3dk_prof_init()` succeeds at most once per process.
  - Every later call returns `PS3DK_PROF_EALREADY`: before or after shutdown, quiesced or not. There is no restart.
- **The stop latch** is separate from the phase:
  - It is one word, set once by shutdown with release (under the thread-table mutex, section 5.4) and never cleared.
  - Producers test the latch with acquire, never the phase. So a thread that next calls in any later phase (STOPPING,
    SEALING or SEALED) still sees STOP and acknowledges.
- **The phase** goes RUNNING, STOPPING, SEALING, SEALED, forward only. It is used by the drainer and the API, and
  producers never read it.
- **Shutdown entry checks,** made in this order and before ANY state change, lock or join:
  1. **Inside a drain:** the calling thread is inside a drain. The drainer sets a thread-local in-drain flag for its
     whole run, sink calls included, so this covers a sink callback in either mode. The call returns
     `PS3DK_PROF_EBUSY` and changes nothing. It can therefore never self-join or wait for a drain lock it already
     holds.
  2. **APP mode, wrong thread:** the caller is not the designated drain thread. The call returns `PS3DK_PROF_EPERM`.
  3. **Second call:** the shutdown once-flag (compare-and-swap) is already taken. The call returns `PS3DK_PROF_EALREADY`.
- **The STOP batch:**
  - At its first profiler entry point after it observes the latch, a producer emits ONE STOP batch. The batch closes
    every span still open on its stack, innermost first, with END + TRUNCATED_AT_STOP and `tb` = now.
  - This includes the span whose END call observed the latch. A span counts as completed only if its END was emitted
    before the latch was observed.
  - The batch is reserved and published as a whole, or dropped into loss.
- **The ack is the LAST write.** The producer completes the following steps, in this order:
  1. publish the STOP batch, or drop it into loss;
  2. update its counters;
  3. clear its stack;
  4. set its thread-local "acked" flag;
  5. only then store its ack word with release.
  - Nothing the incarnation owns changes after that store: not its ring, its control block, its counters or its stack.
  - Its later profiler calls only READ the latch and its own thread-local flag, and return REFUSED silently.
  - A reader that has acquire-loaded the ack therefore sees final values, with no race against a later clear.
- **Retire follows the same rule.** `ps3dk_prof_thread_end()`, in this order:
  1. publish any final event;
  2. update the counters;
  3. clear the stack;
  4. set the thread-local "retired" flag;
  5. store the retired word with release, as its last write.
  - Later calls from that thread are REFUSED silently. The exception is `ps3dk_prof_thread_begin()` before the latch,
    which registers a NEW incarnation.
  - A refused call from a retired thread reads and writes ONLY its thread-local state. It never goes through its old
    slot or control-block pointer. That slot may already belong to a new incarnation (section 5.4 frees a slot for
    reuse once the retired incarnation is drained), and the retired thread must not touch it.
  - A thread that retires after the latch also counts as quiesced.
- **Self-quiescence:**
  - Shutdown first runs the CALLING thread's own STOP batch and ack, if that thread is a registered incarnation. The
    caller never waits for itself.
  - **APP mode:** the caller is the designated drain thread. It then runs the quiescence wait and the sealing itself.
  - **THREAD mode:**
    - The caller wakes the drain thread and joins it.
    - At its next step the drain thread sees the latch. It turns off self-instrumentation, runs its own STOP batch and
      ack if it is registered, then runs the quiescence wait and the sealing.
    - The drain thread never joins anything.
- **Quiescence wait:**
  - It is bounded by `cfg.quiesce_timeout_us`.
  - It ends when every incarnation registered before the latch has acked or retired, or at the timeout.
- **The frozen frontier,** taken exactly ONCE when the wait ends, in this order:
  1. For each incarnation, acquire-load its ack/retire word and fix its state: ACKED, RETIRED or NOT_QUIESCED.
  2. For each incarnation's ring, acquire-load its write index, giving F[i].
  3. Acquire-load the descriptor count, giving Fd. The thread count Ft was already fixed by the latch.
  4. Read the counters of the ACKED and RETIRED incarnations. They are final (section 6).
  - Step 1 comes before step 2, so an acked thread's STOP batch is below its F[i].
  - Step 2 comes before step 3, so every descriptor that a record below F[i] references is below Fd.
  - The final drain emits exactly the records below each F[i], the descriptors below Fd and the incarnations below Ft.
    It takes no further snapshot.
- **Sealing:**
  - The final drain runs, then the TRAILER is written, then flush, then close, and the phase becomes SEALED.
  - Self-instrumentation is off from the frontier onwards.
  - The capture is exactly the bytes through the TRAILER. A NOT_QUIESCED producer can still publish into its ring
    after the frontier, but that data is never drained and is not part of the sealed capture.
- **Storage lifetime: process lifetime, with no release API in slice 1.**
  - What is allocated once by `ps3dk_prof_init()` and never freed or reused for the life of the process:
    - the rings, control blocks, descriptor table and thread table;
    - everything that any thread-local profiler pointer can reach.
  - Consequences:
    - every entry point stays memory-safe in every phase, including after SEALED, from acked, retired, NOT_QUIESCED and
      unregistered threads alike;
    - the hot path needs no in-flight accounting;
    - the drainer, sink callbacks and late producers can never meet freed memory.
  - The header declares no release function, and the library exports none (checked with `nm`).
  - Freeing is DEFERRED to a later design. Before freeing anything, that design must specify:
    - drainer and sealing completion;
    - an in-flight entry/exit protocol showing that no user remains;
    - refusal of release from inside a drain or a sink callback;
    - a permanent released guard that every entry point checks before it touches any thread-local or control pointer;
    - tests for release racing the final drain, and for calls after a successful release.
  - "Never freed" is about MEMORY. It does not forbid reusing a SLOT before STOP.
    - A retired, fully drained incarnation's slot may be handed to a new incarnation (section 5.4). The memory stays
      allocated; only its owner changes, under the thread-table mutex, with a new incarnation id.
    - Slots are not reused after the latch, and init stays EALREADY (see above).

## 8. Sink semantics and the shutdown result

- **File sink:**
  - The file is opened exclusive-create. If it already exists, init fails with `PS3DK_PROF_EEXIST` and nothing is
    written.
  - A short write is retried for the remainder, up to 16 tries with no progress in a row. EINTR-class errors are retried.
  - Any other error, or running out of retries, is a SINK FAILURE.
- **Callback sink:** the callback returns the bytes accepted (0..n) or a negative error.
  - A short count is retried like a short write.
  - A negative value is a SINK FAILURE.
  - The callback runs inside a drain. A `ps3dk_prof_drain()` from it gets EBUSY (section 5.4), and so does a
    `ps3dk_prof_shutdown()` (section 7), with no state change.
- **After a sink failure:**
  - Nothing more is written, and the first error is kept.
  - The trailer is attempted only if the sink has not failed. A capture whose trailer could not be written is
    INCOMPLETE.
  - `sink_error_count` covers errors recovered before the trailer (bit2).
  - "Recovered" means the retry then wrote every byte. A sink error never drops bytes while writing continues: an error
    that is not recovered is a SINK FAILURE, which stops all writing and leaves no trailer. Bit2 alone therefore never
    explains a missing record.
- **Shutdown result:** `ps3dk_prof_shutdown()` returns the most severe outcome. The trailer records every condition
  that applies. In order of severity:
  1. a sink failure, where the trailer is missing;
  2. a close failure, after the trailer was written;
  3. `PS3DK_PROF_ENOTQUIESCED` (end_flags bit0);
  4. `PS3DK_PROF_ESEQLIMIT` (end_flags bit1);
  5. `PS3DK_PROF_ESINKRECOVERED` (end_flags bit2 without a failure);
  6. OK.
  - The result is OK exactly when the reader would give TRANSPORT COMPLETE (section 9) and close succeeded.
  - A close failure FAILS the run even though the trailer bytes exist.
  - The harness takes the shutdown result from the program's output. The file alone never certifies a run.

## 9. Reader verdicts

- **MALFORMED:** any rule marked MALFORMED. Nothing is analysed.
- **INCOMPLETE** (the structure is sound, and the readable prefix may be shown, labelled as INCOMPLETE):
  - the TRAILER is missing;
  - the tail is torn;
  - an incarnation has no final entry;
  - `end_flags` bit0 (NOT_QUIESCED), bit1 (SEQ_LIMIT) or bit2 (SINK_ERROR) is set.
- **TRANSPORT COMPLETE:** none of the above. The harness additionally requires an OK shutdown result, which by
  section 8 corresponds one-to-one.
- **MEASUREMENT VALID, per incarnation.** The capture must be TRANSPORT COMPLETE, and the incarnation must have:
  - state 1 or 2;
  - `dropped_count` = 0, `refused_count` = 0 and `regerr_count` = 0;
  - no SEQ_LIMIT and no CLOCK_ANOMALY.
- **Use of validity:** A/B, population statistics, frame elapsed and attributed work use only valid incarnations, and the output states
  that scope.
- **Censored spans.** OPEN spans and spans flagged TRUNCATED_AT_STOP are CENSORED observations: the true duration is at
  least the recorded part, and is not known. For a zone (a logical key within the valid incarnations) with any
  censored instance:
  - The viewer may show descriptive values over its COMPLETED instances only, labelled "completed instances only:
    k censored of n". They are never presented as the zone's population or its distribution.
  - A/B: a zone with any censored instance in either capture is NOT compared as clean. The comparison is refused with
    reason CENSORED, and both captures' counts are shown.
  - Zones with no censored instance are reported normally.
  - Censoring alone does not make an incarnation invalid.
- **Frame attribution of a span.** Attribution always goes by the frame the span STARTED in.
  - **"spans N frames"** is reported only where it is knowable:
    - For an un-tokened span on its owner, N = 1 + the number of the owner's FRAME records between the span's BEGIN and
      END in the owner's own sequence order.
    - For a tokened span on another incarnation, N is reported as UNKNOWN. A token names only the start frame, and the
      reader never infers cross-thread ordering.
    - After loss on the owner, N is UNKNOWN as well.
  - **Tokened span:** the token's (owner, frame). Never by time.
  - **Un-tokened span on an owner incarnation** (one that emits FRAME records): the owner frame that is current at its
    BEGIN in the owner's own sequence order, meaning the last FRAME record with a lower seq. If there is no earlier
    FRAME, the frame is UNKNOWN(pre-frame).
  - **After a GAP on the owner:** the frame is UNKNOWN(loss) until the next FRAME record, which carries its own id. A
    FRAME may have been lost in the GAP.
  - **Un-tokened span on any other incarnation:** UNKNOWN. Never inferred from timestamps.
- **Token resolution (ordered and exhaustive).** It is done after the whole file has been read. Each token
  reference (O, f) takes the outcome of the FIRST rule that matches, so every reference gets exactly one:

  | Order | Rule | Outcome |
  |---|---|---|
  | 1 | O has a FRAME record with id f. This holds in any capture, including an INCOMPLETE one. | RESOLVED |
  | 2 | The file can be missing O's records: there is no TRAILER, the tail is torn, or O has no final entry. | UNRESOLVED-INCOMPLETE |
  | 3 | O's own stream can be missing records: O has a GAP or terminal loss (dropped_count > 0), or O's final entry is state 3 (counts UNKNOWN). | UNRESOLVED-LOSS |
  | 4 | Anything else. | IMPOSSIBLE: the capture is MALFORMED |

  - **Why rule 4 is right in every remaining case.** O's final entry is state 1 or 2 with dropped_count = 0, a TRAILER is
    present, and the tail is intact.
    - By section 5.6, FRAME f was assigned before the token existed and was not dropped, so it was published. O's ack or
      retire follows every publication (section 7), so the record lies below O's frontier, and the trailer shows that
      the final drain wrote that frontier.
  - **The other INCOMPLETE reasons do not change rule 4:**
    - SEQ_LIMIT (bit1), on O or on another incarnation: a refused FRAME assigns no id;
    - a recovered sink error (bit2), which loses no bytes (section 8);
    - NOT_QUIESCED (bit0) of an incarnation OTHER than O: O's stream is still complete.
    In all three cases a missing reference is IMPOSSIBLE, not tolerated.
  - An unresolved reference keeps its (O, f) claim for listing, but contributes to no frame value.
- **Frame values (frame elapsed and per-lane attributed work; nothing is a cross-lane frame total):**
  - **Frame elapsed of (O, k):** the time from FRAME k to FRAME k + 1 of O, with both records present.
    - It is reported only if O is MEASUREMENT VALID.
    - The last, unclosed frame is censored and is listed as such.
  - **Exclusive duration of a span:** its inclusive duration (END tb - BEGIN tb) minus the inclusive durations of
    its DIRECT children on the same incarnation. Within one incarnation the exclusive durations of all its spans never
    overlap, because nested spans partition their parent.
  - **Attributed work W(O, k, i), per incarnation:** the sum of the EXCLUSIVE durations of the spans of
    incarnation i that are attributed to (O, k).
    - It is reported for each incarnation separately. It is never summed across incarnations, never added to frame
      elapsed, and never called a "frame total".
    - Parallel lanes are shown side by side, so overlapping workers are never combined.
    - A child attributed to a different frame from its parent counts in the child's frame. The parent's exclusive
      duration already excludes it, so nothing is counted twice.
    - W(O, k, i) is reported only if both O and i are MEASUREMENT VALID and no span of i attributed to (O, k) is
      censored or has an unresolved token. Otherwise it is refused with the reason named (owner invalid, lane
      invalid, censored or unresolved).
    - Validity is judged per lane. An invalid third lane refuses only its own W values; a valid lane's W stands.
  - **A clean worker whose tokens name an invalid owner** keeps its own zone statistics, which do not depend on the
    owner. Its frame attribution is shown as "owner invalid", and its W for that owner is refused.
- **AFFECTED spans** (section 4) are listed only. Their incarnation is already invalid.

## 10. Test obligations added by this spec (assigned to plan stages)

- **Stage 2, reader:**
  - every MALFORMED rule, one fixture each;
  - the torn tail, giving INCOMPLETE;
  - BEGIN_PARENT split across chunks;
  - an adjacent GAP pair, and a GAP at the end of a chunk;
  - `produced_count` = 2^63 accepted and 2^63 + 1 refused;
  - a 2-slot BEGIN at 2^63 - 1 refused, with `produced_count` at 2^63 - 1;
  - a REGERR dropped while `regerr_count > 0`, which must give INVALID and must not look clean;
  - consistency arithmetic, including terminal tail = 1 with `written_count` = 0;
  - RETIRED-then-RECORDS;
  - a decreasing `tb`, which gives CLOCK_ANOMALY;
  - a frame-id hole with and without loss.
  - **Ancestry under loss:**
    - a parent BEGIN dropped in a GAP, followed by the child's BEGIN, the child's END and the parent's END. It is
      accepted, with the thread INVALID, the spans AFFECTED and no MALFORMED.
    - the partial-stack checks while the base is UNKNOWN: a wrong child depth or parent above a seen span is
      MALFORMED;
    - a depth-0 resync, after which a wrong parent IS MALFORMED;
    - a depth-0 BEGIN while the partial stack is non-empty, MALFORMED;
    - `depth`/parent zero mismatch, MALFORMED in either base state;
    - an END on an empty stack: MALFORMED with base KNOWN, AFFECTED with base UNKNOWN.
  - **Final entries:**
    - a state-3 entry with exactly the UNKNOWN fields and `limit_flags` 0x02, accepted as INCOMPLETE;
    - a state-3 entry with a concrete `produced_count`, MALFORMED;
    - a state-1 or state-2 entry containing UNKNOWN, MALFORMED;
    - a SEQ_LIMIT only in an earlier RETIRED entry: bit1 required, giving INCOMPLETE; bit1 missing, MALFORMED; bit1 with
      no limited entry, MALFORMED.
  - **Censoring:**
    - a zone with one truncated instance: the descriptive output is labelled "completed instances only", and A/B is
      refused CENSORED;
    - a clean zone in the same capture compares normally;
    - the unclosed last frame is censored.
  - **Frames and tokens:**
    - a future-arriving valid frame: the worker's tokened RECORDS chunk comes before the owner's FRAME chunk. It gives
      RESOLVED and no error.
    - a nonexistent frame in a complete capture with a lossless, quiesced owner: IMPOSSIBLE, so MALFORMED;
    - the same reference with owner loss: UNRESOLVED-LOSS. The owner's frame elapsed and every W for that owner are refused, and the worker's zone
      statistics are still reported.
    - a clean worker with an invalid owner: the worker's zone statistics are reported, its attribution shows "owner
      invalid", and its W for that owner is refused;
    - an owner's un-tokened span before its first FRAME gives UNKNOWN(pre-frame), and one after a GAP but before the
      next FRAME gives UNKNOWN(loss).
  - **Token resolution, one fixture per rule and per INCOMPLETE reason.** The missing reference always has a
    lossless ACKED owner unless stated:
    - present FRAME in an INCOMPLETE (no-trailer) capture: RESOLVED (rule 1);
    - missing, no TRAILER: UNRESOLVED-INCOMPLETE; missing, torn tail: UNRESOLVED-INCOMPLETE; missing, O has no final
      entry: UNRESOLVED-INCOMPLETE (rule 2);
    - missing, O has a GAP: UNRESOLVED-LOSS; missing, O has terminal loss only: UNRESOLVED-LOSS; missing, O is
      state 3: UNRESOLVED-LOSS (rule 3);
    - missing, capture INCOMPLETE ONLY through SEQ_LIMIT (bit1, on another lane, and on O itself): MALFORMED;
    - missing, ONLY through a recovered sink error (bit2): MALFORMED;
    - missing, ONLY through another lane's NOT_QUIESCED (bit0): MALFORMED;
    - missing, complete lossless capture: MALFORMED (rule 4).
  - **Attributed work:**
    - nested parent and child on one lane: their exclusive durations sum to the parent's inclusive duration, and the
      parent is not counted twice;
    - a child in a later frame than its parent: the child's time is in the child's frame, and the parent's W excludes
      it;
    - two overlapping workers tokened to the same frame: two separate W values, with no cross-lane sum and nothing
      labelled "frame total";
    - a clean owner and a clean worker plus a third, invalid lane: frame elapsed is reported, the clean worker's W is
      reported, and only the invalid lane's W is refused.
  - **"spans N frames":**
    - an owner span crossing two FRAMEs gives "spans 3 frames";
    - a tokened worker span gives "spans N: unknown";
    - an owner span with a GAP inside it gives N unknown.
- **Stage 3, runtime on the host build:**
  - APP mode: drain from a non-designated thread returns EPERM, and reentrant and concurrent drains return EBUSY;
  - THREAD mode: a second shutdown returns EALREADY;
  - the STOP batch, where the END that observes the latch is flagged truncated;
  - after an ack, nothing is published and no counter changes;
  - table-full plus ring-full;
  - the sequence-limit pair (a 1-slot event at 2^63 - 1, then a refusal; a 2-slot event refused whole);
  - the descriptor and incarnation drain ordering, with an adversarial interleaving forced by test hooks;
  - the short-write retry limit, a negative callback return, a close failure after the trailer.
  - **Lifecycle:**
    - a shutdown caller with open spans self-quiesces: its truncated ENDs are present and there is no timeout;
    - the THREAD-mode drain thread self-acks;
    - shutdown from inside a sink callback returns EBUSY with no state change, in both modes. A later proper shutdown
      still works.
    - init after the first success returns EALREADY, before and after shutdown;
    - registration after the latch is REFUSED;
    - a quiescence timeout with a producer forced to publish after the frontier: that data is absent from the file;
      the state-3 entry has UNKNOWN fields; Fd and Ft do not grow;
    - the shutdown result severity order, including ESEQLIMIT from a thread that retired before shutdown.
  - **Ordering and lifetime:**
    - **Pause at ack publication:** a test hook stalls a producer just before its ack store. The frontier must not
      classify the producer as ACKED. When the producer is released, a reader that has acquire-loaded the ack sees
      final counters, an empty stack and the published batch. With a timeout during the stall, the producer is
      NOT_QUIESCED and the entry is UNKNOWN.
    - **Pause at retire publication:** the same checks for the retired word.
    - **Calls after SEALED.** These run on the host build under ASan and TSan, from acked, retired,
      NOT_QUIESCED and unregistered threads:
      - every call returns safely, and the SEALED FILE bytes never change (they are hashed before and after);
      - a NOT_QUIESCED producer may still make exactly the specified late changes, its STOP batch into its own ring and
        then its ack. After its ack it makes no further change;
      - acked, retired and unregistered threads make no change to any shared state.
    - **Pre-STOP slot reuse:**
      - thread A retires and is drained, and its slot goes to a new incarnation B;
      - A then makes zone calls: they are refused and touch only A's thread-local state;
      - B's ring, counters and control block are unchanged by A's calls (checked under TSan);
      - A's `thread_begin` before the latch gets a NEW incarnation id.
    - **No release symbol:** `nm` of both ABI libraries shows no release symbol.
    - **Frame tokens:** a token taken before the first frame is NONE; a refused FRAME leaves `current_frame` unchanged;
      a dropped FRAME still assigns its id, so a later token resolves as UNRESOLVED-LOSS.
- **Stage 5, overhead and cross-check:** the barrier and anti-DCE inspection of section 5.5.
