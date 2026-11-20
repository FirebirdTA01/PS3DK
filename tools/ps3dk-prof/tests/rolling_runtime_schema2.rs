//! Actual producer/core private schema2 bridge: no source synthesis or replay.
//! The prebuilt helper must ordinary-capture the same application schedule first.
#[allow(dead_code)] mod common;
use ps3dk_prof::{reader,rolling_combined as combined,rolling_file,rolling_store,Outcome};
use serde_json::Value;
use std::{io::Read,path::{Path,PathBuf},process::{Command,Stdio}};

struct Run { dir:PathBuf, meta:Value }
impl Drop for Run { fn drop(&mut self) {
    if std::thread::panicking(){eprintln!("schema2 runtime evidence retained: {}",self.dir.display());}
    else {let _=std::fs::remove_dir_all(&self.dir);}
}}
fn bounded(path:&Path,cap:usize)->Vec<u8>{
    let f=std::fs::File::open(path).unwrap_or_else(|e|panic!("{}: {e}",path.display()));
    assert!(f.metadata().unwrap().len()<=cap as u64);let mut b=Vec::new();
    f.take(cap as u64+1).read_to_end(&mut b).unwrap();assert!(b.len()<=cap);b
}
fn run(case:&str)->Run{
    let late=case.starts_with("late-");
    let automatic=if case=="rotation"{std::env::var_os("PS3DK_PROF_RUNTIME_SCHEMA2_AUTOMATIC")}else{None};
    if automatic.is_some(){assert!(public_profile(),"automatic fixture uses the public profile");}
    let exe=std::fs::canonicalize(if late{std::env::var_os("PS3DK_PROF_RUNTIME_DELAYED_FIFO")}else{automatic.or_else(||std::env::var_os("PS3DK_PROF_RUNTIME_SCHEMA2"))}
        .expect("set prebuilt actual schema2 runtime helper; no implicit skip")).unwrap();
    static NEXT:std::sync::atomic::AtomicU64=std::sync::atomic::AtomicU64::new(0);
    let id=NEXT.fetch_add(1,std::sync::atomic::Ordering::Relaxed);
    let stamp=std::time::SystemTime::now().duration_since(std::time::UNIX_EPOCH).unwrap().as_nanos();
    let dir=std::env::temp_dir().join(format!("ps3dk-runtime-schema2-{}-{stamp}-{id}",std::process::id()));
    std::fs::create_dir(&dir).unwrap();let mut r=Run{dir,meta:Value::Null};
    let out=r.dir.join("stdout.log");let err=r.dir.join("stderr.log");
    let mut c=Command::new(exe).arg(case).arg(&r.dir)
        .env("PS3DK_SCHEMA2_AUTOMATIC_OUTPUT",&r.dir)
        .stdout(Stdio::from(std::fs::File::create(&out).unwrap()))
        .stderr(Stdio::from(std::fs::File::create(&err).unwrap())).spawn().unwrap();
    let start=std::time::Instant::now();let status=loop{
        if let Some(s)=c.try_wait().unwrap(){break s;}
        if start.elapsed()>std::time::Duration::from_secs(90)||[&out,&err].iter().any(|p|std::fs::metadata(p).is_ok_and(|m|m.len()>1<<20)){
            let _=c.kill();let _=c.wait();panic!("schema2 helper deadline/log bound; not behavioral evidence");
        }
        std::thread::sleep(std::time::Duration::from_millis(10));
    };
    assert!(status.success(),"{case}: actual native assertion failed ({status}): {}",String::from_utf8_lossy(&bounded(&err,1<<20)));
    let _=bounded(&out,1<<20);r.meta=serde_json::from_slice(&bounded(&r.dir.join(if late{"late.json"}else{"result.json"}),65536)).unwrap();
    if late{assert_eq!(r.meta["case"],case);return r;}
    assert_eq!(r.meta["schema"],1);assert_eq!(r.meta["case"],case);assert_eq!(r.meta["flags"],707);
    assert!(r.meta["attach_PAL_calls"].as_u64().unwrap()>0&&r.meta["attach_PAL_bytes"].as_u64().unwrap()>0);
    assert_eq!(r.meta["MAIN_mapped_bytes"],1<<20);assert!(r.meta["MAIN_allocation_bytes"].as_u64().unwrap()>1<<20);
    assert_eq!(r.meta["steady_PAL_calls"],0);assert_eq!(r.meta["steady_MAIN_calls"],0);
    assert_eq!(r.meta["global_memory_cap_claim"],false);r
}
fn analyse(b:&[u8])->Box<ps3dk_prof::Analysis>{
    reader::read(b).expect("ordinary reader validates the actual original before rolling comparisons");
    match common::load(b){Outcome::Analysed(a)=>a,Outcome::Malformed(m)=>panic!("actual source malformed: {m}")}
}
fn offsets(b:&[u8])->Vec<usize>{
    assert_eq!(u32::from_le_bytes(b[12..16].try_into().unwrap()),707);
    let mut v=vec![16];let mut at=16;
    while at<b.len(){assert!(at+20<=b.len()&&v.len()<32768);
        let n=u32::from_le_bytes(b[at+4..at+8].try_into().unwrap()) as usize;
        assert_eq!(u64::from_le_bytes(b[at+8..at+16].try_into().unwrap()),(v.len()-1) as u64);
        at=at.checked_add(20+n).unwrap();assert!(at<=b.len());v.push(at);
    }v
}
fn append(recipe:&str)->Run{
    let r=run(&format!("append-{recipe}"));let b=bounded(&r.dir.join("capture.prof"),16<<20);let a=analyse(&b);let o=offsets(&b);
    assert!(a.capture.trailer.is_some());assert_eq!(a.capture.profile.as_ref().unwrap().profile_id,"SCHEMA2-HOST");
    let auto=a.capture.auto.as_ref().unwrap();let cfg=auto.config.as_ref().unwrap().attached().unwrap();
    assert_eq!(cfg.kind_mask,4095);assert_eq!(cfg.form_mask,11);assert!(auto.final_seen);
    let draws:Vec<_>=auto.ops.iter().filter(|x|x.covered()).collect();
    assert_eq!(draws.len(),if recipe=="rotation"{47_020}else{31});assert_eq!(r.meta["draws"],draws.len());
    assert!(draws.iter().all(|x|x.kind==3&&x.raw_valid()&&x.raw_end>x.raw_begin&&x.ring.is_some()));
    assert_eq!(r.meta["ring_acks"],draws.len());assert_eq!(r.meta["acks"],auto.ops.len());
    let fifo=a.capture.fifo.as_ref().unwrap();assert!(fifo.final_seen&&!fifo.accounting.rows.is_empty());
    assert!(fifo.accounting.rows.iter().any(|x|x.reasons==0&&x.prepared>0&&x.published>0),"actual owner bytes retained");
    if recipe=="rotation"{assert!(fifo.parsed_events>0,"actual native wraps emitted events");assert!(auto.ring_seen.generations>4);}
    // These are reference calculations from the C-produced original bytes, not
    // fixture inputs or a substitute for the runtime acceptance/ACK path.
    assert_eq!(u32::from_le_bytes(b[o[o.len()-2]..o[o.len()-2]+4].try_into().unwrap()),6);
    // The larger byte-full schedule must not rebuild every growing historical
    // prefix quadratically. Check bootstrap, a middle cut and the final restart
    // here; joined() checks every actual physical checkpoint independently.
    let cuts=[0,1,2,3,o.len()/2,o.len()-2];
    for i in cuts {combined::checkpoint_combined(&b[..o[i]],rolling_file::desktop_limits())
        .expect("ordinary source belongs to schema2's selected profile at a restart cut");}
    assert!(matches!(combined::checkpoint_combined(&b,rolling_file::desktop_limits()),
        Err(combined::Error::Checkpoint("TRAILER has no successor checkpoint"))),
        "terminal source has no next-chunk restart checkpoint");
    eprintln!("schema2 ordinary-first {recipe}: {} real draws, {} original chunks",draws.len(),o.len()-1);r
}
fn public_profile()->bool{match std::env::var("PS3DK_PROF_RUNTIME_SCHEMA2_PUBLIC"){
    Err(std::env::VarError::NotPresent)=>false,Ok(s) if s=="1"=>true,_=>panic!("public profile must be absent or 1")}}
fn file_limit()->usize{if public_profile(){32<<20}else{16<<20}}
fn limits()->rolling_store::Limits{rolling_store::Limits{max_file_bytes:file_limit() as u64,max_slot_bytes:if public_profile(){4<<20}else{2<<20}}}
fn source(r:&Run)->(Vec<u8>,Vec<usize>,Box<ps3dk_prof::Analysis>){
    let b=bounded(&r.dir.join("accepted-original.prof"),16<<20);let a=analyse(&b);let o=offsets(&b);
    assert_eq!((0..3).map(|i|u32::from_le_bytes(b[o[i]..o[i]+4].try_into().unwrap())).collect::<Vec<_>>(),vec![1,2,17]);
    (b,o,a)
}
fn joined(r:&Run,b:&[u8],o:&[usize],name:&str)->rolling_file::CombinedFile{
    let bytes=bounded(&r.dir.join(name),file_limit());let p=rolling_store::read(&bytes,limits()).unwrap();
    if public_profile(){
        let clock=analyse(b).capture.clock.clone().unwrap();
        assert_eq!(&p.config.session[..8],&clock.tb_at_init.to_le_bytes());
        assert_ne!(&p.config.session[8..],&[0;8],"exclusive path/build namespace");
    }else{assert_eq!(p.config.session,[0x52;16]);}
    assert_eq!(p.config.format_version,2);
    assert_eq!(p.view.bound_payload_bytes,(o[p.view.next_chunk as usize]-16) as u64);
    assert_eq!(p.view.evicted_payload_bytes,(o[p.view.first_chunk as usize]-16) as u64);
    for s in &p.segments{
        assert_eq!(s.payload,&b[o[s.first_chunk as usize]..o[s.next_chunk as usize]],"actual accepted original interval");
        if &s.checkpoint[8..12]==b"\x03\x00\x80\x00"{
            // Support is intentionally projected, so it cannot equal an
            // unprojected whole-prefix checkpoint. Check every retained raw
            // anchor against its actual original, then let the independent
            // reader verify every deterministic checkpoint transition below.
            // Own-next policy: every emitted nonterminal checkpoint is compacted
            // at its own next chunk, never at the oldest retained floor.
            assert_eq!(u64::from_le_bytes(s.checkpoint[80..88].try_into().unwrap()),s.first_chunk);
            // Byte-for-byte writer parity with an independent Rust replay of
            // the same accepted original prefix (first, middle and last slot,
            // so the long schedules do not replay every prefix quadratically).
            let k=p.segments.iter().position(|x|std::ptr::eq(x,s)).unwrap();
            if k==0||k==p.segments.len()/2||k+1==p.segments.len(){
                let replay=combined::checkpoint_combined_projected(&b[..o[s.first_chunk as usize]],rolling_file::desktop_limits()).unwrap();
                assert_eq!(s.checkpoint,replay.as_slice(),"C own-next checkpoint equals the Rust per-chunk replay");
            }
            let mut at=144;let n=u32::from_le_bytes(s.checkpoint[132..136].try_into().unwrap());
            for _ in 0..n{
                let len=u32::from_le_bytes(s.checkpoint[at..at+4].try_into().unwrap()) as usize;at+=4;
                let raw=&s.checkpoint[at..at+len];let index=u64::from_le_bytes(raw[8..16].try_into().unwrap()) as usize;
                assert!(index<s.first_chunk as usize);
                assert_eq!(raw,&b[o[index]..o[index+1]],"projected support is an exact original chunk");at+=len;
            }
            assert_eq!(at,144+u32::from_le_bytes(s.checkpoint[136..140].try_into().unwrap()) as usize);
        }else{
            let cp=combined::checkpoint_combined(&b[..o[s.first_chunk as usize]],rolling_file::desktop_limits()).unwrap();
            assert_eq!(s.checkpoint,cp.as_slice(),"independent C checkpoint equals original-prefix Rust state");
            assert_eq!(&s.checkpoint[..12],b"PFRMIX01\x02\x00\x60\x00");
        }
    }
    rolling_file::read_combined(&bytes,limits(),rolling_file::desktop_limits(),combined::ReportOptions{
        spu_profile_table:&[],causal_limits:Default::default()}).expect("actual schema2 retained physical/semantic join")
}
fn terminal(r:&Run,b:&[u8],o:&[usize],state:u32)->rolling_file::CombinedFile{
    let boot=joined(r,b,o,"ready.profr");assert_eq!((boot.store_view.first_chunk,boot.store_view.next_chunk,boot.store_view.state),(0,3,1));
    let j=joined(r,b,o,"capture.profr");assert_eq!(j.store_view.state,state);
    assert_eq!(j.store_view.next_chunk,(o.len()-1) as u64);assert_eq!(r.meta["next"],j.store_view.next_chunk);
    assert_eq!(r.meta["bytes"],j.store_view.bound_payload_bytes);assert_eq!(r.meta["revision"],j.store_view.generation);
    assert_eq!(j.retained.original_header.as_slice(),&b[..16]);j
}

#[test]
#[ignore="requires prebuilt actual native-provider/schema2 runtime fixture"]
fn actual_schema2_append_control_validates_native_ring_and_fifo(){let _=append("rotation");}

#[test]
#[ignore="requires prebuilt actual native-provider/schema2 runtime fixture"]
fn actual_schema2_runtime_preserves_ring_bytes_and_rotates_useful_output(){
    let _ordinary=append("rotation");let r=run("rotation");let (b,o,a)=source(&r);
    assert!(a.capture.trailer.is_some());let actual=a.capture.auto.as_ref().unwrap();
    assert_eq!(actual.ops.iter().filter(|x|x.covered()).count(),47_020);
    assert_eq!(r.meta["acks"],actual.ops.len());assert_eq!(r.meta["ring_acks"],47_020);
    let j=terminal(&r,&b,&o,2);assert!(j.retained.terminal&&j.store_view.evicted_segments>=3);
    let auto=j.retained.auto.as_ref().unwrap();assert!(auto.prefix.ops>0&&auto.retained.ops>0);
    assert_eq!(auto.cumulative.ops,actual.ops_parsed);assert!(auto.ops.iter().any(|x|x.value.covered()&&x.value.raw_valid()&&x.value.ring.is_some()));
    assert!(j.retained.auto_report.as_ref().unwrap().intervals.iter().any(|x|x.raw.is_some()),"useful retained RSX raw interval, no invented PPU mapping");
    let fifo=j.retained.fifo.as_ref().unwrap();assert!(fifo.cumulative.events>0&&!fifo.accounting.is_empty());
    assert!(fifo.accounting.iter().any(|x|x.reasons==0&&x.prepared>0));
}

#[test]
#[ignore="requires prebuilt actual native-provider/schema2 runtime fixture"]
fn actual_schema2_resource_refusal_precedes_metadata_ack_and_main_retirement(){
    let _ordinary=append("resource");let r=run("resource");let (b,o,a)=source(&r);
    assert!(a.capture.trailer.is_none());let auto=a.capture.auto.as_ref().unwrap();
    assert_eq!(auto.ops.iter().filter(|x|x.covered()).count(),10,"real useful first frame accepted before later build-entry refusal");
    assert_eq!(r.meta["draws"],31);assert_eq!(r.meta["ring_acks"],10);assert_eq!(r.meta["acks"],auto.ops.len());
    assert_eq!(r.meta["refusals"],1);assert_eq!(r.meta["refused_type"],35);
    assert_eq!(r.meta["refused_next"],r.meta["next"]);assert_eq!(r.meta["refused_bytes"],r.meta["bytes"]);
    let j=terminal(&r,&b,&o,3);assert_eq!(j.store_view.reason,3);assert!(!j.retained.terminal);
    let stopped=joined(&r,&b,&o,"automatic-before-public.profr");
    assert_eq!((stopped.store_view.state,stopped.store_view.reason),(3,3));
    assert_eq!(bounded(&r.dir.join("automatic-before-public.profr"),file_limit()),bounded(&r.dir.join("capture.profr"),file_limit()),"public shutdown must not recommit automatic stop");
}

#[test]
#[ignore="requires prebuilt actual native-provider/schema2 runtime fixture"]
fn actual_schema2_partial_drains_preserve_one_pending_payload(){
    let r=run("batching");let (b,o,a)=source(&r);
    assert_eq!(a.capture.auto.as_ref().unwrap().ops.iter().filter(|x|x.covered()).count(),960);
    let j=terminal(&r,&b,&o,2);
    assert_eq!(j.store_view.evicted_segments,0);
    assert_eq!(j.store_view.generation,4,"bootstrap, payload, terminal root only");
    assert_eq!(j.retained.auto.as_ref().unwrap().prefix.ops,0);
    assert_eq!(j.retained.auto.as_ref().unwrap().cumulative.ops,r.meta["acks"].as_u64().unwrap());
}

#[test]
#[ignore="requires prebuilt actual native-provider/schema2 runtime fixture"]
fn actual_projected_runtime_exceeds_2000_frames_without_capacity_loss(){
    let r=run("long");let (b,o,a)=source(&r);let actual=a.capture.auto.as_ref().unwrap();
    assert!(actual.frames_parsed>=2000);assert!(actual.final_seen);
    let mut visits=0u64;let mut timed=0u64;let mut room=0u64;
    for row in &actual.frames{for kind in &row.rows{
        visits+=kind[0];timed+=kind[1];room+=kind[2];
        assert!(kind[3..].iter().all(|&n|n==0),"no capacity or invalid-evidence exclusions");
    }assert_eq!(row.ring_full,[0;3]);assert_eq!(row.ring_state,[0;3]);assert_eq!(row.outside_timed,[0;3]);}
    assert_eq!(visits,timed+room);assert_eq!(r.meta["draws"],visits);assert_eq!(r.meta["ring_acks"],timed);
    let j=terminal(&r,&b,&o,2);byte_full_rotations(&r);
    let retained=j.retained.auto.as_ref().unwrap();
    assert_eq!(retained.cumulative.frames,actual.frames_parsed);
    assert_eq!(retained.cumulative.ops,actual.ops_parsed);
    assert_eq!(retained.cumulative.clocks,actual.clocks_parsed);
    // Analysis retains original source records; the writer's live support is
    // independently bounded and compared byte-for-byte in joined(). Sparse
    // runs need not evict anything merely because they passed 2000 frames.
    assert!(retained.frames.len()<=actual.frames.len());
    assert_eq!(r.meta["refusals"],0);
}

fn byte_full_rotations(r:&Run){
    let bytes=bounded(&r.dir.join("capture.profr"),file_limit());
    let p=rolling_store::read(&bytes,limits()).unwrap();
    assert_eq!(r.meta["revision"],p.view.next_segment+2,"initial root + commits + terminal root");
    for pair in p.segments.windows(2){
        if pair[0].segment==0{continue;} // Explicit bootstrap flush.
        let next_bytes=u32::from_le_bytes(pair[1].payload[4..8].try_into().unwrap()) as usize+20;
        assert!(pair[0].payload.len()+next_bytes>p.config.payload_bytes as usize,
            "segment {} committed before the next complete chunk needed room",pair[0].segment);
    }
    assert!(p.segments.last().unwrap().payload.len()<=p.config.payload_bytes as usize);
}

#[test]
#[ignore="requires prebuilt actual native-provider/schema2 runtime fixture"]
fn actual_projected_runtime_recycles_2000_short_windows(){
    let r=run("windows");let (b,o,a)=source(&r);let actual=a.capture.auto.as_ref().unwrap();
    assert!(actual.frames_parsed>=2000);assert!(actual.final_seen);
    assert!(actual.clocks_parsed>=4*16,"exercise at least four times the configured anchor capacity");
    assert_eq!(r.meta["refusals"],0);
    let j=terminal(&r,&b,&o,2);byte_full_rotations(&r);
    let retained=j.retained.auto.as_ref().unwrap();
    assert_eq!(retained.cumulative.frames,actual.frames_parsed);
    assert_eq!(retained.cumulative.ops,actual.ops_parsed);
    assert_eq!(retained.cumulative.clocks,actual.clocks_parsed);
    assert!(retained.frames.len()<=actual.frames.len());
    assert_eq!(j.retained.fifo.as_ref().unwrap().unavailable_window_rows,0);
    let bytes=bounded(&r.dir.join("capture.profr"),file_limit());let physical=rolling_store::read(&bytes,limits()).unwrap();
    // The earliest slot may still be the bootstrap now that history cannot
    // force commits. Check retirement at the latest actual restart instead.
    let cp=physical.segments.last().unwrap().checkpoint;
    assert!(u64::from_le_bytes(cp[88..96].try_into().unwrap())>1000,"window declarations really retired");
    assert_eq!(combined::advance_combined(cp,&[],rolling_file::desktop_limits()).unwrap(),cp);
    for (at,value) in [(80,u64::MAX),(88,0),(88,u64::from_le_bytes(cp[88..96].try_into().unwrap())+1),
        (96,u64::MAX),(104,1u64<<63),(112,u64::from_le_bytes(cp[80..88].try_into().unwrap())),(120,1)]{
        let mut bad=cp.to_vec();bad[at..at+8].copy_from_slice(&value.to_le_bytes());
        let end=bad.len()-4;let crc=common::crc32(&bad[..end]);bad[end..].copy_from_slice(&crc.to_le_bytes());
        assert!(combined::advance_combined(&bad,&[],rolling_file::desktop_limits()).is_err(),"forged window checkpoint field {at}");
    }
}

#[test]
#[ignore="requires prebuilt real FIFO queue delayed-row fixture"]
fn actual_delayed_fifo_keeps_counts_without_inventing_window_bounds(){
    for case in ["late-retired","late-boundary"]{
        let r=run(case);let n=|key:&str|r.meta[key].as_u64().unwrap();
        assert!(n("release_frame")>n("held_frame")+16);assert!(n("retired_max")>=11);
        assert_eq!(n("state"),2);assert_eq!(r.meta["shutdown"],0);assert_eq!(r.meta["io_result"],0);
        let read=|name:&str|{let bytes=bounded(&r.dir.join(name),32<<20);
            rolling_file::read_combined(&bytes,rolling_store::Limits{max_file_bytes:32<<20,max_slot_bytes:4<<20},rolling_file::desktop_limits(),
                combined::ReportOptions{spu_profile_table:&[],causal_limits:Default::default()}).unwrap()};
        let at_release=read("late-release.profr");let f=at_release.retained.fifo.as_ref().unwrap();
        let row=f.accounting.iter().find(|row|row.seq==n("held_seq")).expect("released original row retained");
        assert!(f.unavailable_window_sequences.contains(&row.seq));
        assert!(!at_release.retained.ppu.windows.iter().any(|w|u64::from(w.original.id)==row.window));
        let raw=r.meta["original_row_hex"].as_str().unwrap();
        let bytes:Vec<_>=(0..raw.len()).step_by(2).map(|i|u8::from_str_radix(&raw[i..i+2],16).unwrap()).collect();
        let u64_at=|at|u64::from_le_bytes(bytes[at..at+8].try_into().unwrap());
        assert_eq!((row.prepared,row.published,row.wraps,row.seq),(u64_at(40),u64_at(48),u64_at(56),u64_at(64)));
        assert_eq!((row.window,row.frame),if case=="late-retired"{(u64_at(32),u64_at(24))}else{(n("retired_max"),n("retired_last"))});
        let final_=read("capture.profr");let f=final_.retained.fifo.as_ref().unwrap();
        assert_eq!(f.unavailable_window_rows,if case=="late-retired"{16}else{1});
        assert!(f.known_missing>0);assert!(n("first_after_seq")>n("held_seq")+16);
    }
}

#[test]
#[ignore="requires prebuilt real FIFO queue delayed-row fixture"]
fn actual_delayed_fifo_still_refuses_future_zero_and_past_bound_rows(){
    for case in ["late-future","late-zero","late-past-boundary"]{
        let r=run(case);assert_ne!(r.meta["state"],2);assert_ne!(r.meta["shutdown"],0);
        assert_eq!(r.meta["invalid_type"],43);assert_eq!(r.meta["invalid_seq"],r.meta["held_seq"]);
        assert_eq!(r.meta["invalid_rc"],r.meta["io_result"]);assert_ne!(r.meta["invalid_rc"],0);
    }
}
