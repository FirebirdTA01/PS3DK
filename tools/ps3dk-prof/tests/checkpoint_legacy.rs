//! Legacy boundary controls for the opt-in frame checkpoint extension.
//! These capture existing behavior before adding bit 3; they do not grant
//! any scoped completion claim to legacy files.

mod common;

use common::*;
use ps3dk_prof::{Incomplete, Rule};

fn refused(bytes: &[u8], rule: Rule, detail: &str) {
    let m = reader::read(bytes).expect_err("expected a structural refusal");
    assert_eq!(m.rule, rule);
    assert_eq!(m.detail, detail);
}

fn headers(flags: u32) -> File {
    let mut f = File::new(vec![
        chunk(T_HEADER, Hdr::default().payload()),
        chunk(T_CLOCKS, clocks_payload(1, 1, 0, 79_800_000, 0)),
    ]);
    f.flags = flags;
    f
}

#[test]
fn neither_optional_feature_preserves_partial_header_masks_and_diagnostics() {
    // Compare the legacy family only: bits 3 and 4 now opt into independent extensions.
    for flags in (0u8..=255).filter(|b| b & (8 | 16) == 0) {
        let mut f = headers(u32::from(flags));
        // No chunks are present: only the 13..15-byte header gate runs.
        f.chunks.clear();
        for n in 13..16 {
            let b = f.bytes();
            if [0, 1, 3, 7].contains(&flags) {
                let c = reader::read(&b[..n]).unwrap();
                assert!(c.incomplete.contains(&Incomplete::TornTail));
            } else {
                let allowed = if flags & 2 == 0 { "0 or 1" } else { "0, 1, 3 or 7" };
                refused(
                    &b[..n],
                    Rule::FileFlagsNonzero,
                    &format!("{n}-byte file: file_flags bytes are not {allowed}"),
                );
            }
        }
    }
}

#[test]
fn neither_optional_feature_preserves_full_header_prerequisite_precedence() {
    // Combined-capability coverage is separate from these unchanged legacy diagnostics.
    for flags in (0u32..=255).filter(|b| b & (8 | 16) == 0) {
        if [0, 1, 3, 7].contains(&flags) {
            continue;
        }
        let detail = if flags & !7 != 0 || (flags & 4 != 0 && flags & 2 == 0) {
            format!("file_flags {flags:#x}")
        } else {
            format!("file_flags {flags:#x}: bit 1 without bit 0")
        };
        refused(&headers(flags).bytes(), Rule::FileFlagsNonzero, &detail);
    }
}

#[test]
fn checkpoint_types_stay_unknown_without_the_capability() {
    for flags in [0, 1] {
        for ty in 19..=21 {
            let mut f = headers(flags);
            f.chunks.push(chunk(ty, vec![]));
            refused(&f.bytes(), Rule::UnknownChunkType, &format!("chunk 2: type {ty}"));
        }
    }
}

#[test]
fn profile_placement_still_precedes_the_unknown_checkpoint_type() {
    for flags in [3, 7] {
        for ty in 19..=21 {
            let mut f = headers(flags);
            f.chunks.push(chunk(ty, vec![]));
            refused(
                &f.bytes(),
                Rule::ProfileMissing,
                &format!("file_flags bit 1 and chunk 2 has type {ty}"),
            );
        }
    }
}

#[test]
fn length_crc_and_sequence_precede_type_and_profile_checks() {
    for flags in [0, 1, 3, 7] {
        for ty in 19..=21 {
            let mut f = headers(flags);
            f.chunks.push(chunk(ty, vec![0; 4]));
            f.bad_crc = Some(2);
            f.seq_override = Some((2, 99));
            refused(&f.bytes(), Rule::ChunkCrc, "chunk 2: CRC mismatch");
            f.len_override = Some((2, 1_048_577));
            refused(&f.bytes(), Rule::ChunkLenOverCap, "chunk 2: len 1048577");
            f.len_override = None;
            f.bad_crc = None;
            refused(&f.bytes(), Rule::ChunkSeqOrder, "chunk 2 carries chunk_seq 99");
        }
    }
}

#[test]
fn torn_unknown_chunk_is_not_dispatched_before_its_crc_arrives() {
    for ty in 19..=21 {
        let prefix = headers(0).bytes();
        let mut f = headers(0);
        f.chunks.push(chunk(ty, vec![0; 4]));
        let bytes = f.bytes();
        for n in prefix.len() + 1..bytes.len() {
            let c = reader::read(&bytes[..n]).unwrap();
            assert!(c.incomplete.contains(&Incomplete::TornTail), "type={ty}, cut={n}");
        }
        refused(&bytes, Rule::UnknownChunkType, &format!("chunk 2: type {ty}"));
    }
}

#[test]
fn after_trailer_guard_precedes_new_chunk_length_crc_and_type() {
    let base = scenario(vec![]).file();
    reader::read(&base.bytes()).unwrap();
    for ty in [6, 19, 20, 21] {
        let mut f = base.clone();
        let i = f.chunks.len();
        f.chunks.push(chunk(ty, vec![]));
        f.len_override = Some((i, 1_048_577));
        f.bad_crc = Some(i);
        f.seq_override = Some((i, 999));
        if ty == 6 {
            refused(&f.bytes(), Rule::SecondTrailer, "a second TRAILER follows the first");
        } else {
            refused(
                &f.bytes(),
                Rule::TrailingBytesAfterTrailer,
                "20 bytes after the TRAILER",
            );
        }
    }
}
