mod arcs_support;
mod common;
mod pair_support;
use arcs_support::encode;
use pair_support::*;
use ps3dk_prof::{arcs::read_arcs, companions::match_companions};
#[test]
fn exact_pair_matches() {
    let p = timing();
    let a = read_arcs(&encode(&bind(&p, true, &[(0x1004, 0x1104, 7)]))).unwrap();
    assert!(match_companions(&a, &p.bytes()).unwrap().is_final());
}
#[test]
fn absent_timing_refused() {
    let p = timing();
    let a = read_arcs(&encode(&bind(&p, true, &[]))).unwrap();
    assert!(match_companions(&a, &[]).is_err());
}
#[test]
fn wrong_build_refused() {
    let p = timing();
    let mut c = bind(&p, true, &[]);
    c[0].1[7] ^= 1;
    let a = read_arcs(&encode(&c)).unwrap();
    assert!(match_companions(&a, &p.bytes()).is_err());
}
#[test]
fn wrong_clock_refused() {
    let p = timing();
    let mut c = bind(&p, true, &[]);
    c[1].1[12] ^= 1;
    let a = read_arcs(&encode(&c)).unwrap();
    assert!(match_companions(&a, &p.bytes()).is_err());
}
#[test]
fn wrong_thread_refused() {
    let p = timing();
    let mut c = bind(&p, true, &[]);
    c[2].1[12] ^= 1;
    let a = read_arcs(&encode(&c)).unwrap();
    assert!(match_companions(&a, &p.bytes()).is_err());
}
#[test]
fn extra_thread_refused() {
    let p = timing();
    let mut c = bind(&p, true, &[]);
    c.insert(3, (4, common::threads_payload(&[common::thr(2)])));
    let a = read_arcs(&encode(&c)).unwrap();
    assert!(match_companions(&a, &p.bytes()).is_err());
}
#[test]
fn altered_bytes_refused() {
    let p = timing();
    let a = read_arcs(&encode(&bind(&p, true, &[]))).unwrap();
    let mut b = p.bytes();
    b[40] ^= 1;
    assert!(match_companions(&a, &b).is_err());
}
#[test]
fn progress_ignores_later_invalid_tail() {
    let p = timing().without_trailer();
    let a = read_arcs(&encode(&bind(&p, false, &[]))).unwrap();
    let mut b = p.bytes();
    b.extend(b"invalid future");
    assert!(!match_companions(&a, &b).unwrap().is_final());
}
#[test]
fn final_rejects_appended_bytes() {
    let p = timing();
    let a = read_arcs(&encode(&bind(&p, true, &[]))).unwrap();
    let mut b = p.bytes();
    b.push(0);
    assert!(match_companions(&a, &b).is_err());
}
#[test]
fn progress_rejects_partial_chunk_even_with_matching_crc() {
    let p = timing().without_trailer();
    let mut c = bind(&p, false, &[]);
    let mut b = p.bytes();
    b.pop();
    let n = c[0].1.len();
    c[0].1[n - 28..n - 20].copy_from_slice(&(b.len() as u64).to_le_bytes());
    c[0].1[n - 12..n - 8].copy_from_slice(&arcs_support::crc(&b).to_le_bytes());
    let a = read_arcs(&encode(&c)).unwrap();
    assert!(match_companions(&a, &b).is_err());
}
