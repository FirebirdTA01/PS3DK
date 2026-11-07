//! The RSX joint set and hold-out on JSON cases: the input of the independent exact checker
//! tests/rsx_measured_check.py (rsx-measured-slice-design.md 8.7).
//!
//! usage: rsx_joint_cases < cases.json > results.json   (exit 0; 2 unreadable or malformed input)
//! Input:  {"cases": [{"obs": [["f", "lo", "hi"], ...], "halves": [["f", "lo"|"hi", "T"], ...]}, ...]}
//! Output: {"results": [{"f0": state, "fj": state, "fj_slope": [lo, lo_open, hi|null] | null,
//!   "hold": [[contradicted, decisive], ...] | null, "split": {"na": class} | {"kept": k, "held": [[ordinal,
//!   contradicted, decisive, lo, lo_open, hi, hi_open], ...]}}]}: the hold-out is evaluated only when F_0 is
//!   COMPATIBLE with at least 2 distinct bracket coordinates (rsx_measured's rule); H-SPLIT ([`split_holdout`], the
//!   reader's own function) on the brackets in the order given.  Rationals in lowest terms, "n" or "n/d"; an infinite
//!   end is "-inf" / "+inf".
use std::io::Read;

use ps3dk_prof::clockmap::{reduce, solve, solve_with, Half, Meter, Obs, Rat, Solved};
use ps3dk_prof::clockmap::End;
use ps3dk_prof::rsx_measured::{classify_constraint, split_holdout, SplitOutcome};
use serde_json::{json, Value};

fn num(v: &Value) -> Option<i128> {
    v.as_str()?.parse().ok()
}

fn rat(r: &Rat) -> String {
    reduce(&mut Meter::unlimited(), r).expect("unlimited").to_string()
}

fn state(s: &Solved) -> &'static str {
    match s {
        Solved::NoObservations => "NO-OBSERVATIONS",
        Solved::Inconsistent => "INCONSISTENT",
        Solved::Compatible(_) => "COMPATIBLE",
    }
}

fn end(e: &End) -> (String, bool) {
    match e {
        End::NegInf => ("-inf".into(), false),
        End::PosInf => ("+inf".into(), false),
        End::At { value, open } => (rat(value), *open),
    }
}

fn case(c: &Value) -> Option<Value> {
    let obs: Vec<Obs> = c["obs"]
        .as_array()?
        .iter()
        .map(|t| Some(Obs { f: num(&t[0])?, lo: num(&t[1])?, hi: num(&t[2])? }))
        .collect::<Option<_>>()?;
    let halves: Vec<Half> = c["halves"]
        .as_array()?
        .iter()
        .map(|t| {
            let f = num(&t[0])?;
            let v = num(&t[2])?;
            match t[1].as_str()? {
                "lo" => Some(Half { f, lo: Some(v), hi: None }),
                "hi" => Some(Half { f, lo: None, hi: Some(v) }),
                _ => None,
            }
        })
        .collect::<Option<_>>()?;
    let m = &mut Meter::unlimited();
    let f0 = solve(m, &obs).ok()?;
    let fj = solve_with(m, &obs, &halves).ok()?;
    let fj_slope = match &fj {
        Solved::Compatible(mj) => json!([rat(&mj.slope_lo), mj.slope_lo_open(), mj.slope_hi.as_ref().map(rat)]),
        _ => Value::Null,
    };
    let hold = match &f0 {
        Solved::Compatible(m0) if m0.distinct_f >= 2 => {
            let mut v = Vec::new();
            for h in &halves {
                let p = m0.project(m, h.f).ok()?;
                let (lower, t) = match (h.lo, h.hi) {
                    (Some(t), None) => (true, t),
                    (None, Some(t)) => (false, t),
                    _ => return None,
                };
                let (c, d) = classify_constraint(&p.lo, &p.hi, t, lower);
                v.push(json!([c, d]));
            }
            Value::Array(v)
        }
        _ => Value::Null,
    };
    let split = match split_holdout(m, &obs, false).ok()? {
        SplitOutcome::NotAvailable(why) => json!({ "na": why }),
        SplitOutcome::Checked { kept, held } => {
            let v: Vec<Value> = held
                .iter()
                .map(|h| {
                    let (lo, lo_open) = end(&h.lo);
                    let (hi, hi_open) = end(&h.hi);
                    json!([h.ordinal, h.contradicted, h.decisive, lo, lo_open, hi, hi_open])
                })
                .collect();
            json!({ "kept": kept, "held": v })
        }
    };
    Some(json!({"f0": state(&f0), "fj": state(&fj), "fj_slope": fj_slope, "hold": hold, "split": split}))
}

fn main() {
    let mut s = String::new();
    if std::io::stdin().read_to_string(&mut s).is_err() {
        std::process::exit(2);
    }
    let Ok(v) = serde_json::from_str::<Value>(&s) else { std::process::exit(2) };
    let Some(cases) = v["cases"].as_array() else { std::process::exit(2) };
    let mut out = Vec::with_capacity(cases.len());
    for c in cases {
        match case(c) {
            Some(r) => out.push(r),
            None => std::process::exit(2),
        }
    }
    println!("{}", json!({ "results": out }));
}
