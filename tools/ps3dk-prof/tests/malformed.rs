//! Section 10, "every MALFORMED rule, one fixture each": one test per rule of sections 1-6 and token rule 4 of
//! section 9.  Each fixture breaks exactly one rule of an otherwise valid capture.  tests/SPEC-COVERAGE.md maps every
//! test to its rule.

mod common;

use common::*;
use ps3dk_prof::{Incomplete, Rule};

/// A valid one-lane capture: one completed zone.
fn ok_lane() -> Lane {
    lane(1, seqd(0, [zone(0, 0, 0, 100, 1, 0), vec![end(0, 0, 0, 200, 1)]].concat()))
}

fn ok_file() -> File {
    scenario(vec![ok_lane()]).file()
}

fn with_header(h: Hdr) -> Vec<u8> {
    let mut f = ok_file();
    f.chunks[0] = chunk(T_HEADER, h.payload());
    f.bytes()
}

fn with_descs(ds: Vec<Desc>) -> Vec<u8> {
    let mut s = scenario(vec![ok_lane()]);
    s.descs = ds;
    s.bytes()
}

fn with_threads_payload(p: Vec<u8>) -> Vec<u8> {
    let mut f = ok_file();
    let i = f.find(T_THREADS);
    f.chunks[i] = chunk(T_THREADS, p);
    f.bytes()
}

fn with_recs(recs: Vec<Rec>) -> Vec<u8> {
    scenario(vec![lane(1, recs)]).bytes()
}

fn with_trailer(edit: impl FnOnce(&mut Trl)) -> Vec<u8> {
    let s = scenario(vec![ok_lane()]);
    let mut t = s.trailer_struct();
    edit(&mut t);
    let mut f = s.file();
    let i = f.find(T_TRAILER);
    f.chunks[i] = chunk(T_TRAILER, t.payload());
    f.bytes()
}

#[test]
fn baseline_is_complete() {
    complete(&ok_file().bytes());
}

// ---- 1. Primitives ----------------------------------------------------------------------------------------------

#[test]
fn string_too_long() {
    assert_rule(&with_header(Hdr { app: vec![b'a'; 513], ..Hdr::default() }), Rule::StringTooLong);
}

#[test]
fn string_invalid_utf8() {
    assert_rule(&with_header(Hdr { app: vec![0xff, 0xfe], ..Hdr::default() }), Rule::StringInvalidUtf8);
}

#[test]
fn string_contains_nul() {
    assert_rule(&with_header(Hdr { app: b"a\0b".to_vec(), ..Hdr::default() }), Rule::StringContainsNul);
}

// ---- 2. File structure ------------------------------------------------------------------------------------------

#[test]
fn magic_mismatch() {
    let mut f = ok_file();
    f.magic = *b"PS3DKPRX";
    assert_rule(&f.bytes(), Rule::MagicMismatch);
}

#[test]
fn major_version() {
    let mut f = ok_file();
    f.major = 2;
    assert_rule(&f.bytes(), Rule::MajorVersion);
}

#[test]
fn minor_version_is_readable_with_a_warning() {
    let mut f = ok_file();
    f.minor = 1;
    let a = complete(&f.bytes());
    assert!(a.capture.minor_warning);
}

#[test]
fn file_flags_nonzero() {
    let mut f = ok_file();
    f.flags = 1;
    assert_rule(&f.bytes(), Rule::FileFlagsNonzero);
}

#[test]
fn chunk_len_over_cap() {
    let mut f = ok_file();
    f.len_override = Some((2, 1_048_577));
    assert_rule(&f.bytes(), Rule::ChunkLenOverCap);
}

#[test]
fn chunk_seq_out_of_order() {
    let mut f = ok_file();
    f.seq_override = Some((1, 5));
    assert_rule(&f.bytes(), Rule::ChunkSeqOrder);
}

#[test]
fn chunk_crc_mismatch() {
    let mut f = ok_file();
    f.bad_crc = Some(2);
    assert_rule(&f.bytes(), Rule::ChunkCrc);
}

#[test]
fn unknown_chunk_type() {
    let mut f = ok_file();
    f.chunks.insert(3, chunk(9, Vec::new()));
    assert_rule(&f.bytes(), Rule::UnknownChunkType);
}

#[test]
fn header_not_chunk_0() {
    let mut f = ok_file();
    f.chunks.swap(0, 1);
    assert_rule(&f.bytes(), Rule::HeaderNotChunk0);
}

#[test]
fn clocks_not_chunk_1() {
    let mut f = ok_file();
    f.chunks.remove(1);
    assert_rule(&f.bytes(), Rule::ClocksNotChunk1);
}

#[test]
fn duplicate_header_or_clocks() {
    let mut f = ok_file();
    let clocks = f.chunks[1].clone();
    f.chunks.insert(3, clocks);
    assert_rule(&f.bytes(), Rule::DuplicateHeaderOrClocks);
}

#[test]
fn trailing_bytes_after_trailer() {
    let mut f = ok_file();
    f.tail = vec![0, 0, 0];
    assert_rule(&f.bytes(), Rule::TrailingBytesAfterTrailer);
}

#[test]
fn second_trailer() {
    let mut f = ok_file();
    let t = f.chunks.last().unwrap().clone();
    f.chunks.push(t);
    assert_rule(&f.bytes(), Rule::SecondTrailer);
}

// ---- 3. Chunk payloads ------------------------------------------------------------------------------------------

#[test]
fn payload_length() {
    let mut f = ok_file();
    let mut p = Hdr::default().payload();
    p.push(0);
    f.chunks[0] = chunk(T_HEADER, p);
    assert_rule(&f.bytes(), Rule::PayloadLength);
}

#[test]
fn header_abi() {
    assert_rule(&with_header(Hdr { abi: 3, ..Hdr::default() }), Rule::HeaderAbi);
}

#[test]
fn header_consumer_mode() {
    assert_rule(&with_header(Hdr { mode: 0, ..Hdr::default() }), Rule::HeaderConsumerMode);
}

#[test]
fn header_reserved() {
    assert_rule(&with_header(Hdr { reserved: 1, ..Hdr::default() }), Rule::HeaderReserved);
}

#[test]
fn header_max_threads() {
    assert_rule(&with_header(Hdr { max_threads: 1025, ..Hdr::default() }), Rule::HeaderMaxThreads);
}

#[test]
fn header_max_descriptors() {
    assert_rule(&with_header(Hdr { max_descs: 65_537, ..Hdr::default() }), Rule::HeaderMaxDescriptors);
}

fn with_clocks(p: Vec<u8>) -> Vec<u8> {
    let mut f = ok_file();
    f.chunks[1] = chunk(T_CLOCKS, p);
    f.bytes()
}

#[test]
fn clocks_count() {
    assert_rule(&with_clocks(clocks_payload(2, 1, 0, 79_800_000, 0)), Rule::ClocksCount);
}

#[test]
fn clocks_domain() {
    assert_rule(&with_clocks(clocks_payload(1, 2, 0, 79_800_000, 0)), Rule::ClocksDomain);
}

#[test]
fn clocks_reserved() {
    assert_rule(&with_clocks(clocks_payload(1, 1, 1, 79_800_000, 0)), Rule::ClocksReserved);
}

#[test]
fn clocks_freq_zero() {
    assert_rule(&with_clocks(clocks_payload(1, 1, 0, 0, 0)), Rule::ClocksFreqZero);
}

#[test]
fn descriptors_count_zero() {
    let mut f = ok_file();
    f.chunks[2] = chunk(T_DESCS, 0u32.to_le_bytes().to_vec());
    assert_rule(&f.bytes(), Rule::DescriptorsCountZero);
}

#[test]
fn descriptor_scope() {
    let mut ds = std_descs();
    ds[1].scope = 3;
    assert_rule(&with_descs(ds), Rule::DescriptorScope);
}

#[test]
fn descriptor_reserved() {
    let mut ds = std_descs();
    ds[1].reserved = 1;
    assert_rule(&with_descs(ds), Rule::DescriptorReserved);
}

#[test]
fn descriptor_name_empty() {
    let mut ds = std_descs();
    ds[1].name = Vec::new();
    assert_rule(&with_descs(ds), Rule::DescriptorNameEmpty);
}

#[test]
fn descriptor_index_range() {
    let mut ds = std_descs();
    ds[2].index = 16; // max_descriptors is 16
    assert_rule(&with_descs(ds), Rule::DescriptorIndexRange);
}

#[test]
fn descriptor_index_duplicate() {
    let mut ds = std_descs();
    ds[2].index = 1;
    assert_rule(&with_descs(ds), Rule::DescriptorIndexDuplicate);
}

#[test]
fn descriptor_id_mismatch() {
    let mut ds = std_descs();
    ds[1].id = Some(1);
    assert_rule(&with_descs(ds), Rule::DescriptorIdMismatch);
}

#[test]
fn descriptor_id_duplicate() {
    let mut ds = std_descs();
    // Same (file, line, name) as descriptor 0, so the same id; only the tag differs.
    ds[2].name = ds[0].name.clone();
    ds[2].line = ds[0].line;
    ds[2].tag = b"other".to_vec();
    assert_rule(&with_descs(ds), Rule::DescriptorIdDuplicate);
}

#[test]
fn threads_count_zero() {
    assert_rule(&with_threads_payload(0u32.to_le_bytes().to_vec()), Rule::ThreadsCountZero);
}

#[test]
fn thread_incarnation_zero() {
    assert_rule(&with_threads_payload(threads_payload(&[thr(1), thr(0)])), Rule::ThreadIncarnationZero);
}

#[test]
fn thread_slot_range() {
    let mut t = thr(2);
    t.slot = 8; // max_threads is 8
    assert_rule(&with_threads_payload(threads_payload(&[thr(1), t])), Rule::ThreadSlotRange);
}

#[test]
fn incarnation_duplicate() {
    assert_rule(&with_threads_payload(threads_payload(&[thr(1), thr(1)])), Rule::IncarnationDuplicate);
}

#[test]
fn too_many_live_incarnations() {
    let ts: Vec<Thr> = (1..=9).map(thr).collect();
    assert_rule(&with_threads_payload(threads_payload(&ts)), Rule::TooManyLiveIncarnations);
}

#[test]
fn records_count_zero() {
    let mut f = ok_file();
    let i = f.find(T_RECORDS);
    f.chunks[i] = chunk(T_RECORDS, records_payload(1, &[]));
    assert_rule(&f.bytes(), Rule::RecordsCountZero);
}

#[test]
fn records_length() {
    let mut f = ok_file();
    let i = f.find(T_RECORDS);
    let mut p = records_payload(1, &[frame(0, 10, 0)]);
    p[4..8].copy_from_slice(&2u32.to_le_bytes()); // n = 2, one record present
    f.chunks[i] = chunk(T_RECORDS, p);
    assert_rule(&f.bytes(), Rule::RecordsLength);
}

#[test]
fn retired_count_zero() {
    let mut f = ok_file();
    let i = f.find(T_TRAILER);
    f.chunks.insert(i, chunk(T_RETIRED, 0u32.to_le_bytes().to_vec()));
    assert_rule(&f.bytes(), Rule::RetiredCountZero);
}

#[test]
fn retired_state() {
    let mut l = ok_lane();
    l.state = 1;
    let mut fin = l.fin();
    fin.state = 2;
    let mut f = scenario(vec![l]).file();
    let i = f.find(T_RETIRED);
    f.chunks[i] = chunk(T_RETIRED, retired_payload(&[fin]));
    assert_rule(&f.bytes(), Rule::RetiredState);
}

#[test]
fn undeclared_incarnation() {
    let mut f = ok_file();
    let i = f.find(T_RECORDS);
    f.chunks.insert(i, chunk(T_RECORDS, records_payload(5, &[frame(0, 10, 0)])));
    assert_rule(&f.bytes(), Rule::UndeclaredIncarnation);
}

#[test]
fn undeclared_descriptor() {
    let recs = seqd(0, [zone(0, 9, 0, 100, 1, 0), vec![end(0, 9, 0, 200, 1)]].concat());
    assert_rule(&with_recs(recs), Rule::UndeclaredDescriptor);
}

#[test]
fn undeclared_token_owner() {
    let recs = seqd(0, [zone_tok(0, 0, 0, 100, 1, 0, 7, 0), vec![end(0, 0, 0, 200, 1)]].concat());
    assert_rule(&with_recs(recs), Rule::UndeclaredTokenOwner);
}

#[test]
fn records_after_retired() {
    let mut l = ok_lane();
    l.state = 1;
    let mut f = scenario(vec![l]).file();
    let i = f.find(T_RETIRED);
    f.chunks.insert(i + 1, chunk(T_RECORDS, records_payload(1, &[frame(3, 300, 0)])));
    assert_rule(&f.bytes(), Rule::RecordsAfterRetired);
}

#[test]
fn duplicate_final_entry() {
    let mut l = ok_lane();
    l.state = 1;
    let s = scenario(vec![l.clone()]);
    let mut t = s.trailer_struct();
    t.fins.push(l.fin());
    let mut f = s.file();
    let i = f.find(T_TRAILER);
    f.chunks[i] = chunk(T_TRAILER, t.payload());
    assert_rule(&f.bytes(), Rule::DuplicateFinalEntry);
}

// ---- 4. Records -------------------------------------------------------------------------------------------------

#[test]
fn unknown_record_kind() {
    assert_rule(&with_recs(vec![rec(7, 0, 0, 0, 0, 0, 0)]), Rule::UnknownRecordKind);
}

#[test]
fn record_reserved_bits() {
    let mut recs = seqd(0, [zone(0, 0, 0, 100, 1, 0), vec![end(0, 0, 0, 200, 1)]].concat());
    recs[0].flags = 2; // BEGIN has no flags
    assert_rule(&with_recs(recs), Rule::RecordReserved);
}

#[test]
fn record_reserved_field() {
    let mut f = frame(0, 10, 0);
    f.a = 1; // FRAME a is reserved
    assert_rule(&with_recs(vec![f]), Rule::RecordReserved);
}

#[test]
fn depth_on_non_span_record() {
    let mut f = frame(0, 10, 0);
    f.depth = 1;
    assert_rule(&with_recs(vec![f]), Rule::DepthOnNonSpanRecord);
}

#[test]
fn token_flag_mismatch() {
    let mut recs = seqd(0, [zone(0, 0, 0, 100, 1, 0), vec![end(0, 0, 0, 200, 1)]].concat());
    recs[1].flags = 1; // TOKEN set while a == 0
    assert_rule(&with_recs(recs), Rule::TokenFlagMismatch);
}

#[test]
fn regerr_code() {
    assert_rule(&with_recs(vec![regerr(0, 3)]), Rule::RegerrCode);
}

#[test]
fn span_id_zero() {
    let recs = seqd(0, [zone(0, 0, 0, 100, 0, 0), vec![end(0, 0, 0, 200, 0)]].concat());
    assert_rule(&with_recs(recs), Rule::SpanIdZero);
}

#[test]
fn begin_parent_placement_lone_begin() {
    let recs = seqd(0, vec![begin(0, 0, 0, 100, 1), end(0, 0, 0, 200, 1)]);
    assert_rule(&with_recs(recs), Rule::BeginParentPlacement);
}

#[test]
fn begin_parent_placement_lone_begin_parent() {
    let recs = seqd(0, vec![bparent(0, 0, 0)]);
    assert_rule(&with_recs(recs), Rule::BeginParentPlacement);
}

#[test]
fn begin_parent_placement_depth_differs() {
    let mut recs = seqd(0, [zone(0, 0, 0, 100, 1, 0), vec![end(0, 0, 0, 200, 1)]].concat());
    recs[1].depth = 1;
    assert_rule(&with_recs(recs), Rule::BeginParentPlacement);
}

#[test]
fn seq_range() {
    // GAP [0, 2^63 - 1] then a record at 2^63.
    assert_rule(&with_recs(vec![gap(0, P63 - 1), frame(P63, 10, 0)]), Rule::SeqRange);
}

#[test]
fn seq_order_first_record_not_zero() {
    assert_rule(&with_recs(vec![frame(1, 10, 0)]), Rule::SeqOrder);
}

#[test]
fn seq_order_gap_range_does_not_fit() {
    // GAP [0, 1] but the next record is seq 3.
    assert_rule(&with_recs(vec![gap(0, 1), frame(3, 10, 1)]), Rule::SeqOrder);
}

#[test]
fn gap_placement_last_in_chunk() {
    assert_rule(&with_recs(vec![frame(0, 10, 0), gap(1, 1)]), Rule::GapPlacement);
}

#[test]
fn gap_seq_field() {
    let mut g = gap(1, 1);
    g.seq = 2;
    assert_rule(&with_recs(vec![frame(0, 10, 0), g, frame(2, 30, 2)]), Rule::GapSeqField);
}

#[test]
fn span_id_duplicate() {
    let recs = seqd(
        0,
        [
            zone(0, 0, 0, 100, 1, 0),
            vec![end(0, 0, 0, 200, 1)],
            zone(0, 0, 0, 300, 1, 0),
            vec![end(0, 0, 0, 400, 1)],
        ]
        .concat(),
    );
    assert_rule(&with_recs(recs), Rule::SpanIdDuplicate);
}

#[test]
fn depth_parent_zero_mismatch() {
    let recs = seqd(0, [zone(0, 0, 0, 100, 1, 5), vec![end(0, 0, 0, 200, 1)]].concat());
    assert_rule(&with_recs(recs), Rule::DepthParentZeroMismatch);
}

#[test]
fn stack_begin_mismatch() {
    let recs = seqd(0, [zone(0, 0, 0, 100, 1, 0), zone(0, 1, 1, 150, 2, 9)].concat());
    assert_rule(&with_recs(recs), Rule::StackBeginMismatch);
}

#[test]
fn stack_end_mismatch() {
    let recs = seqd(0, [zone(0, 0, 0, 100, 1, 0), vec![end(0, 0, 0, 200, 2)]].concat());
    assert_rule(&with_recs(recs), Rule::StackEndMismatch);
}

#[test]
fn known_empty_begin_depth() {
    let recs = seqd(0, zone(0, 0, 1, 100, 1, 9));
    assert_rule(&with_recs(recs), Rule::KnownEmptyBeginDepth);
}

#[test]
fn known_empty_end() {
    assert_rule(&with_recs(vec![end(0, 0, 0, 100, 1)]), Rule::KnownEmptyEnd);
}

#[test]
fn frame_id_sequence() {
    assert_rule(&with_recs(vec![frame(0, 10, 0), frame(1, 20, 2)]), Rule::FrameIdSequence);
}

// ---- 6. TRAILER and final entries -------------------------------------------------------------------------------

#[test]
fn trailer_end_flags_reserved() {
    assert_rule(&with_trailer(|t| t.end_flags |= 8), Rule::TrailerEndFlagsReserved);
}

#[test]
fn trailer_reserved() {
    assert_rule(&with_trailer(|t| t.reserved = 1), Rule::TrailerReserved);
}

#[test]
fn final_entry_state() {
    assert_rule(&with_trailer(|t| t.fins[0].state = 4), Rule::FinalEntryState);
}

#[test]
fn final_entry_reserved() {
    assert_rule(&with_trailer(|t| t.fins[0].reserved = 1), Rule::FinalEntryReserved);
}

#[test]
fn final_entry_limit_flags_reserved() {
    assert_rule(&with_trailer(|t| t.fins[0].limit = 4), Rule::FinalEntryLimitFlagsReserved);
}

#[test]
fn exact_entry_unknown() {
    assert_rule(&with_trailer(|t| t.fins[0].produced = UNKNOWN), Rule::ExactEntryUnknown);
}

#[test]
fn state3_fields() {
    assert_rule(
        &with_trailer(|t| {
            t.fins[0] = Fin { state: 3, limit: 2, ..t.fins[0] };
            t.end_flags |= 1;
        }),
        Rule::State3Fields,
    );
}

#[test]
fn written_count_mismatch() {
    assert_rule(&with_trailer(|t| t.fins[0].written += 1), Rule::WrittenCountMismatch);
}

#[test]
fn produced_count_range() {
    assert_rule(
        &with_trailer(|t| {
            t.fins[0].produced = P63 + 1;
            t.fins[0].dropped = P63 + 1 - t.fins[0].written;
        }),
        Rule::ProducedCountRange,
    );
}

#[test]
fn produced_count_arithmetic() {
    assert_rule(&with_trailer(|t| t.fins[0].produced += 1), Rule::ProducedCountArithmetic);
}

#[test]
fn dropped_count_arithmetic() {
    // FRAME 0, GAP [1, 1], FRAME 2: written 2, GAP ranges 1.  produced 2 = written 2 + dropped 0 balances, but the
    // GAP range is not covered by dropped_count (produced < last written seq + 1).
    let l = lane(1, vec![frame(0, 10, 0), gap(1, 1), frame(2, 30, 2)]);
    let s = scenario(vec![l]);
    let mut t = s.trailer_struct();
    t.fins[0].produced = 2;
    t.fins[0].dropped = 0;
    let mut f = s.file();
    let i = f.find(T_TRAILER);
    f.chunks[i] = chunk(T_TRAILER, t.payload());
    assert_rule(&f.bytes(), Rule::DroppedCountArithmetic);
}

#[test]
fn descriptor_count_mismatch() {
    assert_rule(&with_trailer(|t| t.desc_count = 2), Rule::DescriptorCountMismatch);
}

#[test]
fn end_flag_not_quiesced_mismatch() {
    let mut l = ok_lane();
    l.state = 3;
    let s = scenario(vec![l]);
    let mut t = s.trailer_struct();
    t.end_flags &= !1;
    let mut f = s.file();
    let i = f.find(T_TRAILER);
    f.chunks[i] = chunk(T_TRAILER, t.payload());
    assert_rule(&f.bytes(), Rule::EndFlagNotQuiescedMismatch);
}

#[test]
fn end_flag_seq_limit_mismatch() {
    assert_rule(&with_trailer(|t| t.end_flags |= 2), Rule::EndFlagSeqLimitMismatch);
}

#[test]
fn end_flag_sink_error_mismatch() {
    assert_rule(&with_trailer(|t| t.sink_errors = 1), Rule::EndFlagSinkErrorMismatch);
}

// ---- review additions: exact count ranges (section 6) and REGERR reconciliation (section 5.3) ----------------------

#[test]
fn written_count_over_2_pow_63() {
    assert_rule(&with_trailer(|t| t.fins[0].written = P63 + 1), Rule::ExactCountRange);
}

#[test]
fn dropped_count_over_2_pow_63() {
    // Nothing written, 2^63 + 1 lost at the tail: dropped (and produced) over the bound; dropped is checked first.
    let mut l = lane(1, Vec::new());
    l.terminal = P63 + 1;
    assert_rule(&scenario(vec![l]).bytes(), Rule::ExactCountRange);
}

#[test]
fn refused_count_over_2_pow_63() {
    assert_rule(&with_trailer(|t| t.fins[0].refused = P63 + 1), Rule::ExactCountRange);
}

#[test]
fn regerr_count_over_2_pow_63() {
    assert_rule(&with_trailer(|t| t.fins[0].regerr = P63 + 1), Rule::ExactCountRange);
}

#[test]
fn open_spans_over_2_pow_63() {
    assert_rule(&with_trailer(|t| t.fins[0].open = P63 + 1), Rule::ExactCountRange);
}

#[test]
fn state3_written_count_over_2_pow_63() {
    // written_count is exact even in a NOT_QUIESCED entry, so UNKNOWN there is out of range.
    assert_rule(
        &with_trailer(|t| {
            t.fins[0] = Fin { state: 3, limit: 2, ..t.fins[0] };
            t.fins[0].produced = UNKNOWN;
            t.fins[0].dropped = UNKNOWN;
            t.fins[0].refused = UNKNOWN;
            t.fins[0].regerr = UNKNOWN;
            t.fins[0].open = UNKNOWN;
            t.fins[0].written = UNKNOWN;
            t.end_flags |= 1;
        }),
        Rule::ExactCountRange,
    );
}

#[test]
fn regerr_record_with_regerr_count_0() {
    // A REGERR record was written, but the unconditional counter says 0.
    assert_rule(&with_recs(vec![regerr(0, 1)]), Rule::RegerrCountBelowRecords);
}

// ---- review additions: FRAME ids never saturate (section 4) ---------------------------------------------------------

#[test]
fn frame_id_repeated_after_gap() {
    assert_rule(&with_recs(vec![frame(0, 10, 0), gap(1, 1), frame(2, 30, 0)]), Rule::FrameIdSequence);
}

#[test]
fn frame_id_u64_max_after_gap() {
    // Codex's case: u64::MAX after a GAP (previously accepted, then a second u64::MAX passed by saturation).
    let recs = vec![frame(0, 10, 0), gap(1, 1), frame(2, 30, u64::MAX), frame(3, 40, u64::MAX)];
    assert_rule(&with_recs(recs), Rule::FrameIdBound);
}

#[test]
fn frame_id_one_above_its_seq() {
    assert_rule(&with_recs(vec![frame(0, 10, 0), gap(1, 1), frame(2, 30, 3)]), Rule::FrameIdBound);
}

// ---- review additions: one live incarnation per slot (section 3) ----------------------------------------------------

#[test]
fn slot_already_live() {
    let t2 = Thr { slot: 1, ..thr(2) };
    assert_rule(&with_threads_payload(threads_payload(&[thr(1), t2])), Rule::SlotAlreadyLive);
}

#[test]
fn slot_reused_before_its_incarnation_retired() {
    assert_rule(&slot_reuse_file(false), Rule::SlotAlreadyLive);
}

// ---- review additions: the short-header prefix (section 2) ----------------------------------------------------------

#[test]
fn short_header_valid_prefix_at_every_length_is_torn() {
    let h = v1_header();
    for n in 0..16 {
        let (_, why) = incomplete(&h[..n]);
        assert!(why.contains(&Incomplete::TornTail), "{n}-byte prefix: {why:?}");
    }
}

#[test]
fn short_header_bad_byte_at_every_cut_point() {
    for n in 1..16usize {
        let mut h = v1_header();
        h.truncate(n);
        // The last byte present, unless it is a minor-version byte (10, 11: any value is legal); then byte 9.
        let pos = if n - 1 == 10 || n - 1 == 11 { 9 } else { n - 1 };
        h[pos] ^= 0x40;
        let want = if pos < 8 {
            Rule::MagicMismatch
        } else if pos < 10 {
            Rule::MajorVersion
        } else {
            Rule::FileFlagsNonzero
        };
        assert_eq!(malformed(&h).rule, want, "cut at {n}, bad byte {pos}");
    }
}

#[test]
fn fifteen_byte_header_with_nonzero_flags_byte_is_not_torn() {
    let mut h = v1_header();
    h.truncate(15);
    h[14] = 1;
    assert_rule(&h, Rule::FileFlagsNonzero);
}

// ---- 9. Token rule 4 --------------------------------------------------------------------------------------------

#[test]
fn token_impossible() {
    let owner = lane(1, vec![frame(0, 10, 0)]);
    let worker = lane(2, seqd(0, [zone_tok(0, 0, 0, 100, 1, 0, 1, 5), vec![end(0, 0, 0, 200, 1)]].concat()));
    assert_rule(&scenario(vec![owner, worker]).bytes(), Rule::TokenImpossible);
}
