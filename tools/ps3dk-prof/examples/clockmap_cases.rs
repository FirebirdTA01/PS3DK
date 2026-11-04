//! The pure clockmap solver on JSON cases: the input of the independent exact checker tests/clockmap_check.py.
//!
//! usage: clockmap_cases < cases.json > results.json   (exit 0; 2 unreadable or malformed input)
//!
//! Input:  {"cases": [{"obs": [["f", "lo", "hi"], ...], "q": [["q", "count"], ...]}, ...]}  (decimal strings)
//! Output: {"results": [...]}, one per case: {"state": "NO-OBSERVATIONS" | "INCONSISTENT" | "COMPATIBLE" |
//!   "UNSUPPORTED:<limit>", and for COMPATIBLE "slope_lo", "slope_lo_open", "slope_hi" (null: +inf), "proj": [{"q",
//!   "lo", "lo_open", "hi", "hi_open", "interp"}] ("-inf" / "+inf" for an infinite end) and "summary"}.  Every
//!   rational is printed in lowest terms as "n" or "n/d".
use std::collections::BTreeMap;
use std::io::Read;

use ps3dk_prof::clockmap::{reduce, solve, summarize, End, Meter, Obs, Rat, Solved};
use ps3dk_prof::durations::duration_bound;
use serde_json::{json, Value};

fn num(v: &Value) -> Option<i128> {
    v.as_str()?.parse().ok()
}

fn rat(r: &Rat) -> String {
    reduce(&mut Meter::unlimited(), r).expect("unlimited").to_string()
}

fn end(e: &End) -> (Value, bool) {
    match e {
        End::NegInf => (json!("-inf"), false),
        End::PosInf => (json!("+inf"), false),
        End::At { value, open } => (json!(rat(value)), *open),
    }
}

fn case(c: &Value) -> Option<Value> {
    let obs: Vec<Obs> = c["obs"]
        .as_array()?
        .iter()
        .map(|t| Some(Obs { f: num(&t[0])?, lo: num(&t[1])?, hi: num(&t[2])? }))
        .collect::<Option<_>>()?;
    let qs: Vec<(i128, u64)> = c["q"]
        .as_array()?
        .iter()
        .map(|t| Some((num(&t[0])?, u64::try_from(num(&t[1])?).ok()?)))
        .collect::<Option<_>>()?;
    let model = match solve(&mut Meter::unlimited(), &obs) {
        Err(l) => return Some(json!({"state": format!("UNSUPPORTED:{}", l.name())})),
        Ok(Solved::NoObservations) => return Some(json!({"state": "NO-OBSERVATIONS"})),
        Ok(Solved::Inconsistent) => return Some(json!({"state": "INCONSISTENT"})),
        Ok(Solved::Compatible(m)) => m,
    };
    let mut proj = Vec::new();
    let mut counts: BTreeMap<i128, u64> = BTreeMap::new();
    for &(q, n) in &qs {
        *counts.entry(q).or_default() += n;
        match model.project(&mut Meter::unlimited(), q) {
            Ok(p) => {
                let (lo, lo_open) = end(&p.lo);
                let (hi, hi_open) = end(&p.hi);
                proj.push(json!({"q": q.to_string(), "lo": lo, "lo_open": lo_open, "hi": hi, "hi_open": hi_open,
                                 "interp": p.interpolated}));
            }
            Err(l) => proj.push(json!({"q": q.to_string(), "error": l.name()})),
        }
    }
    let summary = match summarize(&mut Meter::unlimited(), &model, &counts) {
        Ok(s) => json!({
            "events": s.events, "interpolated": s.interpolated, "extrapolated": s.extrapolated,
            "bounded": s.bounded, "half_unbounded": s.half_unbounded, "unbounded": s.unbounded,
            "open_bound": s.open_bound,
            "width_max": s.width_max.as_ref().map(rat), "width_median": s.width_median.as_ref().map(rat),
        }),
        Err(l) => json!({"error": l.name()}),
    };
    let mut dur = Vec::new();
    for t in c["x"].as_array().map(Vec::as_slice).unwrap_or(&[]) {
        let x: u128 = t.as_str()?.parse().ok()?;
        let b = duration_bound(
            &mut Meter::unlimited(),
            &model.slope_lo,
            model.slope_lo_open(),
            model.slope_hi.as_ref(),
            x,
        )
        .ok()?;
        dur.push(json!({"x": x.to_string(), "lo": rat(&b.lo), "lo_open": b.lo_open,
                        "hi": b.hi.as_ref().map_or("+inf".to_string(), rat)}));
    }
    Some(json!({
        "dur": dur,
        "state": "COMPATIBLE",
        "slope_lo": rat(&model.slope_lo),
        "slope_lo_open": model.slope_lo_open(),
        "slope_hi": model.slope_hi.as_ref().map(rat),
        "proj": proj,
        "summary": summary,
    }))
}

fn main() {
    let mut text = String::new();
    if std::io::stdin().read_to_string(&mut text).is_err() {
        eprintln!("clockmap_cases: unreadable input");
        std::process::exit(2);
    }
    let Ok(v) = serde_json::from_str::<Value>(&text) else {
        eprintln!("clockmap_cases: input is not JSON");
        std::process::exit(2);
    };
    let Some(cases) = v["cases"].as_array() else {
        eprintln!("clockmap_cases: no cases array");
        std::process::exit(2);
    };
    let mut results = Vec::with_capacity(cases.len());
    for (i, c) in cases.iter().enumerate() {
        match case(c) {
            Some(r) => results.push(r),
            None => {
                eprintln!("clockmap_cases: case {i} is malformed");
                std::process::exit(2);
            }
        }
    }
    println!("{}", json!({ "results": results }));
}
