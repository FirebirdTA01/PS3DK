//! The reader's SPU ticks result as JSON: the input of the independent checker tests/spu_ticks_check.py
//! (spu-ticks slice design 10.8).  The compiled-in table is used (no profile is reviewed), so no segment is
//! TICKS[profile]; the checker compares the bounds, the steps, the segments, the tick counts and the counts.
//!
//! usage: spu_ticks_json FILE...   (one JSON object per line; exit 0, 3 on an unreadable file)
//! Per file: {"file", "malformed": bool, "lanes": [{"lane", "state", "obs": [[kind "e"|"b", epoch, seq|bseq, raw, L,
//! U1, U2, U, segment]], "segs": [[epoch, first, last, steps_proven, unavailable reasons | null, end]], "pairs":
//! [[begin, end, ticks | null, class | null, BEGIN state, END state]], "d": n, "oc": n}], "cov": [Q, q_t, q_p]}.
use ps3dk_prof::spu_ticks::{analyze, Boundary, LaneState, ObsKind, Options, TickState};
use ps3dk_prof::{load, Outcome};
use serde_json::{json, Value};

fn opt(v: Option<u64>) -> Value {
    v.map_or(Value::Null, |v| json!(v))
}

fn main() {
    let mut rc = 0;
    for path in std::env::args().skip(1) {
        let Ok(bytes) = std::fs::read(&path) else {
            eprintln!("spu_ticks_json: {path}: unreadable");
            rc = 3;
            continue;
        };
        let a = match load(&bytes) {
            Outcome::Malformed(_) => {
                println!("{}", json!({"file": path, "malformed": true}));
                continue;
            }
            Outcome::Analysed(a) => a,
        };
        let Some(r) = analyze(&a, &Options::default()) else {
            println!("{}", json!({"file": path, "malformed": false, "lanes": [], "cov": [0, 0, 0]}));
            continue;
        };
        let lanes: Vec<Value> = r
            .lanes
            .iter()
            .map(|u| {
                let obs: Vec<Value> = u
                    .obs
                    .iter()
                    .map(|o| {
                        let (k, s) = match o.kind {
                            ObsKind::Event { seq, .. } => ("e", seq),
                            ObsKind::Bracket { bseq, .. } => ("b", bseq),
                        };
                        json!([k, o.epoch, s, o.raw, opt(o.l), opt(o.u1), opt(o.u2), opt(o.u), o.seg])
                    })
                    .collect();
                let segs: Vec<Value> = u
                    .segments
                    .iter()
                    .map(|s| {
                        let un = match &s.state {
                            TickState::Unavailable(w) => json!(w),
                            _ => Value::Null,
                        };
                        let end = match s.end {
                            Boundary::Step(c) => c.name(),
                            b => b.name(),
                        };
                        json!([s.epoch, s.first, s.last, s.steps_proven, un, end])
                    })
                    .collect();
                let pairs: Vec<Value> = u
                    .pairs_out
                    .iter()
                    .map(|p| {
                        json!([
                            p.begin,
                            p.end,
                            p.ticks.map_or(Value::Null, |t| json!(t.to_string())),
                            p.tick_class,
                            p.place_b.state(),
                            p.place_e.state()
                        ])
                    })
                    .collect();
                let state = match &u.state {
                    LaneState::Evaluated => "EVALUATED",
                    LaneState::Empty => "EMPTY",
                    LaneState::EmptyUnproven(_) => "EMPTY-UNPROVEN",
                    LaneState::Unsupported(_) => "UNSUPPORTED",
                    LaneState::Refused(_) => "REFUSED",
                    LaneState::NoOrder => "NO-ORDER",
                };
                json!({"lane": u.lane_id, "state": state, "obs": obs, "segs": segs, "pairs": pairs,
                       "d": u.d_contradicted, "oc": u.order_contradicted})
            })
            .collect();
        let c = &r.coverage;
        println!(
            "{}",
            json!({"file": path, "malformed": false, "order": r.order_present, "lanes": lanes,
                   "cov": [c.total, c.q_t, c.q_p]})
        );
    }
    std::process::exit(rc);
}
