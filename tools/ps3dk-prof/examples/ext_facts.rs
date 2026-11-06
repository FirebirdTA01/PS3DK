//! Machine-readable facts of a capture's extension sources, from the reader's own structures (no report text): the
//! input of the E1 probe's acceptance checker (samples/profiling/ext-probe/host-tests/check_e1_capture.py).
//!
//! usage: ext_facts <file.prof>   (exit 0 facts printed; 1 MALFORMED (one `file` line); 2 usage/unreadable)
//!
//! Lines (space-separated key=value; names never contain whitespace or '=', else '?' replaces those bytes):
//!   E1FACTS 1
//!   file malformed=0 verdict=<complete|incomplete> incomplete=<n> ext=<0|1>
//!   source kind=<lane|gpu> id=<id> domain=<d> structure=<complete|incomplete> raw_valid=<0|1> final=<state|none>
//!     events=<n> lost=<n> losses=<n> epochs=<n> paired=<n> marks=<n> unpaired=<n> censored=<n>
//!     frames_resolved=<n> frames_unresolved=<n> frames_none=<n>
//!   events kind=<lane|gpu> id=<id> epoch=<e> ev=<begin|end|mark> desc=<name> count=<n>
//!   epochrec domain=<d> epoch=<e> reason=<r> written=<16 hex>
//!   variation id=<lane id> epoch=<e> state=<insufficient|static|moving>
//!   end
use std::collections::BTreeMap;

use ps3dk_prof::analysis::{load, Outcome};
use ps3dk_prof::ext::{counter_variation, CounterVariation, SourceKey, Structure, EV_BEGIN, EV_END, EV_MARK};
use ps3dk_prof::Verdict;

fn clean(s: &str) -> String {
    if s.is_empty() {
        return "?".to_string();
    }
    s.chars().map(|c| if c.is_whitespace() || c == '=' || !c.is_ascii() { '?' } else { c }).collect()
}

fn main() {
    let args: Vec<String> = std::env::args().collect();
    if args.len() != 2 {
        eprintln!("usage: ext_facts <file.prof>");
        std::process::exit(2);
    }
    let bytes = match std::fs::read(&args[1]) {
        Ok(b) => b,
        Err(e) => {
            eprintln!("{}: {e}", args[1]);
            std::process::exit(2);
        }
    };
    println!("E1FACTS 1");
    let a = match load(&bytes) {
        Outcome::Malformed(m) => {
            println!("file malformed=1 rule={}", clean(&format!("{:?}", m.rule)));
            std::process::exit(1);
        }
        Outcome::Analysed(a) => a,
    };
    let (verdict, n_inc) = match &a.verdict {
        Verdict::Complete => ("complete", 0),
        Verdict::Incomplete(v) => ("incomplete", v.len()),
    };
    let cap = &a.capture;
    println!("file malformed=0 verdict={verdict} incomplete={n_inc} ext={}", u8::from(cap.ext.is_some()));
    if let Some(x) = &cap.ext {
        for s in &x.sources {
            let (kind, id) = match s.key {
                SourceKey::Lane(id) => ("lane", id),
                SourceKey::Gpu(id) => ("gpu", id),
            };
            let fin = s.final_state.map_or("none".to_string(), |f| f.to_string());
            println!(
                "source kind={kind} id={id} domain={} structure={} raw_valid={} final={fin} events={} lost={} \
                 losses={} epochs={} paired={} marks={} unpaired={} censored={} frames_resolved={} \
                 frames_unresolved={} frames_none={}",
                s.domain_id,
                if s.structure == Structure::Complete { "complete" } else { "incomplete" },
                u8::from(s.raw_valid),
                s.events_total,
                s.lost_total,
                s.raw.losses.len(),
                s.epochs,
                s.paired(),
                s.marks(),
                s.unpaired(),
                s.censored(),
                s.frames_resolved,
                s.frames_unresolved,
                s.frames_none
            );
            // per epoch, kind and descriptor NAME (SPU: the lane's image table; GPU: the v1 descriptor table)
            let mut counts: BTreeMap<(u32, u8, String), u64> = BTreeMap::new();
            for ev in &s.raw.events {
                let name = match s.key {
                    SourceKey::Lane(lid) => x
                        .lanes
                        .get(&lid)
                        .and_then(|l| x.images.get(&l.image_index))
                        .and_then(|im| im.descriptors.iter().find(|d| d.index == ev.desc))
                        .map_or("?".to_string(), |d| clean(&d.name)),
                    SourceKey::Gpu(_) => cap.descriptors.get(&ev.desc).map_or("?".to_string(), |d| clean(&d.name)),
                };
                *counts.entry((ev.epoch, ev.kind, name)).or_insert(0) += 1;
            }
            for ((epoch, k, name), n) in counts {
                let ev = match k {
                    EV_BEGIN => "begin",
                    EV_END => "end",
                    EV_MARK => "mark",
                    _ => "?",
                };
                println!("events kind={kind} id={id} epoch={epoch} ev={ev} desc={name} count={n}");
            }
            if let SourceKey::Lane(lid) = s.key {
                if let Some(v) = counter_variation(x, s) {
                    for e in v {
                        let st = match e.state {
                            CounterVariation::Insufficient => "insufficient",
                            CounterVariation::Static => "static",
                            CounterVariation::Moving => "moving",
                        };
                        println!("variation id={lid} epoch={} state={st}", e.epoch);
                    }
                }
            }
        }
        for e in &x.epochs {
            println!("epochrec domain={} epoch={} reason={} written={:016x}", e.domain_id, e.epoch, e.reason, e.written_value);
        }
    }
    println!("end");
}
