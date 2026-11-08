//! RSX MEASURED work bound, H-SPLIT margin distribution: the sort of each pass's margins is charged before it runs
//! (rsx-measured-holdout-delta.md, the publication of a pass).  The window of budgets that stop at the sort is its
//! charge, derived here from the pass's held-out count alone.

mod common;
mod rsxm_support;

use common::*;
use ps3dk_prof::rsx_measured::{analyze, Convention, Options, RsxResult, State};
use rsxm_support::*;

fn opts() -> Options {
    Options {
        table: test_table(),
        convention: Some(Convention { submit: "render.submit".into(), wait: "render.flip_wait".into() }),
        ..Options::default()
    }
}

const PHASES: [&str; 10] =
    ["classification", "index", "brackets", "F_0", "H-SPLIT", "X-RSX", "H-CAUSAL", "F_J", "F_J summary", "publication"];

/// The phase index where `res` stopped (None: no exhaustion).
fn stop(res: &RsxResult) -> Option<usize> {
    res.exhausted_at.as_ref().map(|e| PHASES.iter().position(|p| *p == e.phase).expect("a known phase"))
}

/// ceil(log2 n), 0 for n <= 1.
fn clog2(n: u64) -> u64 {
    if n <= 1 {
        0
    } else {
        64 - u64::from((n - 1).leading_zeros())
    }
}

#[test]
fn the_h_split_margin_sort_is_charged_before_it_runs() {
    // the baseline with 10 brackets 8000 ticks apart (+-3000 wide, admissible): the even pass holds out 2, 4, 6, 8
    let mut r = baseline();
    r.brackets = (0..10u64).map(|j| 100_000 + 8000 * j).map(|tt| (2 * tt, tt - 3000, tt + 3000)).collect();
    let a = analysed(&r.bytes());
    let o = opts();
    let full = analyze(&a, &o).unwrap();
    assert_eq!(full.units.len(), 1);
    let even = full.units[0].split.even.clone().expect("the even pass");
    assert!(even.held == 4 && even.not_available.is_none(), "{even:?}");
    assert!(even.margins.windows(2).all(|w| w[0] <= w[1]), "sorted smallest to largest");
    // the merge sort's charge on h margins: two buffers and 2 h ceil(log2 h) comparisons and copies
    let h = even.held;
    let sort = 2 * h * clog2(h) + 2 * h;
    let split = PHASES.iter().position(|p| *p == "H-SPLIT").unwrap();
    // the smallest budget that gets past H-SPLIT (the stop phase only moves later as the budget grows)
    let (mut lo, mut hi) = (0u64, full.used);
    while lo < hi {
        let mid = lo + (hi - lo) / 2;
        let r = analyze(&a, &Options { capture_ops: mid, ..o.clone() }).unwrap();
        if stop(&r).map_or(true, |s| s > split) {
            hi = mid;
        } else {
            lo = mid + 1;
        }
    }
    let pass = lo;
    // the budgets just below it stop in H-SPLIT at one and the same refused charge (the module's own charges stand
    // still: a refused charge exhausts the meter, so `used` is the budget itself): the even pass's last charge, the
    // sort
    let at = |b: u64| analyze(&a, &Options { capture_ops: b, ..o.clone() }).unwrap();
    let top = at(pass - 1);
    assert_eq!(stop(&top), Some(split));
    let mut window = 0u64;
    let mut b = pass - 1;
    loop {
        let r = at(b);
        if stop(&r) != Some(split) || r.work.own_charged != top.work.own_charged || r.work.steps != top.work.steps {
            break;
        }
        assert_eq!(r.work.after_exhaustion, 0, "budget {b}");
        assert!(r.work.steps <= r.work.own_charged, "budget {b}: {:?}", r.work);
        let u = &r.units[0];
        assert_eq!(u.state, State::Unmeasured(vec!["UNSUPPORTED-RESOURCE (capture budget) in H-SPLIT".into()]));
        assert_eq!(u.split, Default::default(), "budget {b}: atomic, no H-SPLIT result published");
        window += 1;
        if b == 0 {
            break;
        }
        b -= 1;
    }
    assert_eq!(window, sort, "the budgets refused at the margin sort are exactly its charge (h {h})");
    // at the first budget past H-SPLIT the sort was paid (and nothing more of the module's own charges, the next
    // phase's first charge being refused there), and its counted steps fit inside that charge
    let past = at(pass);
    assert_eq!(past.work.own_charged, top.work.own_charged + sort, "the sort is the last charge of H-SPLIT");
    assert!(past.work.steps - top.work.steps <= sort, "{} steps for a charge of {sort}", past.work.steps - top.work.steps);
    assert!(past.work.steps > top.work.steps + 2 * h, "the sort ran (two buffers and its moves) only after its charge");
}
