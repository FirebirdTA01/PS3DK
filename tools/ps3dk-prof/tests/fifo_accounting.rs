//! Type43 is owner FIFO bytes, including profiler commands, never GPU execution.
mod common;
mod fifo_support;
use common::*;
use fifo_support::*;
use ps3dk_prof::{fifo, reader};

fn row(owner: u32, frame: u64, reasons: u32, p: u64, u: u64, wraps: u64, seq: u64) -> Chunk {
    let mut b = Buf::default();
    b.u16(1).u16(2 | u16::from(reasons == 0)).u32(1).u32(1).u32(D).u32(owner).u32(reasons)
        .u64(frame).u64(0).u64(p).u64(u).u64(wraps).u64(seq).u64(0);
    chunk(43, b.0)
}
fn json(f: &File) -> serde_json::Value {
    let c = reader::read(&f.bytes()).expect("type43 must decode");
    let mut out = Vec::new();
    fifo::write_json(&mut out, &fifo::analyze(&c)).unwrap();
    serde_json::from_slice(&out).unwrap()
}

#[test]
fn owner_bytes_and_separate_publication_survive_decode() {
    let v = json(&with(vec![row(OWNER, 7, 0, 120, 112, 2, 1), row(OWNER, 8, 0, 0, 8, 0, 2)]));
    assert_eq!(v["owner_accounting"]["frames"][0]["prepared_bytes"], 120);
    assert_eq!(v["owner_accounting"]["frames"][0]["published_bytes"], 112);
    assert_eq!(v["owner_accounting"]["frames"][0]["wraps"], 2);
    assert_eq!(v["owner_accounting"]["frames"][1]["published_bytes"], 8);
    assert_eq!(v["gpu_execution"], "UNKNOWN");
}
#[test]
fn unavailable_counts_are_null_and_old_json_is_unchanged() {
    assert!(json(&with(vec![])).get("owner_accounting").is_none());
    let v = json(&with(vec![row(OWNER, 0, 2, 0, 0, 0, 1)]));
    assert_eq!(v["owner_accounting"]["frames"][0]["state"], "UNAVAILABLE");
    assert!(v["owner_accounting"]["frames"][0]["prepared_bytes"].is_null());
}
#[test]
fn owner_reason_sequence_and_unavailable_value_guards() {
    for rows in [vec![row(2, 7, 0, 0, 0, 0, 1)], vec![row(OWNER, 7, 128, 0, 0, 0, 1)],
        vec![row(OWNER, 7, 2, 4, 0, 0, 1)],
        vec![row(OWNER, 7, 0, 4, 4, 0, 1), row(OWNER, 8, 0, 4, 4, 0, 1)],
        vec![row(OWNER, 7, 0, 4, 4, 0, 1), row(OWNER, 7, 0, 4, 4, 0, 2)],
        vec![row(OWNER, 7, 0, 3, 4, 0, 1)]] {
        assert!(reader::read(&with(rows).bytes()).is_err());
    }
}
#[test]
fn retention_refusal_still_validates_later_rows() {
    let f = with(vec![row(OWNER, 7, 0, 4, 4, 0, 1), row(OWNER, 8, 0, 4, 4, 0, 2)]);
    let c = reader::read_with_fifo_budgets(&f.bytes(), fifo::Budgets { events: 1, admission_ops: 64 }).unwrap();
    assert!(c.fifo.unwrap().refusal.is_some());
    let bad = with(vec![row(OWNER, 7, 0, 4, 4, 0, 1), row(2, 8, 0, 4, 4, 0, 2)]);
    assert!(reader::read_with_fifo_budgets(&bad.bytes(), fifo::Budgets { events: 0, admission_ops: 0 }).is_err());
}

#[test]
fn identity_flags_layout_and_final_status_guards() {
    let valid = row(OWNER, 7, 0, 120, 112, 2, 1);
    for (offset, value) in [(0, 2u8), (2, 1), (4, 2), (8, 2), (12, 3), (72, 1), (64, 0)] {
        let mut bad = valid.clone(); bad.payload[offset] = value;
        assert!(reader::read(&with(vec![bad]).bytes()).is_err(), "offset {offset}");
    }
    let mut short = valid.clone(); short.payload.pop();
    assert!(reader::read(&with(vec![short]).bytes()).is_err());
    assert!(reader::read(&with(vec![St::new(1, 0, 0, 0, 0, 0, 0, 0).chunk(), valid]).bytes()).is_err());
}

#[test]
fn sequence_gaps_are_scalar_missing_rows_not_allocations() {
    let v = json(&with(vec![row(OWNER, 7, 0, 4, 4, 0, (1u64 << 62) + 1)]));
    assert_eq!(v["owner_accounting"]["known_missing_rows"], 1u64 << 62);
    assert_eq!(v["owner_accounting"]["frames"].as_array().unwrap().len(), 1);
    assert_eq!(v["owner_accounting"]["coverage"], "PARTIAL_ROWS");
}

fn window_begin() -> Chunk {
    let mut b = Buf::default();
    b.u16(1).u16(0).u32(1).u32(u32::MAX).u64(7).u8(1).u8(1).u16(0).u32(0).u32(0);
    chunk(41, b.0)
}
fn window_end(last: u64) -> Chunk {
    let mut b = Buf::default();
    b.u16(1).u16(0).u32(1).u8(1).u8(0).u16(0).u64(last).u32(0).u64(0);
    chunk(42, b.0)
}
#[test]
fn window_identity_and_later_close_validate_even_without_retention() {
    let mut r = row(OWNER, 8, 0, 4, 4, 0, 1);
    r.payload[32] = 1;
    let mut f = with(vec![window_begin(), r.clone(), window_end(8)]); f.flags |= 512;
    assert!(reader::read(&f.bytes()).is_ok());
    let mut f = with(vec![window_begin(), r.clone(), window_end(7)]); f.flags |= 512;
    assert!(reader::read_with_fifo_budgets(&f.bytes(), fifo::Budgets { events: 0, admission_ops: 0 }).is_err());
    let mut f = with(vec![r]); f.flags |= 512;
    assert!(reader::read(&f.bytes()).is_err());
}
