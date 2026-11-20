//! Independent finite-slot exhaustive enumerator for the combined checkpoint position proof.
//! Deliberately includes the pure module without wiring a rolling runtime API.
#[path = "../src/rolling_combined_positions.rs"]
mod positions;
use positions::{verify_positions, Error, Limits, PpuDemand, SpuBlock};

fn p(start: u64, end: u64, chunks: u64) -> PpuDemand { PpuDemand { start, end, chunks } }
fn s(first: u64, last: u64, chunks: u64) -> SpuBlock { SpuBlock { first, last, chunks } }
fn verify(n: u64, metadata: &[u64], ppu: &[PpuDemand], spu: &[SpuBlock]) -> Result<(), Error> {
    verify_positions(n, metadata, ppu, spu, Limits::default())
}
fn invalid(result: Result<(), Error>) {
    assert!(matches!(result, Err(Error::Invalid(_))), "expected malformed position claims, got {result:?}");
}

#[test]
fn empty_and_metadata_only_prefixes_are_fully_occupied() {
    assert_eq!(verify(0, &[], &[], &[]), Ok(()));
    assert_eq!(verify(3, &[0, 1, 2], &[], &[]), Ok(()));
}

#[test]
fn free_positions_must_all_be_occupied() {
    assert_eq!(verify(1, &[], &[], &[]), Err(Error::Infeasible));
    assert_eq!(verify(4, &[0], &[p(1, 4, 2)], &[]), Err(Error::Infeasible));
    assert_eq!(verify(4, &[0], &[p(1, 4, 3)], &[]), Ok(()));
    assert_eq!(verify(4, &[0], &[p(1, 4, 4)], &[]), Err(Error::Infeasible));
}

#[test]
fn ppu_metadata_inside_a_spu_block_uses_a_distinct_slot() {
    // SPU endpoints 0 and 4; metadata 2; one SPU and one PPU in 1 and 3.
    assert_eq!(verify(5, &[2], &[p(1, 4, 1)], &[s(0, 4, 3)]), Ok(()));
    assert_eq!(verify(5, &[2], &[], &[s(0, 4, 5)]), Err(Error::Infeasible));
}

#[test]
fn forced_spu_endpoints_can_exhaust_a_ppu_lifetime() {
    // The only free slot is 0, outside the PPU incarnation's lifetime.
    assert_eq!(verify(4, &[3], &[p(1, 3, 1)], &[s(1, 2, 2)]), Err(Error::Infeasible));
    assert_eq!(verify(4, &[3], &[p(0, 3, 1)], &[s(1, 2, 2)]), Ok(()));
}

#[test]
fn competing_ppu_lifetimes_need_a_joint_assignment() {
    assert_eq!(verify(4, &[], &[p(0, 2, 2), p(0, 2, 2)], &[]), Err(Error::Infeasible));
    assert_eq!(verify(4, &[], &[p(0, 4, 2), p(0, 2, 2)], &[]), Ok(()));
    // A later release has an earlier deadline; a single initial sort is insufficient.
    assert_eq!(verify(6, &[], &[p(0, 6, 4), p(2, 4, 2)], &[]), Ok(()));
}

#[test]
fn lifetime_end_is_exclusive_and_retired_slots_can_be_reused() {
    assert_eq!(verify(6, &[0, 2, 3], &[p(1, 2, 1), p(4, 6, 2)], &[]), Ok(()));
    assert_eq!(verify(6, &[0, 2, 3], &[p(1, 2, 2), p(4, 6, 1)], &[]), Err(Error::Infeasible));
    assert_eq!(verify(3, &[0], &[p(1, 2, 2)], &[]), Err(Error::Infeasible));
}

#[test]
fn unused_zero_demands_are_valid_but_still_range_checked() {
    assert_eq!(verify(1, &[0], &[p(0, 0, 0), p(1, 1, 0)], &[]), Ok(()));
    assert_eq!(verify(1, &[], &[p(0, 0, 1)], &[]), Err(Error::Infeasible));
    invalid(verify(1, &[0], &[p(2, 2, 0)], &[]));
    invalid(verify(1, &[0], &[p(1, 0, 0)], &[]));
}

#[test]
fn one_chunk_spu_blocks_have_one_forced_endpoint() {
    assert_eq!(verify(3, &[1], &[], &[s(0, 0, 1), s(2, 2, 1)]), Ok(()));
    invalid(verify(3, &[], &[], &[s(0, 2, 1)]));
    invalid(verify(1, &[], &[], &[s(0, 0, 2)]));
    invalid(verify(1, &[], &[], &[s(0, 0, 0)]));
}

#[test]
fn multiple_blocks_cannot_claim_the_same_forced_slot() {
    invalid(verify(5, &[], &[], &[s(0, 2, 3), s(2, 4, 2)]));
    invalid(verify(5, &[], &[], &[s(3, 4, 2), s(0, 2, 3)]));
    assert_eq!(verify(5, &[], &[], &[s(0, 2, 3), s(3, 4, 2)]), Ok(()));
}

#[test]
fn metadata_and_forced_endpoints_are_disjoint_and_in_range() {
    invalid(verify(4, &[0], &[p(0, 4, 1)], &[s(0, 3, 2)]));
    invalid(verify(4, &[3], &[p(0, 4, 1)], &[s(0, 3, 2)]));
    invalid(verify(4, &[2, 1], &[p(0, 4, 2)], &[]));
    invalid(verify(4, &[1, 1], &[p(0, 4, 2)], &[]));
    invalid(verify(4, &[4], &[p(0, 4, 3)], &[]));
    invalid(verify(4, &[], &[], &[s(0, 4, 4)]));
    invalid(verify(4, &[], &[], &[s(3, 2, 4)]));
    invalid(verify(4, &[], &[p(0, 5, 4)], &[]));
}

#[test]
fn original_order_of_ppu_demand_rows_does_not_change_feasibility() {
    let forward = [p(0, 6, 2), p(1, 3, 2), p(4, 6, 2)];
    let reverse = [forward[2], forward[1], forward[0]];
    assert_eq!(verify(6, &[], &forward, &[]), Ok(()));
    assert_eq!(verify(6, &[], &reverse, &[]), Ok(()));
}

#[test]
fn u64_max_frontier_is_exclusive_without_endpoint_overflow() {
    let n = u64::MAX;
    assert_eq!(verify(n, &[], &[p(0, n, n)], &[]), Ok(()));
    assert_eq!(verify(n, &[], &[], &[s(0, n - 1, n)]), Ok(()));
    assert_eq!(verify(n, &[0, n - 1], &[p(1, n - 1, n - 2)], &[]), Ok(()));
    invalid(verify(n, &[n], &[], &[]));
    invalid(verify(n, &[], &[], &[s(0, n, n)]));
}

#[test]
fn aggregate_chunk_count_overflow_is_rejected() {
    let n = u64::MAX;
    invalid(verify(n, &[], &[p(0, n, n), p(0, n, 1)], &[]));
    invalid(verify(n, &[0], &[p(1, n, n)], &[]));
    invalid(verify(n, &[], &[p(0, n, n)], &[s(0, 0, 1)]));
}

#[test]
fn huge_multiplicity_has_tiny_structural_resource_cost() {
    let n = u64::MAX;
    let limits = Limits { max_temporary_items: 16, max_work: 4096 };
    assert_eq!(verify_positions(n, &[], &[p(0, n, n - 4)], &[s(2, n - 2, 4)], limits), Ok(()));
    // Same bounded state, but a release gap leaves the first original slot empty.
    assert_eq!(verify_positions(n, &[], &[p(1, n, n - 4)], &[s(2, n - 2, 4)], limits), Err(Error::Infeasible));
}

#[test]
fn structural_limits_fail_closed_before_unbounded_work() {
    let storage = Limits { max_temporary_items: 0, ..Limits::default() };
    assert!(matches!(verify_positions(1, &[], &[p(0, 1, 1)], &[], storage), Err(Error::Limit(_))));
    assert!(matches!(verify_positions(1, &[0], &[], &[], storage), Err(Error::Limit(_))));
    let work = Limits { max_work: 0, ..Limits::default() };
    assert!(matches!(verify_positions(1, &[], &[p(0, 1, 1)], &[], work), Err(Error::Limit(_))));
}

// This exhaustive enumerator works in the ORIGINAL tiny slot space. It has no free-rank
// compression, deadline sorting, greedy choice, or production helper calls.
fn brute_force(n: u64, metadata: &[u64], ppu: &[PpuDemand], spu: &[SpuBlock]) -> bool {
    assert!(n <= 6, "oracle may only enumerate tiny fixtures");
    let mut occupied = vec![false; n as usize];
    let mut previous = None;
    for &index in metadata {
        if index >= n || previous.is_some_and(|p| p >= index) { return false; }
        occupied[index as usize] = true;
        previous = Some(index);
    }
    let mut needs = Vec::new();
    for d in ppu {
        if d.start > d.end || d.end > n { return false; }
        needs.push((d.start, d.end, d.chunks));
    }
    previous = None;
    for b in spu {
        if b.first > b.last || b.last >= n || previous.is_some_and(|end| end >= b.first)
            || b.chunks == 0 || b.chunks > b.last - b.first + 1
            || (b.chunks == 1) != (b.first == b.last) { return false; }
        if occupied[b.first as usize] { return false; }
        occupied[b.first as usize] = true;
        let forced = if b.first == b.last { 1 } else {
            if occupied[b.last as usize] { return false; }
            occupied[b.last as usize] = true;
            2
        };
        needs.push((b.first, b.last + 1, b.chunks - forced));
        previous = Some(b.last);
    }
    fn fill(index: usize, occupied: &[bool], needs: &mut [(u64, u64, u64)]) -> bool {
        if index == occupied.len() { return needs.iter().all(|d| d.2 == 0); }
        if occupied[index] { return fill(index + 1, occupied, needs); }
        for i in 0..needs.len() {
            if needs[i].0 <= index as u64 && (index as u64) < needs[i].1 && needs[i].2 > 0 {
                needs[i].2 -= 1;
                let possible = fill(index + 1, occupied, needs);
                needs[i].2 += 1;
                if possible { return true; }
            }
        }
        false
    }
    fill(0, &occupied, &mut needs)
}

// Every ordered/disjoint SPU block set for this tiny frontier, including empty.
fn block_sets(n: u64, from: u64, current: &mut Vec<SpuBlock>, out: &mut Vec<Vec<SpuBlock>>) {
    out.push(current.clone());
    for first in from..n {
        for last in first..n {
            let minimum = if first == last { 1 } else { 2 };
            for chunks in minimum..=last - first + 1 {
                current.push(s(first, last, chunks));
                block_sets(n, last + 1, current, out);
                current.pop();
            }
        }
    }
}

#[test]
fn exhaustive_tiny_positions_match_an_independent_assignment_oracle() {
    let mut comparisons = 0u64;
    for n in 0..=3u64 {
        let mut choices = Vec::new();
        for start in 0..=n {
            for end in start..=n {
                for chunks in 0..=n + 1 { choices.push(p(start, end, chunks)); }
            }
        }
        let mut blocks = Vec::new();
        block_sets(n, 0, &mut Vec::new(), &mut blocks);
        for mask in 0..(1u64 << n) {
            let metadata: Vec<u64> = (0..n).filter(|i| mask & (1u64 << i) != 0).collect();
            for spu in &blocks {
                for &a in &choices {
                    for &b in &choices {
                        let ppu = [a, b];
                        let expected = brute_force(n, &metadata, &ppu, spu);
                        let actual = verify(n, &metadata, &ppu, spu);
                        assert!(!matches!(actual, Err(Error::Limit(_))), "tiny fixture hit default limits");
                        assert_eq!(actual.is_ok(), expected,
                            "N={n} metadata={metadata:?} ppu={ppu:?} spu={spu:?}; result={actual:?}");
                        comparisons += 1;
                    }
                }
            }
        }
    }
    assert!(comparisons > 100_000, "the exhaustive matrix must not silently shrink");
}
