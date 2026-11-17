//! A retained-data listing, deliberately separate from whole-capture verdicts.
use ps3dk_prof::{auto_rsx, ext, fifo, model::Incomplete, rolling, rolling_combined as mixed,
    rolling_file as join, rolling_store as store};
use serde_json::{json, Value};
use std::{fs::File, io::Read, process::ExitCode};

const NOTE: &str = "Retained data only. The dropped prefix is a checkpoint claim, not authenticated history. Exit 0 validates this retained listing, not whole-run completeness or a performance verdict.";
type Result<T> = std::result::Result<T, (u8, String)>;
fn failure(code: u8, e: impl std::fmt::Debug) -> (u8, String) { (code, format!("{e:?}")) }
fn store_code(e: &store::Error) -> u8 { if matches!(e, store::Error::Budget(_)) { 1 } else { 2 } }
fn ppu_code(e: &rolling::Error) -> u8 {
    if matches!(e, rolling::Error::Unsupported { .. } | rolling::Error::Limit(_)) { 1 } else { 2 }
}
fn mixed_code(e: &mixed::Error) -> u8 {
    match e {
        mixed::Error::Unsupported { .. } | mixed::Error::Limit(_) | mixed::Error::MissingImplementation => 1,
        mixed::Error::Spu(ext::rolling::Error::Unsupported(_) | ext::rolling::Error::Limit(_)) => 1,
        mixed::Error::Auto(auto_rsx::rolling::Error::Unsupported(_) | auto_rsx::rolling::Error::Limit(_) | auto_rsx::rolling::Error::MissingImplementation) => 1,
        mixed::Error::Fifo(fifo::rolling::Error::Unsupported(_) | fifo::rolling::Error::Limit(_) | fifo::rolling::Error::Unavailable) => 1,
        _ => 2,
    }
}
fn ppu_json(p: &rolling::RetainedPpu) -> Value {
    json!({"zones": p.zones.iter().map(|z| json!({"name":z.name,"completed":z.completed,
        "left_censored":z.left_censored,"open":z.open,"total_ticks":z.total_ticks.map(|v|v.to_string())})).collect::<Vec<_>>(),
        "lanes":p.lanes.iter().map(|(id,l)|json!({"id":id,"prefix_written":l.prefix_written,
            "retained_written":l.retained_written,"retained_gap_sum":l.retained_gap_sum,
            "final_present":l.final_entry.is_some()})).collect::<Vec<_>>(),
        "incomplete":p.incomplete.iter().map(ToString::to_string).collect::<Vec<_>>()})
}
fn report(bytes: &[u8], cap: u64, analysis_cap: usize, analysis_work: u64) -> Result<Value> {
    let limits = store::Limits { max_file_bytes:cap, max_slot_bytes:cap.min(u32::MAX as u64) as u32 };
    let physical = store::read(bytes, limits).map_err(|e|failure(store_code(&e),e))?;
    let magic = physical.segments.first().ok_or_else(||failure(1,"empty store has no semantic checkpoint"))?
        .checkpoint.get(..8).ok_or_else(||failure(2,"short checkpoint"))?;
    let v = &physical.view;
    let retained_bytes = physical.segments.iter().map(|s|s.payload.len() as u64).sum::<u64>();
    let state = match v.state { 1=>"OPEN", 2=>"SEALED", 3=>"STOPPED_RESOURCE", _=>"UNSPECIFIED" };
    let mut out = json!({"schema":1,"status":"VALID_RETAINED","note":NOTE,
        "physical":{"version":v.format_version,"state":state,"reason":v.reason,"recovered":v.recovered,"generation":v.generation},
        "retained":{"first_chunk":v.first_chunk,"next_chunk":v.next_chunk,"segments":v.retained_segments,
            "payload_bytes":retained_bytes,"first_payload_byte":v.evicted_payload_bytes},
        "evicted":{"chunks":v.evicted_chunks,"payload_bytes":v.evicted_payload_bytes,"segments":v.evicted_segments},
        "capture_totals":{"value":null,"reason":"This command reports retained measurements; it does not reconstruct capture totals from checkpoint claims."}});
    if magic == b"PFRCKP01" {
        let f = join::read_ppu(bytes, limits, rolling::Limits::default()).map_err(|e| {
            let code = match &e { join::Error::Store(x)=>store_code(x),join::Error::Semantic(x)=>ppu_code(x),join::Error::Limit(_)=>1,_=>2 };
            failure(code,e)
        })?;
        let final_seen = !f.retained.incomplete.iter().any(|r|matches!(r,Incomplete::NoTrailer | Incomplete::TornTail));
        out["source"] = json!({"final":final_seen,"complete":final_seen && f.retained.incomplete.is_empty(),
            "scope":"retained source validation; separate from physical sealing"});
        out["ppu"] = ppu_json(&f.retained);
    } else if magic == b"PFRMIX01" {
        let f = join::read_combined(bytes,limits,mixed::Limits {
            input_bytes:usize::try_from(cap).map_err(|_|failure(1,"host input cap"))?,
            temporary_bytes:analysis_cap,work_items:analysis_work,..join::desktop_limits()
        },mixed::ReportOptions { spu_profile_table:&[],causal_limits:Default::default() }).map_err(|e| {
            let code = match &e { join::CombinedError::Store(x)=>store_code(x),join::CombinedError::Semantic(x)=>mixed_code(x),join::CombinedError::Limit(_)=>1,_=>2 };
            failure(code,e)
        })?;
        let r = f.retained;
        // Extension validity is not a boolean whole-run claim. Expose each source's
        // facts; no SPU duration is invented from its decreasing local counter.
        out["source"] = json!({"final":r.terminal,"complete":null,
            "reason":"Mixed source validity is reported per family; no whole-run verdict is reconstructed."});
        out["ppu"] = ppu_json(&r.ppu);
        out["spu"] = json!({"placement":"CAUSAL_SUPPORT_ONLY","counter_history":"NOT_EVALUATED",
            "note":r.spu.causal.note,"refusal":r.spu.causal.refusal.as_ref().map(|x|format!("{x:?}")),
            "sources":r.spu.retained.sources.iter().map(|(key,s)|json!({"source":format!("{key:?}"),
                "retained_events":s.retained_events,"prefix_events":s.prefix_events,
                "retained_lost":s.retained_lost,"pairs":s.pairs.len(),"left_censored":s.left_censored.len(),
                "open_retained":s.open_retained.len(),"final_state":s.final_state,"raw_valid":s.raw_valid,
                "structure":format!("{:?}",s.structure),"protocol_error":s.protocol_error})).collect::<Vec<_>>()});
        if let Some(a) = r.auto_report {
            out["auto"] = json!({"map":format!("{:?}",a.map),"frames":a.frames.iter().map(|f|json!({
                "owner":f.view.owner,"frame":f.view.frame,"verdict":format!("{:?}",f.view.verdict),
                "reason":f.view.reason,"rsx_busy_raw":f.view.rsx_busy_raw,"rsx_busy_ppu":f.view.rsx_busy_ppu,
                "unavailable":f.unavailable.iter().map(|u|format!("{u:?}")).collect::<Vec<_>>() })).collect::<Vec<_>>()});
        }
        if let Some(f) = r.fifo {
            out["fifo"] = json!({"final":f.final_seen,"retained_events":f.retained.events,
                "retained_accounting_rows":f.retained.accounting_rows,"known_missing":f.known_missing,
                "prefix_unassociated":f.prefix_unassociated,"boundary_frames":f.boundary_frames.iter()
                    .map(|b|json!({"owner":b.owner,"first":b.first,"last":b.last})).collect::<Vec<_>>()});
        }
    } else { return Err(failure(1,"unsupported rolling checkpoint schema")); }
    Ok(out)
}

fn read_saved(path: &str, cap: u64) -> Result<Vec<u8>> {
    let file=File::open(path).map_err(|e|failure(3,e))?;
    let metadata=file.metadata().map_err(|e|failure(3,e))?;
    if !metadata.is_file() { return Err(failure(3,"a coherent saved regular file is required")); }
    if metadata.len()>cap { return Err(failure(1,"file exceeds --max-bytes")); }
    let size=usize::try_from(metadata.len()).map_err(|_|failure(1,"host file size"))?;
    let mut bytes=Vec::new(); bytes.try_reserve_exact(size).map_err(|e|failure(1,e))?;
    // Read no more than the admitted size; a separate byte catches growth without
    // allowing read_to_end to allocate beyond the admission limit.
    bytes.resize(size,0);
    (&file).read_exact(&mut bytes).map_err(|e|failure(3,e))?;
    let mut extra=[0u8;1]; let more=(&file).read(&mut extra).map_err(|e|failure(3,e))?;
    if bytes.len()!=size || more!=0 || file.metadata().map_err(|e|failure(3,e))?.len()!=metadata.len() {
        return Err(failure(3,"file changed while reading; use a coherent saved snapshot"));
    }
    Ok(bytes)
}
pub fn run(args: &[String]) -> ExitCode {
    let mut json_output=false; let mut cap=None; let mut analysis_cap=None; let mut analysis_work=None; let mut path=None; let mut i=0;
    let usage=|| { eprintln!("usage: ps3dk-prof rolling [--json] [--max-bytes N] [--max-analysis-bytes N] [--max-analysis-work N] <file>"); ExitCode::from(3) };
    while i<args.len() {
        match args[i].as_str() {
            "--json" if !json_output => json_output=true,
            "--max-bytes" if cap.is_none() => {
                i+=1; let Some(n)=args.get(i) else {return usage()};
                if n.is_empty() || !n.bytes().all(|b|b.is_ascii_digit()) {return usage()}
                let Ok(n)=n.parse::<u64>() else {return usage()};
                if n==0 || usize::try_from(n).is_err() {return usage()} cap=Some(n);
            }
            "--max-analysis-bytes" if analysis_cap.is_none() => {
                i+=1; let Some(n)=args.get(i) else {return usage()};
                if n.is_empty() || !n.bytes().all(|b|b.is_ascii_digit()) {return usage()}
                let Ok(n)=n.parse::<usize>() else {return usage()};
                if n==0 {return usage()} analysis_cap=Some(n);
            }
            "--max-analysis-work" if analysis_work.is_none() => {
                i+=1; let Some(n)=args.get(i) else {return usage()};
                if n.is_empty() || !n.bytes().all(|b|b.is_ascii_digit()) {return usage()}
                let Ok(n)=n.parse::<u64>() else {return usage()};
                if n==0 {return usage()} analysis_work=Some(n);
            }
            s if !s.starts_with('-') && path.is_none() => path=Some(s),
            _ => return usage(),
        } i+=1;
    }
    let Some(path)=path else {return usage()}; let cap=cap.unwrap_or(64<<20);
    let analysis_cap=analysis_cap.unwrap_or(join::DEFAULT_ANALYSIS_BYTES);
    let analysis_work=analysis_work.unwrap_or(join::DEFAULT_ANALYSIS_WORK);
    match read_saved(path,cap).and_then(|b|report(&b,cap,analysis_cap,analysis_work)) {
        Err((code,reason))=> {eprintln!("rolling: {reason}");ExitCode::from(code)},
        Ok(v)=> {
            if json_output { println!("{}",serde_json::to_string(&v).expect("JSON value serialization")); }
            else {
                println!("{}",NOTE);
                println!("physical {} (reason {}); retained chunks {}..{}; dropped prefix: {} chunks, {} bytes",
                    v["physical"]["state"].as_str().unwrap_or("UNSPECIFIED"),v["physical"]["reason"],
                    v["retained"]["first_chunk"],v["retained"]["next_chunk"],v["evicted"]["chunks"],v["evicted"]["payload_bytes"]);
                // The same projection supplies both formats, including unavailable
                // evidence and censoring; text cannot silently omit a family.
                println!("{}",serde_json::to_string_pretty(&v).expect("JSON value serialization"));
            }
            ExitCode::SUCCESS
        }
    }
}
