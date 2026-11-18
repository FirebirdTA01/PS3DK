//! Test-first general private C PPU accepted-state boundary. The prebuilt C
//! helper receives only original source bytes. No runtime/general-ready claim.
#[allow(dead_code)]
mod common;
use common::*;
use ps3dk_prof::{Outcome, rolling};
use std::io::Read;
use std::path::{Path, PathBuf};
use std::process::Command;

struct Scratch(PathBuf);
impl Scratch {
    fn new() -> Self {
        static NEXT: std::sync::atomic::AtomicU64 = std::sync::atomic::AtomicU64::new(0);
        let stamp = std::time::SystemTime::now().duration_since(std::time::UNIX_EPOCH).unwrap().as_nanos();
        for _ in 0..32 {
            let n = NEXT.fetch_add(1, std::sync::atomic::Ordering::Relaxed);
            let path = std::env::temp_dir().join(format!("ps3dk-prof-general-c-ppu-{}-{stamp}-{n}", std::process::id()));
            match std::fs::create_dir(&path) {
                Ok(()) => return Self(path),
                Err(e) if e.kind() == std::io::ErrorKind::AlreadyExists => continue,
                Err(e) => panic!("create private C encoder fixture directory: {e}"),
            }
        }
        panic!("could not create unique C encoder fixture directory");
    }
}
impl Drop for Scratch {
    fn drop(&mut self) {
        if std::thread::panicking() { eprintln!("FAILED C PPU fixture evidence retained at {}", self.0.display()); }
        else { let _ = std::fs::remove_dir_all(&self.0); }
    }
}
fn read_bounded(path: &Path, cap: usize) -> Vec<u8> {
    let f = std::fs::File::open(path).unwrap_or_else(|e| panic!("required C artifact {}: {e}", path.display()));
    assert!(f.metadata().unwrap().len() <= cap as u64);
    let mut bytes = Vec::new(); f.take(cap as u64 + 1).read_to_end(&mut bytes).unwrap();
    assert!(bytes.len() <= cap); bytes
}
fn original() -> File {
    let groups = vec![
        vec![frame(0, 1000, 0)],
        vec![begin(1, 0, 0, 1010, 1), bparent(2, 0, 0)],
        vec![begin(3, 1, 1, 1020, 2), bparent(4, 1, 1), end(5, 1, 1, 1040, 2)],
        vec![end(6, 0, 0, 1050, 1), frame(7, 1100, 1)],
        vec![begin(8, 2, 0, 1110, 3), bparent(9, 0, 0), end(10, 2, 0, 1140, 3), frame(11, 1200, 2)],
    ];
    let mut f = scenario(vec![lane(1, groups.concat())]).file();
    let trailer = f.chunks.pop().unwrap(); f.chunks.truncate(4);
    f.chunks.extend(groups.into_iter().map(|records| chunk(T_RECORDS, records_payload(1, &records))));
    f.chunks.push(trailer); f
}
fn offset(f: &File, count: usize) -> usize { 16 + f.chunks[..count].iter().map(|c| c.payload.len() + 20).sum::<usize>() }
fn limits() -> rolling::Limits {
    rolling::Limits { checkpoint_bytes: 65536, metadata_bytes: 16 + 32768,
        incarnations: 16, descriptors: 16, stack_rows: 32, frame_runs: 64, work_records: 1_000_000 }
}

#[test]
#[ignore = "requires prebuilt run_ppu_encoder.py fixture via PS3DK_PROF_ROLLING_PPU_ENCODER"]
fn c_general_ppu_encoder_matches_original_cuts_and_retained_semantics() {
    let exe = std::env::var_os("PS3DK_PROF_ROLLING_PPU_ENCODER")
        .expect("set PS3DK_PROF_ROLLING_PPU_ENCODER to prebuilt rolling-ppu-encoder-fixture");
    let exe = std::fs::canonicalize(exe).expect("prebuilt general C PPU fixture executable must exist");
    assert!(exe.is_file());
    let f = original(); let source = f.bytes();
    let full = match load(&source) {
        Outcome::Analysed(a) => a,
        Outcome::Malformed(e) => panic!("independent original source must validate before invoking C: {e}"),
    };
    assert!(full.capture.incomplete.is_empty()); assert_eq!(full.capture.spans.len(), 3);
    assert_eq!(full.capture.lanes[&1].written, 12);
    // Establish every reference prefix FIRST. The C process never sees these
    // bytes or a serialized Rust state, only the ordinary original source.
    let expected: Vec<_> = (0..f.chunks.len()).map(|cut|
        rolling::checkpoint_ppu(&source[..offset(&f, cut)], limits())
            .unwrap_or_else(|e| panic!("independent original cut {cut} invalid: {e:?}"))).collect();
    eprintln!("PASS independent original PPU source and all bootstrap/record cut references (12 records, 3 spans)");
    let tmp = Scratch::new(); let input = tmp.0.join("source.prof"); let output = tmp.0.join("c-output");
    std::fs::write(&input, &source).unwrap(); std::fs::create_dir(&output).unwrap();
    let run = Command::new(&exe).arg(&input).arg(&output).output().expect("execute general C PPU fixture");
    std::fs::write(tmp.0.join("producer.stdout"), &run.stdout).unwrap();
    std::fs::write(tmp.0.join("producer.stderr"), &run.stderr).unwrap();
    assert!(run.status.success(), "general C PPU accepted-state encoder unavailable or rejected valid input: {:?}\nstdout: {}\nstderr: {}",
        run.status, String::from_utf8_lossy(&run.stdout), String::from_utf8_lossy(&run.stderr));
    assert_eq!(std::fs::read(&input).unwrap(), source, "source input is immutable");
    let manifest: serde_json::Value = serde_json::from_slice(&read_bounded(&output.join("cuts.json"), 1048576)).unwrap();
    assert_eq!(manifest["schema"].as_u64(), Some(1));
    assert_eq!(manifest["next_chunk"].as_u64(), Some(f.chunks.len() as u64));
    assert_eq!(manifest["prefix_bytes"].as_u64(), Some((source.len() - 16) as u64));
    assert_eq!(manifest["terminal"].as_u64(), Some(1));
    let cuts = manifest["cuts"].as_array().unwrap(); assert_eq!(cuts.len(), expected.len());
    let mut previous: Option<Vec<u8>> = None;
    for (cut, direct) in expected.iter().enumerate() {
        let at = offset(&f, cut);
        assert_eq!(cuts[cut]["next_chunk"].as_u64(), Some(cut as u64));
        assert_eq!(cuts[cut]["source_offset"].as_u64(), Some(at as u64));
        let cp = read_bounded(&output.join(format!("cut-{cut:06}.ckp")), 65536);
        assert_eq!(cuts[cut]["checkpoint_bytes"].as_u64(), Some(cp.len() as u64));
        assert_eq!(&cp, direct, "independent C canonical PFRCKP01 differs at original cut {cut}");
        if let Some(old) = previous {
            let next = rolling::advance_ppu(&old, &source[offset(&f, cut - 1)..at], limits()).unwrap();
            assert_eq!(cp, next, "actual C predecessor and original next chunk must advance canonically");
        }
        let r = rolling::read_ppu_suffix(&cp, &source[at..], limits()).unwrap();
        assert!(r.incomplete.is_empty());
        assert_eq!((r.first_chunk, r.next_chunk, r.prefix_bytes), (cut as u64, f.chunks.len() as u64, (at - 16) as u64));
        let prefix: u64 = f.chunks[..cut].iter().filter(|c| c.ty == T_RECORDS)
            .map(|c| u64::from(u32::from_le_bytes(c.payload[4..8].try_into().unwrap()))).sum();
        assert_eq!((r.lanes[&1].prefix_written, r.lanes[&1].retained_written), (prefix, 12 - prefix));
        for (desc, name) in [(0, "zoneA"), (1, "zoneB"), (2, "zoneC")] {
            let visible: Vec<_> = full.capture.spans.iter().filter(|s| s.desc == desc && s.end_seq.is_some_and(|end| end >= prefix)).collect();
            let complete = visible.iter().filter(|s| s.begin_seq >= prefix).count() as u64;
            let left = visible.iter().filter(|s| s.begin_seq < prefix).count() as u64;
            let z = r.zones.iter().find(|z| z.name == name);
            assert_eq!(z.map_or((0, 0), |z| (z.completed, z.left_censored)), (complete, left), "{name} original cut {cut}");
        }
        if cut == 6 {
            for (name, complete, censored, ticks) in [("zoneA",0,1,None),("zoneB",1,0,Some(20)),("zoneC",1,0,Some(30))] {
                let z = r.zones.iter().find(|z| z.name == name).unwrap();
                assert_eq!((z.completed,z.left_censored,z.total_ticks),(complete,censored,ticks));
            }
        }
        previous = Some(cp);
    }
    assert!(!output.join(format!("cut-{:06}.ckp", f.chunks.len())).exists(), "genuine TRAILER has no successor checkpoint");
}

// Remaining selected-family stages. These tests precede their C behavior;
// ordinary source validation and direct-prefix encoding remain independent.
const DEFAULT: [u32; 6] = [16, 16, 32, 64, 32768, 65536];
fn exe() -> PathBuf {
    let path = std::env::var_os("PS3DK_PROF_ROLLING_PPU_ENCODER").expect("set PS3DK_PROF_ROLLING_PPU_ENCODER");
    let path = std::fs::canonicalize(path).expect("prebuilt C encoder fixture"); assert!(path.is_file()); path
}
fn cfg_limits(c: [u32; 6]) -> rolling::Limits {
    rolling::Limits { incarnations: c[0] as usize, descriptors: c[1] as usize, stack_rows: c[2] as usize,
        frame_runs: c[3] as usize, metadata_bytes: 16 + c[4] as usize, checkpoint_bytes: c[5] as usize,
        work_records: 1_000_000 }
}
fn invoke(f: &File, cfg: [u32; 6], label: &str) -> (Scratch, PathBuf, std::process::Output) {
    let tmp = Scratch::new(); let input = tmp.0.join("source.prof"); let out = tmp.0.join("c-output");
    std::fs::write(&input, f.bytes()).unwrap(); std::fs::create_dir(&out).unwrap();
    std::fs::write(tmp.0.join("case.txt"), format!("{label}\nconfig={cfg:?}\n")).unwrap();
    let run = Command::new(exe()).arg(&input).arg(&out).args(cfg.map(|v| v.to_string())).output().unwrap();
    std::fs::write(tmp.0.join("producer.stdout"), &run.stdout).unwrap();
    std::fs::write(tmp.0.join("producer.stderr"), &run.stderr).unwrap();
    (tmp, out, run)
}
fn accepted(f: &File, cfg: [u32; 6], label: &str) -> Vec<Vec<u8>> {
    let source = f.bytes(); assert_eq!(f.chunks.last().unwrap().ty, T_TRAILER);
    let full = match load(&source) { Outcome::Analysed(a) => a, Outcome::Malformed(m) => panic!("{label}: invalid independent source: {m}") };
    let l = cfg_limits(cfg);
    // The writer stores no TRAILER. The independent suffix reader also parses
    // that terminal chunk, so its analysis allowance includes those bytes.
    let analysis = rolling::Limits { metadata_bytes: l.metadata_bytes
        + f.chunks.last().unwrap().payload.len() + 24, ..l };
    let references: Vec<_> = (0..f.chunks.len()).map(|cut|
        rolling::checkpoint_ppu(&source[..offset(f, cut)], l).unwrap_or_else(|e| panic!("{label}: independent cut {cut}: {e:?}"))).collect();
    let (_tmp, out, run) = invoke(f, cfg, label);
    assert!(run.status.success(), "{label}: valid source refused by C: {:?}\n{}", run.status, String::from_utf8_lossy(&run.stderr));
    let manifest: serde_json::Value = serde_json::from_slice(&read_bounded(&out.join("cuts.json"), 1048576)).unwrap();
    assert_eq!(manifest["cuts"].as_array().unwrap().len(), references.len());
    assert_eq!(manifest["next_chunk"].as_u64(), Some(f.chunks.len() as u64));
    assert_eq!(manifest["prefix_bytes"].as_u64(), Some((source.len() - 16) as u64));
    assert_eq!(manifest["terminal"].as_u64(), Some(1));
    let mut cps = Vec::new();
    for (cut, expected) in references.into_iter().enumerate() {
        let cp = read_bounded(&out.join(format!("cut-{cut:06}.ckp")), cfg[5] as usize);
        assert_eq!(cp, expected, "{label}: canonical C state at cut {cut}");
        let r = rolling::read_ppu_suffix(&cp, &source[offset(f, cut)..], analysis)
            .unwrap_or_else(|e| panic!("{label}: original suffix at cut {cut}: {e:?}"));
        assert_eq!(r.incomplete, full.capture.incomplete, "{label}: incomplete is not promoted by terminal");
        for (&inc, lane) in &full.capture.lanes {
            let mut written = 0u64; let mut gaps = 0u64;
            for c in &f.chunks[..cut] {
                if c.ty != T_RECORDS || u32_at(&c.payload, 0) != inc { continue; }
                for rec in c.payload[8..].chunks_exact(32) {
                    if rec[0] == 5 { gaps += u64_at(rec, 24) - u64_at(rec, 16) + 1; }
                    else { written += 1; }
                }
            }
            let got = &r.lanes[&inc];
            assert_eq!((got.prefix_written, got.retained_written), (written, lane.written - written), "{label}: incarnation {inc}");
            assert_eq!(got.prefix_gap_sum, gaps);
        }
        cps.push(cp);
    }
    assert!(!out.join(format!("cut-{:06}.ckp", f.chunks.len())).exists()); cps
}
fn refused(f: &File, cfg: [u32; 6], marker: &str, label: &str) {
    let (_tmp, _out, run) = invoke(f, cfg, label); let text = String::from_utf8_lossy(&run.stderr);
    assert!(!run.status.success() && text.contains(marker), "{label}: expected {marker}, got {:?}\n{text}", run.status);
    assert!(!text.contains("prepare changed accepted"), "{label}: rejection changed accepted state/context/input");
}
fn u32_at(b: &[u8], at: usize) -> u32 { u32::from_le_bytes(b[at..at + 4].try_into().unwrap()) }
fn u64_at(b: &[u8], at: usize) -> u64 { u64::from_le_bytes(b[at..at + 8].try_into().unwrap()) }
fn put64(b: &mut [u8], at: usize, value: u64) { b[at..at + 8].copy_from_slice(&value.to_le_bytes()); }
fn section(cp: &[u8], kind: u16) -> (u32, &[u8]) {
    let mut at = 64;
    for expected in 1..=3u16 {
        assert_eq!(u16::from_le_bytes(cp[at..at + 2].try_into().unwrap()), expected);
        let count = u32_at(cp, at + 4); let n = u32_at(cp, at + 8) as usize;
        if expected == kind { return (count, &cp[at + 16..at + 16 + n]); }
        at += 16 + n;
    }
    panic!("section missing")
}
fn partition_records(mut s: Scenario, groups: Vec<Vec<Rec>>) -> File {
    s.lanes[0].recs = groups.concat(); let mut f = s.file(); let trailer = f.chunks.pop().unwrap(); f.chunks.truncate(4);
    f.chunks.extend(groups.into_iter().map(|rows| chunk(T_RECORDS, records_payload(1, &rows)))); f.chunks.push(trailer); f
}

#[test]
#[ignore = "requires prebuilt general C encoder"]
fn c_general_ppu_gap_regerr_unknown_base_and_clock_fence() {
    let groups = vec![
        vec![frame(0,1000,0),begin(1,0,0,1010,1),bparent(2,0,0)],
        vec![gap(3,5),end(6,0,0,1030,1)],
        vec![begin(7,1,2,1040,2),bparent(8,2,99),regerr(9,1)],
        vec![end(10,1,2,1050,2),frame(11,1100,4)],
        vec![begin(12,2,0,900,3),bparent(13,0,0),end(14,2,0,1200,3)],
    ];
    let mut l = lane(1, vec![]); l.regerr = 2;
    let f = partition_records(scenario(vec![l]), groups); let cps = accepted(&f, DEFAULT, "producer loss, partial unknown stack, REGERR and TB regression");
    let (_, unknown) = section(&cps[7], 2);
    assert_eq!((u64_at(unknown,16),u64_at(unknown,24),u64_at(unknown,32)),(3,1,1));
    assert_ne!(u32_at(unknown,104)&16,0); assert_eq!(u32_at(unknown,108),1);
    assert_eq!((u32_at(unknown,184),u32_at(unknown,188)),(2,1)); // Loss context, affected
    let (_, holes) = section(&cps[8], 2); assert_eq!(u32_at(holes,112),2);
    assert_eq!((u64_at(holes,120),u64_at(holes,128),u64_at(holes,136),u64_at(holes,144)),(0,0,4,4));
    let (_, final_) = section(&cps[9], 2); assert_eq!(u32_at(final_,104)&16,0);
    assert_ne!(u32_at(final_,104)&32,0); assert_eq!(u64_at(final_,56),1200);
    let mut tail=f.clone();tail.chunks[5].payload.truncate(40);tail.chunks[5].payload[4..8].copy_from_slice(&1u32.to_le_bytes());
    assert!(matches!(load(&tail.bytes()),Outcome::Malformed(_)));
    refused(&tail,DEFAULT,"refused (-1,-1), reason 0","GAP cannot end an accepted chunk");
    let mut adjacent=f.clone();adjacent.chunks[5]=chunk(T_RECORDS,records_payload(1,&[gap(3,4),gap(5,5),end(6,0,0,1030,1)]));
    assert!(matches!(load(&adjacent.bytes()),Outcome::Malformed(_)));
    refused(&adjacent,DEFAULT,"refused (-1,-1), reason 0","adjacent producer GAPs are malformed");
}
fn tokens() -> File {
    let a = vec![begin(0,0,0,1010,1),bparent_tok(1,0,0,2,1),end(2,0,0,1020,1)];
    let b = vec![frame(0,1000,0),frame(1,1100,1)];
    let mut f = scenario(vec![lane(1,a.clone()),lane(2,b.clone())]).file(); let trailer = f.chunks.pop().unwrap(); f.chunks.truncate(4);
    f.chunks.extend([chunk(T_RECORDS,records_payload(1,&a[..2])),chunk(T_RECORDS,records_payload(1,&a[2..])),
        chunk(T_RECORDS,records_payload(2,&b)),trailer]); f
}
#[test]
#[ignore = "requires prebuilt general C encoder"]
fn c_general_ppu_closed_tokens_resolve_within_final_joint_budget() {
    let f = tokens(); let mut cfg = DEFAULT; cfg[3] = 1;
    let cps = accepted(&f,cfg,"closed explicit token, same-chunk transient run+token=2, final joint count=1");
    let (count, keys) = section(&cps[6],3); assert_eq!(count,1); assert_eq!((u32_at(keys,0),u64_at(keys,8)),(2,1));
    assert_eq!(section(&cps[7],3).0,0);
    // A frame run plus a genuinely unresolved second frame exceeds F=1.
    let mut over = f.clone(); let rec = &mut over.chunks[4].payload; put64(rec,8+32+24,2);
    // Leave source incomplete (no TRAILER), so token absence is not malformed.
    over.chunks.pop();
    assert!(reader::read(&over.bytes()).is_ok());
    refused(&over,cfg,"refused (-7,-7), reason 3","genuine final joint identity excess");
    let mut impossible = f.clone(); put64(&mut impossible.chunks[4].payload,8+32+24,2);
    assert!(matches!(load(&impossible.bytes()),Outcome::Malformed(_)));
    refused(&impossible,DEFAULT,"refused (-1,-1), reason 0","token impossible at lossless quiesced final");
}
fn retired_source() -> File {
    let a = zone(0,0,0,1000,1,0).into_iter().chain([end(2,0,0,1010,1)]).collect::<Vec<_>>();
    let b = zone(0,1,0,1100,1,0).into_iter().chain([end(2,1,0,1120,1)]).collect::<Vec<_>>();
    let mut old = lane(1,a); old.state=1;
    let mut f=scenario(vec![old,lane(9,b)]).file();
    f.chunks[3]=chunk(T_THREADS,threads_payload(&[thr(1)]));
    f.chunks.insert(6,chunk(T_THREADS,threads_payload(&[thr(9)]))); f
}
#[test]
#[ignore = "requires prebuilt general C encoder"]
fn c_general_ppu_retirement_slot_reuse_and_exact_retired_equations() {
    let f=retired_source(); let cps=accepted(&f,DEFAULT,"retired slot reuse with lifetime incarnation evidence");
    let (count, row)=section(&cps[7],2); assert_eq!(count,2); assert_ne!(u32_at(row,104)&128,0);
    let mut bad=f.clone(); put64(&mut bad.chunks[5].payload,4+16,2); // written count disagrees
    assert!(matches!(load(&bad.bytes()),Outcome::Malformed(_)));
    refused(&bad,DEFAULT,"refused (-1,-1), reason 0","retired arithmetic before any subsequent declaration");
    let mut live=f.clone(); live.chunks.swap(5,6);
    assert!(matches!(load(&live.bytes()),Outcome::Malformed(_)));
    refused(&live,DEFAULT,"refused (-1,-1), reason 0","slot reuse before retirement");
}
#[test]
#[ignore = "requires prebuilt general C encoder"]
fn c_general_ppu_final_states_preserve_incomplete_and_known_tail_loss() {
    let records=vec![begin(0,0,0,1000,1),bparent(1,0,0),end(2,0,0,1010,1)];
    let mut l=lane(1,records.clone()); l.state=3;
    accepted(&scenario(vec![l]).file(),DEFAULT,"NOT_QUIESCED exact written plus UNKNOWN snapshots");
    let mut l=lane(1,records.clone()); l.terminal=2;l.refused=4;l.regerr=5;l.limit=true;
    let mut s=scenario(vec![l]);s.sink_errors=1;
    let f=s.file(); accepted(&f,DEFAULT,"terminal loss, refusals, REGERR lower bound, SEQ_LIMIT and sink error");
    let mut missing=scenario(vec![lane(1,records)]).file();
    let mut tr=Trl{end_flags:0,fins:vec![],sink_errors:0,first_error:0,reserved:0,unregistered:0,desc_count:3};
    missing.chunks.last_mut().unwrap().payload=tr.payload();
    accepted(&missing,DEFAULT,"genuine TRAILER missing a final stays incomplete");
    // Open producer snapshot is range-bounded, not required to equal observed
    // depth. Ordinary reader permits this and the C mirror must preserve it.
    let open=scenario(vec![lane(1,vec![begin(0,0,0,1000,1),bparent(1,0,0)])]);
    tr=open.trailer_struct();tr.fins[0].open=7;
    let mut open_file=open.file();open_file.chunks.last_mut().unwrap().payload=tr.payload();
    accepted(&open_file,DEFAULT,"open snapshot has no invented equality to partial stack");
    let mut bad=f.clone(); let p=&mut bad.chunks.last_mut().unwrap().payload;put64(p,8+8,4);
    assert!(matches!(load(&bad.bytes()),Outcome::Malformed(_)));
    refused(&bad,DEFAULT,"refused (-1,-1), reason 0","produced != written+dropped");
}
#[test]
#[ignore = "requires prebuilt general C encoder"]
fn c_general_ppu_exact_counter_ceiling_is_not_history_work_or_storage() {
    let f=scenario(vec![lane(1,vec![gap(0,P63-2),frame(P63-1,1000,P63-1)])]).file();
    let mut cfg=DEFAULT;cfg[0]=1;cfg[2]=0;cfg[3]=1;
    let cps=accepted(&f,cfg,"2^63 exact next/produced from one GAP and one FRAME, bounded state/work");
    let (_,row)=section(&cps[5],2);
    assert_eq!((u64_at(row,8),u64_at(row,16),u64_at(row,40)),(1,P63-1,P63));
    assert_eq!((u64_at(row,120),u64_at(row,128)),(P63-1,P63-1));
    let mut bad=f.clone();put64(&mut bad.chunks[4].payload,8+24,P63-1); // next FRAME must now be >=2^63
    assert!(matches!(load(&bad.bytes()),Outcome::Malformed(_)));
    refused(&bad,DEFAULT,"refused (-1,-1), reason 0","GAP/next sequence ceiling refuses instead of wrapping");
}
fn wb(id:u32,name:u32,first:u64,kind:u8,count:u32)->Chunk {
    let mut b=Buf::default();b.u16(1).u16(0).u32(id).u32(name).u64(first).u8(1).u8(kind).u16(0).u32(count).u32(0);chunk(41,b.0)
}
fn we(id:u32,last:u64,reason:u8)->Chunk {
    let mut b=Buf::default();b.u16(1).u16(0).u32(id).u8(reason).u8(0).u16(0).u64(last);b.0.extend_from_slice(&[0;12]);chunk(42,b.0)
}
fn window_source()->File {
    let records=vec![frame(0,1000,0),frame(1,1100,1),frame(2,1200,2)];
    let mut f=scenario(vec![lane(1,records.clone())]).file();let trailer=f.chunks.pop().unwrap();f.chunks.truncate(4);f.flags=512;
    for (id,name,kind,count,reason) in [(1,0,2,1,2),(2,u32::MAX,1,0,1),(3,1,3,0,4)] {
        f.chunks.extend([wb(id,name,u64::from(id-1),kind,count),chunk(T_RECORDS,records_payload(1,&records[(id-1)as usize..id as usize])),we(id,u64::from(id-1),reason)]);
    }
    f.chunks.push(trailer);f
}
#[test]
#[ignore = "requires prebuilt general C encoder"]
fn c_general_ppu_windows_named_unnamed_count_open_and_extreme_bounds() {
    let f=window_source();accepted(&f,DEFAULT,"FRAMES/MANUAL/SECTION and unnamed app window");
    let mut open=f.clone();open.chunks.remove(open.chunks.len()-2);accepted(&open,DEFAULT,"open final window is retained as incomplete window evidence");
    for kind in [1,2] {
        let mut extreme=scenario(vec![lane(1,vec![])]).file();let trailer=extreme.chunks.pop().unwrap();extreme.flags=512;
        extreme.chunks.extend([wb(1,0,u64::MAX,kind,if kind==2{1}else{0}),we(1,u64::MAX,if kind==2{2}else{1}),trailer]);
        accepted(&extreme,DEFAULT,"valid u64::MAX window endpoints need no invented FRAME membership");
    }
    let mut overflow=scenario(vec![lane(1,vec![])]).file();let trailer=overflow.chunks.pop().unwrap();overflow.flags=512;
    overflow.chunks.extend([wb(1,0,0,2,1),we(1,u64::MAX,2),trailer]);
    assert!(matches!(load(&overflow.bytes()),Outcome::Malformed(_)));
    refused(&overflow,DEFAULT,"refused (-1,-1), reason 0","COUNT inclusive interval overflow refuses");
}

#[test]
#[ignore = "requires prebuilt general C encoder"]
fn c_general_ppu_capacity_exact_fit_and_refusal_preserve_accepted_state() {
    let f=original();let bytes=f.bytes();let references:Vec<_>=(0..f.chunks.len()).map(|i|
        rolling::checkpoint_ppu(&bytes[..offset(&f,i)],limits()).unwrap()).collect();
    let required=references.iter().map(Vec::len).max().unwrap() as u32;
    let metadata=f.chunks.iter().filter(|c|!matches!(c.ty,T_RECORDS|T_TRAILER)).map(|c|c.payload.len()+24).sum::<usize>() as u32;
    let mut exact=DEFAULT;exact[0]=1;exact[1]=3;exact[2]=2;exact[3]=1;exact[4]=metadata;exact[5]=required;
    accepted(&f,exact,"exact configured successor/metadata/stack/identity capacities");
    for (field,value,reason,label) in [(0,0,3,"incarnation cap"),(1,2,3,"descriptor cap"),(2,1,2,"active stack cap"),
        (3,0,3,"frame identity cap"),(4,metadata-1,4,"stored metadata one byte short"),(5,required-1,1,"successor checkpoint byte cap")] {
        let mut cfg=exact;cfg[field]=value;
        refused(&f,cfg,&format!("refused (-7,-7), reason {reason}"),label);
    }
    let mut impossible=DEFAULT;impossible[0]=u32::MAX;
    refused(&f,impossible,"arena planner unavailable","offset/size arithmetic must refuse before allocation");
}

#[test]
#[ignore = "requires prebuilt general C encoder"]
fn c_general_ppu_closed_history_compacts_without_lifetime_record_budget() {
    let groups:Vec<_>=(0..128u64).map(|i|vec![begin(i*3,0,0,1000+i*20,i+1),bparent(i*3+1,0,0),end(i*3+2,0,0,1010+i*20,i+1)]).collect();
    let f=partition_records(scenario(vec![lane(1,vec![])]),groups);let mut cfg=DEFAULT;cfg[0]=1;cfg[2]=1;cfg[3]=0;
    let cps=accepted(&f,cfg,"128 closed pairs reuse one active stack node, no historical replay/storage");
    let stable=cps[5].len();assert!(cps[5..].iter().all(|cp|cp.len()==stable));
    assert_eq!(u64_at(&cps[cps.len()-1],48),384);
}

#[test]
#[ignore = "requires prebuilt general C encoder"]
fn c_general_ppu_rejects_malformed_and_unselected_original_inputs() {
    let base=original();
    let mut descriptor=base.clone();descriptor.chunks[2].payload[8]^=1;
    assert!(matches!(load(&descriptor.bytes()),Outcome::Malformed(_)));
    refused(&descriptor,DEFAULT,"refused (-1,-1), reason 0","descriptor hash mismatch with repaired framing CRC");
    let mut parent=base.clone();parent.chunks[5].payload.truncate(40);parent.chunks[5].payload[4..8].copy_from_slice(&1u32.to_le_bytes());
    assert!(matches!(load(&parent.bytes()),Outcome::Malformed(_)));
    refused(&parent,DEFAULT,"refused (-1,-1), reason 0","BEGIN stranded at chunk end");
    let mut duplicate=base.clone();put64(&mut duplicate.chunks[8].payload,32,1);
    assert!(matches!(load(&duplicate.bytes()),Outcome::Malformed(_)));
    refused(&duplicate,DEFAULT,"refused (-1,-1), reason 0","nonincreasing begun span maximum");
    let mut ext=base.clone();ext.flags=1;
    assert!(matches!(rolling::checkpoint_ppu(&ext.bytes()[..16],limits()),Err(rolling::Error::Unsupported{..})));
    refused(&ext,DEFAULT,"initial checkpoint unavailable","unsupported EXT flags remain unmasked");
    let mut family=base.clone();family.chunks.insert(4,chunk(43,vec![0;96]));
    refused(&family,DEFAULT,"refused (-1,-1), reason 0","FIFO43 is not PPU metadata");
    let mut unicode=scenario(vec![lane(1,vec![begin(0,0,0,1000,1),bparent(1,0,0),end(2,0,0,1010,1)])]);
    unicode.descs[0].name="λ-zone".as_bytes().to_vec();unicode.descs[0].file="src/計測.c".as_bytes().to_vec();
    accepted(&unicode.file(),DEFAULT,"valid bounded UTF-8 participates in exact descriptor hash");
}

#[test]
#[ignore = "requires prebuilt general C encoder"]
fn c_general_ppu_per_kind_payloads_do_not_gain_invented_restrictions() {
    // Unknown-base END still needs a nonzero ID and declared descriptor, but
    // unlike BEGIN it has no <2^63 maximum or known-stack depth obligation.
    let mut l=lane(1,vec![gap(0,0),end(1,0,255,1000,u64::MAX),rec(6,0,0,1,2,u64::MAX,u64::MAX)]);
    l.regerr=1;
    accepted(&scenario(vec![l]).file(),DEFAULT,"unknown-base END high ID/depth and unrestricted REGERR b/c payload");
    // REGERR a is a code, not a descriptor reference. The incarnation must
    // exist, while a DESCRIPTORS table is not required for this record kind.
    let mut l=lane(1,vec![rec(6,0,0,2,0,u64::MAX,0)]);l.regerr=1;
    let mut s=scenario(vec![l]);s.descs.clear();let mut f=s.file();
    assert_eq!(f.chunks[2].ty,T_DESCS);f.chunks.remove(2);
    accepted(&f,DEFAULT,"declared incarnation REGERR code2 without any descriptors");
}
