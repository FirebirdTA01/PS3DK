//! Capture RSX frame S2: the fail-closed round-trip checker.  For every `<name>.prof` in a directory it loads the
//! file with the committed reader (`load`, `rsx_captures`) and compares each capture against `<name>.expect`:
//!
//!   exit <n>             (the reader CLI's exit code; checked by the driver, accepted here)
//!   cap <n> = <line>     the capture's state line, exactly (`RsxCapture::state_line`)
//!   cap <n> ~ <STATE>    the state word only; `has <n> <text>` lines then require each text inside the state line
//!   draws <n> <k>        draw pairs attributed to cap n
//!   ckpts <n> <k>        checkpoints received for cap n
//!   nocap <n>            the file holds no capture for cap n
//!   anomalies <k>        the file's anomaly count
//!   kinds <k>            genuine draw-kind descriptors in the file
//!
//! Fail-closed: a missing .expect, an unknown directive, a capture the .expect does not name, a named capture the
//! file lacks, or any difference is a FAIL.  Every capture is also checked for the R9 / R26 assertions (no BEGIN
//! frame-token reason, no END identity mismatch), and every reserved marker descriptor's tag must re-parse and
//! re-format to the same bytes (`parse_tag` / `format_tag`).
//!
//! usage: rsxcap_check <dir>
use std::collections::{BTreeMap, BTreeSet};
use std::path::Path;

use ps3dk_prof::analysis::{load, Outcome};
use ps3dk_prof::rsx_capture::*;

fn check_file(prof: &Path) -> Vec<String> {
    let mut bad = Vec::new();
    let exp = prof.with_extension("expect");
    let Ok(text) = std::fs::read_to_string(&exp) else { return vec!["missing .expect".into()] };
    let bytes = match std::fs::read(prof) {
        Ok(b) => b,
        Err(e) => return vec![format!("unreadable: {e}")],
    };
    let a = match load(&bytes) {
        Outcome::Analysed(a) => a,
        Outcome::Malformed(m) => return vec![format!("MALFORMED: {m:?}")],
    };
    let r = rsx_captures(&a);
    let mut named: BTreeSet<u32> = BTreeSet::new();
    let mut has: BTreeMap<u32, Vec<String>> = BTreeMap::new();
    let mut approx: BTreeMap<u32, String> = BTreeMap::new();
    let mut anomalies_seen = false;
    let mut anycaps = false;
    for line in text.lines() {
        let mut it = line.splitn(3, ' ');
        let (d, a1, rest) = (it.next().unwrap_or(""), it.next().unwrap_or(""), it.next().unwrap_or(""));
        let num = |s: &str| s.parse::<u64>().ok();
        match d {
            "exit" if num(a1).is_some() => {}
            "cap" => {
                let Some(c) = num(a1).map(|v| v as u32) else { bad.push(format!("bad line {line:?}")); continue };
                named.insert(c);
                let Some(cap) = r.get(c) else { bad.push(format!("cap {c}: missing from the file")); continue };
                if let Some(want) = rest.strip_prefix("= ") {
                    if cap.state_line() != want {
                        bad.push(format!("cap {c}: state {:?}, want {want:?}", cap.state_line()));
                    }
                } else if let Some(want) = rest.strip_prefix("~ ") {
                    approx.insert(c, want.to_string());
                    if cap.state.word() != want {
                        bad.push(format!("cap {c}: state {:?}, want state {want}", cap.state_line()));
                    }
                } else {
                    bad.push(format!("bad line {line:?}"));
                }
            }
            "has" => match num(a1) {
                Some(c) => has.entry(c as u32).or_default().push(rest.to_string()),
                None => bad.push(format!("bad line {line:?}")),
            },
            "draws" | "ckpts" => {
                let (Some(c), Some(k)) = (num(a1), num(rest)) else { bad.push(format!("bad line {line:?}")); continue };
                let Some(cap) = r.get(c as u32) else { continue };
                let got = if d == "draws" { cap.draw_pairs } else { cap.checkpoints.len() as u64 };
                if got != k {
                    bad.push(format!("cap {c}: {d} {got}, want {k}"));
                }
            }
            "nocap" => match num(a1) {
                Some(c) => {
                    named.insert(c as u32);
                    if r.get(c as u32).is_some() || r.not_analysed.contains(&(c as u32)) {
                        bad.push(format!("cap {c}: present, want none"));
                    }
                }
                None => bad.push(format!("bad line {line:?}")),
            },
            "anomalies" => match num(a1) {
                Some(k) => {
                    anomalies_seen = true;
                    if r.anomaly_count() != k {
                        let list: Vec<String> = r.anomalies.iter().map(|x| x.to_string()).collect();
                        bad.push(format!("anomalies {}, want {k}: {}", r.anomaly_count(), list.join(" | ")));
                    }
                }
                None => bad.push(format!("bad line {line:?}")),
            },
            "kinds" => match num(a1) {
                Some(k) => {
                    let n = a
                        .capture
                        .descriptors
                        .values()
                        .filter(|x| matches!(descriptor_class(&x.name, &x.logical_tag), DescClass::Draw(_)))
                        .count() as u64;
                    if n != k {
                        bad.push(format!("kinds {n}, want {k}"));
                    }
                }
                None => bad.push(format!("bad line {line:?}")),
            },
            "anycaps" => anycaps = true, /* a stress file: its captures need not be named */
            "" => {}
            _ => bad.push(format!("unknown directive {line:?}")),
        }
    }
    if !anomalies_seen {
        bad.push("no anomalies line".into());
    }
    for (c, texts) in &has {
        if !approx.contains_key(c) {
            bad.push(format!("has {c} without a cap ~ line"));
        }
        if let Some(cap) = r.get(*c) {
            for t in texts {
                if !cap.state_line().contains(t.as_str()) {
                    bad.push(format!("cap {c}: {:?} lacks {t:?}", cap.state_line()));
                }
            }
        }
    }
    for cap in &r.captures {
        if !anycaps && !named.contains(&cap.cap) {
            bad.push(format!("cap {}: in the file ({}), not in the .expect", cap.cap, cap.state_line()));
        }
        // rd.frame_token (R9) and rd.identity (R26) over every capture
        if cap.reasons.iter().any(|x| matches!(x, Reason::FrameToken { .. })) {
            bad.push(format!("cap {}: FrameToken reason", cap.cap));
        }
    }
    for x in &r.anomalies {
        if matches!(x.kind, AnomalyKind::IdentityMismatch) {
            bad.push(format!("IdentityMismatch: {x}"));
        }
        if matches!(x.kind, AnomalyKind::DuplicateCap { .. }) {
            bad.push(format!("DuplicateCap: {x}"));
        }
    }
    for c in &r.not_analysed {
        if !anycaps && !named.contains(c) {
            bad.push(format!("cap {c}: listed, not in the .expect"));
        }
    }
    // ct.reader_parse: every reserved marker tag re-parses and re-formats byte-identically
    for d in a.capture.descriptors.values().filter(|d| d.file == "<ps3dk-rsxcap>") {
        if let DescClass::Marker(k) = descriptor_class(&d.name, &d.logical_tag) {
            match parse_tag(k, &d.logical_tag) {
                Ok(t) if format_tag(&t) == d.logical_tag => {}
                Ok(t) => bad.push(format!("{}: format_tag {:?} != {:?}", d.name, format_tag(&t), d.logical_tag)),
                Err(e) => bad.push(format!("{} {:?}: does not parse ({e})", d.name, d.logical_tag)),
            }
        }
    }
    bad
}

fn main() {
    let dir = std::env::args().nth(1).expect("usage: rsxcap_check <dir>");
    let mut files: Vec<_> = std::fs::read_dir(&dir)
        .expect("dir")
        .filter_map(|e| e.ok().map(|e| e.path()))
        .filter(|p| p.extension().is_some_and(|x| x == "prof"))
        .collect();
    files.sort();
    let (mut pass, mut fail, mut markers) = (0, 0, 0usize);
    for f in &files {
        let bad = check_file(f);
        let name = f.file_name().unwrap().to_string_lossy();
        if bad.is_empty() {
            pass += 1;
            println!("CHECK {name} PASS");
        } else {
            fail += 1;
            for b in bad {
                println!("CHECK {name} FAIL: {b}");
            }
        }
        if let Ok(b) = std::fs::read(f) {
            if let Outcome::Analysed(a) = load(&b) {
                markers += a
                    .capture
                    .descriptors
                    .values()
                    .filter(|d| matches!(descriptor_class(&d.name, &d.logical_tag), DescClass::Marker(_)))
                    .count();
            }
        }
    }
    println!("CHECKER {} files: {pass} pass, {fail} fail; {markers} marker tags re-parsed", files.len());
    if files.is_empty() || fail > 0 {
        std::process::exit(1);
    }
}
