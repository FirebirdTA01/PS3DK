// Test-only oracle for the SPU lane transport model, fail-closed.  For each <name>.prof with a <name>.expect:
// the committed reader's verdict and per-source STRUCTURE / RAW VALIDITY / FINAL state / counts / loss ranges must
// equal the expectation, and every decoded event must be the producer's event at that sequence number.
//
// <name>.truth is REQUIRED.  Per lane it starts with "#lane ID base B count C export E": sequence numbers below B were
// injected as already lost (they may never decode as events); [B, B+E) are listed exactly once each; [B+E, B+C) exist
// but are beyond the export bound (the model's own full C stream check covers them); B+C and above were never
// assigned.  A missing file, a lane without a header, a duplicate or out-of-range row, an unknown expectation
// directive or an expectation without a verdict is an error.  Exit 1 on any error.
use ps3dk_prof::ext::{SourceKey, SourceReport, Structure};
use ps3dk_prof::{load, Outcome, Verdict};
use std::collections::HashMap;

struct Meta {
    base: u64,
    count: u64,
    export: u64,
}

type Truth = HashMap<(u32, u64), (u64, u32, u8, u8)>;

fn main() {
    let dir = std::env::args().nth(1).expect("dir");
    let mut names: Vec<_> = std::fs::read_dir(&dir)
        .expect("read dir")
        .filter_map(|e| e.ok())
        .map(|e| e.path())
        .filter(|p| p.extension().is_some_and(|x| x == "expect"))
        .collect();
    names.sort();
    let (mut pass, mut fail) = (0, 0);
    for exp in &names {
        let stem = exp.with_extension("");
        let name = stem.file_name().unwrap().to_string_lossy().to_string();
        match check(&stem) {
            Ok(()) => pass += 1,
            Err(e) => {
                fail += 1;
                println!("FAIL {name}: {e}");
            }
        }
    }
    println!("oracle files={} pass={pass} fail={fail}", names.len());
    std::process::exit(if fail == 0 && pass > 0 { 0 } else { 1 });
}

fn read_truth(stem: &std::path::Path) -> Result<(Truth, HashMap<u32, Meta>), String> {
    let path = stem.with_extension("truth");
    let text = std::fs::read_to_string(&path).map_err(|e| format!("truth file {}: {e}", path.display()))?;
    let (mut truth, mut meta) = (Truth::new(), HashMap::new());
    for (n, line) in text.lines().enumerate() {
        let v: Vec<&str> = line.split_whitespace().collect();
        if v.first() == Some(&"#lane") {
            if v.len() != 8 || v[2] != "base" || v[4] != "count" || v[6] != "export" {
                return Err(format!("truth line {}: bad header {line:?}", n + 1));
            }
            let id: u32 = v[1].parse().map_err(|_| format!("truth line {}: lane id", n + 1))?;
            let num = |s: &str| s.parse::<u64>().map_err(|_| format!("truth line {}: number {s:?}", n + 1));
            let m = Meta { base: num(v[3])?, count: num(v[5])?, export: num(v[7])? };
            if m.export > m.count || meta.insert(id, m).is_some() {
                return Err(format!("truth line {}: bad or repeated header for lane {id}", n + 1));
            }
            continue;
        }
        let x: Vec<u64> = v.iter().map(|s| s.parse().map_err(|_| format!("truth line {}: {s:?}", n + 1))).collect::<Result<_, _>>()?;
        if x.len() != 6 {
            return Err(format!("truth line {}: {} fields", n + 1, x.len()));
        }
        let id = x[0] as u32;
        let m = meta.get(&id).ok_or(format!("truth line {}: lane {id} has no header", n + 1))?;
        if x[1] < m.base || x[1] - m.base >= m.export {
            return Err(format!("truth line {}: seq {} outside the exported range", n + 1, x[1]));
        }
        if truth.insert((id, x[1]), (x[2], x[3] as u32, x[4] as u8, x[5] as u8)).is_some() {
            return Err(format!("truth line {}: duplicate seq {} for lane {id}", n + 1, x[1]));
        }
    }
    for (id, m) in &meta {
        let listed = truth.keys().filter(|(l, _)| l == id).count() as u64;
        if listed != m.export {
            return Err(format!("truth: lane {id} lists {listed} rows, header says export {}", m.export));
        }
    }
    Ok((truth, meta))
}

fn source<'a>(a: &'a ps3dk_prof::Analysis, id: u32) -> Result<&'a SourceReport, String> {
    a.capture
        .ext
        .as_ref()
        .and_then(|x| x.sources.iter().find(|s| s.key == SourceKey::Lane(id)))
        .ok_or(format!("no source for lane {id}"))
}

fn check(stem: &std::path::Path) -> Result<(), String> {
    let bytes = std::fs::read(stem.with_extension("prof")).map_err(|e| e.to_string())?;
    let expect = std::fs::read_to_string(stem.with_extension("expect")).map_err(|e| e.to_string())?;
    let (truth, meta) = read_truth(stem)?;
    let outcome = load(&bytes);
    let mut verdict_seen = false;
    for line in expect.lines() {
        let f: Vec<&str> = line.split_whitespace().collect();
        match f.first().copied() {
            None | Some("exit") | Some("smoke-exit") => {} // the driver checks exit codes
            Some("verdict") => {
                verdict_seen = true;
                match (&outcome, f.get(1).copied().unwrap_or("")) {
                    (Outcome::Malformed(m), "malformed") => {
                        let want = f[2..].join(" ");
                        if !m.detail.contains(&want) {
                            return Err(format!("malformed detail {:?} lacks {want:?}", m.detail));
                        }
                        return Ok(());
                    }
                    (Outcome::Analysed(a), "complete") if a.verdict == Verdict::Complete => {}
                    (Outcome::Analysed(a), "incomplete") if matches!(a.verdict, Verdict::Incomplete(_)) => {}
                    (o, w) => return Err(format!("verdict: want {w}, got {}", short(o))),
                }
            }
            Some("lane") => {
                let a = analysed(&outcome)?;
                let id: u32 = f[1].parse().map_err(|_| "lane id".to_string())?;
                let s = source(a, id)?;
                let structure = if s.structure == Structure::Complete { "COMPLETE" } else { "INCOMPLETE" };
                let got = format!(
                    "{structure} {} {} {} {}",
                    s.raw_valid as u8,
                    s.final_state.unwrap_or(0),
                    s.events_total,
                    s.lost_total
                );
                let want = f[2..].join(" ");
                if got != want {
                    return Err(format!("lane {id}: got {got}, want {want}"));
                }
                check_truth(s, id, &truth, &meta)?;
            }
            Some("truth") => {
                let a = analysed(&outcome)?;
                let id: u32 = f[1].parse().map_err(|_| "lane id".to_string())?;
                check_truth(source(a, id)?, id, &truth, &meta)?;
            }
            Some("losses") => {
                let a = analysed(&outcome)?;
                let id: u32 = f[1].parse().map_err(|_| "lane id".to_string())?;
                let s = source(a, id)?;
                let got: Vec<String> = s.raw.losses.iter().map(|l| format!("{}:{}", l.first, l.count)).collect();
                let want: Vec<String> = f[2..].iter().map(|x| x.to_string()).collect();
                if got != want {
                    return Err(format!("lane {id} losses: got {got:?}, want {want:?}"));
                }
            }
            Some(k @ ("sources" | "brackets" | "epochs")) => {
                let a = analysed(&outcome)?;
                let x = a.capture.ext.as_ref();
                let n = match k {
                    "sources" => x.map_or(0, |x| x.sources.len()),
                    "brackets" => x.map_or(0, |x| x.brackets.len()),
                    _ => x.map_or(0, |x| x.epochs.len()),
                };
                if n.to_string() != f[1] {
                    return Err(format!("{k}: got {n}, want {}", f[1]));
                }
            }
            Some(other) => return Err(format!("unknown expectation directive {other:?}")),
        }
    }
    if !verdict_seen {
        return Err("the expectation has no verdict".to_string());
    }
    Ok(())
}

/// Every decoded event is the producer's event at its sequence number; every number below the emitted end (within
/// the exported range) is either a decoded event or inside a LOSS range.
fn check_truth(s: &SourceReport, id: u32, truth: &Truth, meta: &HashMap<u32, Meta>) -> Result<(), String> {
    let m = meta.get(&id).ok_or(format!("lane {id}: no truth header"))?;
    let mut seen = std::collections::HashSet::new();
    for ev in &s.raw.events {
        if ev.seq < m.base {
            return Err(format!("lane {id}: seq {} was injected as lost but decodes as an event", ev.seq));
        }
        let off = ev.seq - m.base;
        if off >= m.count {
            return Err(format!("lane {id}: decoded seq {} the producer never assigned", ev.seq));
        }
        if off < m.export {
            let &(dec, desc, kind, depth) =
                truth.get(&(id, ev.seq)).ok_or(format!("lane {id}: seq {} has no truth row", ev.seq))?;
            if (ev.raw, ev.desc, ev.kind, ev.depth) != (dec, desc, kind, depth) {
                return Err(format!("lane {id} seq {}: decoded {ev:?}, producer {:?}", ev.seq, (dec, desc, kind, depth)));
            }
        }
        seen.insert(ev.seq);
    }
    let end = (s.events_total + s.lost_total).min(m.base.saturating_add(m.export));
    let covered = |q: u64| seen.contains(&q) || s.raw.losses.iter().any(|l| q >= l.first && q - l.first < l.count);
    for q in m.base.max(end.saturating_sub(4096))..end {
        if !covered(q) {
            return Err(format!("lane {id}: seq {q} below the emitted end is neither an event nor lost"));
        }
    }
    if m.base > 0 && !covered(0) && end > 0 {
        return Err(format!("lane {id}: injected seq 0 is not inside a LOSS range"));
    }
    Ok(())
}

fn analysed(o: &Outcome) -> Result<&ps3dk_prof::Analysis, String> {
    match o {
        Outcome::Analysed(a) => Ok(a),
        Outcome::Malformed(m) => Err(format!("MALFORMED: {m}")),
    }
}

fn short(o: &Outcome) -> String {
    match o {
        Outcome::Analysed(a) => format!("{:?}", a.verdict),
        Outcome::Malformed(m) => format!("MALFORMED {m}"),
    }
}
