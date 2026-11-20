//! AUTO snapshots use the reader's map and execution tokens, not EXT calibration or build tokens.
use std::{path::PathBuf, sync::Arc};
use ps3dk_prof::auto_rsx::{self as ar, MapState};
use ps3dk_prof_viewer::{capture::{FrameSnapshot, Premise}, capture_ctl::{Captures, Home}, model::Document};

fn fixture() -> Vec<u8> {
    std::fs::read(PathBuf::from(env!("CARGO_MANIFEST_DIR")).join("../ps3dk-prof/tests/fixtures/auto/console-r2.prof")).unwrap()
}

fn freeze(d: &Document, bytes: &[u8], frame: u64) -> Arc<FrameSnapshot> {
    let mut c = Captures::new("auto-snapshot", Home::Live);
    c.bind_source(bytes);
    c.capture(d, Some(1), Some(frame), Premise { w32: true, show_extrapolated: false }, "test",
        &mut |n| Some(bytes[..n as usize].to_vec())).unwrap()
}

#[test]
fn auto_snapshot_uses_records_and_reader_bounds() {
    let bytes = fixture();
    let d = Document::from_bytes("console-r2.prof".into(), &bytes);
    let s = freeze(&d, &bytes, 10);
    let l = s.lanes.iter().find(|l| l.id.code() == "rsx.auto").expect("AUTO lane missing");
    let inv = d.analysis().unwrap().capture.auto.as_ref().unwrap();
    let MapState::Qualified(map) = d.auto.as_ref().unwrap().report.map.as_ref().unwrap() else { panic!("map") };
    let expected: Vec<_> = inv.ops.iter().filter(|o| o.kind != ar::KIND_FRAME && o.token() == Some((1,10)) && o.raw_valid())
        .filter(|o| ar::raw_to_ppu_bounds(&inv.clocks, map, o.raw_begin).is_some() && ar::raw_to_ppu_bounds(&inv.clocks,map,o.raw_end).is_some()).collect();
    assert!(!expected.is_empty());
    assert_eq!(l.end - l.start, expected.len() as u32);
    for (b,o) in s.bars[l.start as usize..l.end as usize].iter().zip(expected) {
        let begin = ar::raw_to_ppu_bounds(&inv.clocks,map,o.raw_begin).unwrap();
        let end = ar::raw_to_ppu_bounds(&inv.clocks,map,o.raw_end).unwrap();
        assert_eq!(b.b_lo.f(), Some(begin.0 as f64 - s.tb as f64));
        assert_eq!(b.b_hi.f(), Some(begin.1 as f64 - s.tb as f64));
        assert_eq!(b.e_lo.f(), Some(end.0 as f64 - s.tb as f64));
        assert_eq!(b.e_hi.f(), Some(end.1 as f64 - s.tb as f64));
        assert!(s.labels[b.label as usize].contains(&format!("op {} ", o.id)));
    }
    let ppu = s.lanes.iter().find(|l| l.id.code() == "ppu.1").unwrap();
    assert_eq!(ppu.status.word(), "NO ZONES IN THIS BUILD");
    let domain = inv.config.as_ref().unwrap().attached().unwrap().domain;
    let ext = s.lanes.iter().find(|l| l.id.code() == format!("rsx.{domain}")).unwrap();
    assert_eq!(ext.status.word(), "TIMED BY AUTO RECORDS");
    let b = ps3dk_prof_viewer::capture::bookmark_of(&s,1).unwrap();
    let h = ps3dk_prof_viewer::capture::Header { file: "console-r2.prof".into(), bytes: bytes.len() as u64,
        sha256: ps3dk_prof_viewer::sha256::digest(&bytes) };
    let text = ps3dk_prof_viewer::capture::render(&h,&[b.clone()]).unwrap();
    let parsed = ps3dk_prof_viewer::capture::parse(text.as_bytes());
    assert!(parsed.refused.is_empty(),"{:?}",parsed.refused);
    assert_eq!(parsed.records,vec![b]);
    assert!(s.retained_bytes() <= s.charge);
}

#[test]
fn auto_snapshot_unqualified_map_does_not_invent_positions() {
    let bytes = fixture();
    let mut d = Document::from_bytes("console-r2.prof".into(), &bytes);
    d.auto.as_mut().unwrap().report.map = Some(MapState::Unqualified("test missing clock".into()));
    let s = freeze(&d, &bytes, 10);
    let l = s.lanes.iter().find(|l| l.id.code() == "rsx.auto").expect("AUTO lane missing");
    assert_eq!(l.start, l.end);
    assert_eq!(l.status.code(), "UNMAPPABLE");
    assert!(l.evidence.lines().join(" ").contains("clock"));
}

#[test]
fn execution_token_wins_over_build_frame_and_ppu_overlap() {
    let bytes = fixture();
    let mut d = Document::from_bytes("console-r2.prof".into(), &bytes);
    let ps3dk_prof_viewer::model::DocState::Analysed(a) = &mut d.state else { panic!("analysis") };
    let inv = a.capture.auto.as_mut().unwrap();
    let mut op = inv.ops.iter().find(|o| o.kind == ar::KIND_CLEAR && o.token() == Some((1,10))).unwrap().clone();
    op.build = Some((1,9));
    op.flags |= ar::OP_SEGMENT;
    let mut other = op.clone();
    other.id += 10000;
    other.frame = 9;
    other.build = Some((1,10));
    inv.ops = vec![op,other];
    let s = freeze(&d, &bytes, 10);
    let l = s.lanes.iter().find(|l| l.id.code()=="rsx.auto").expect("AUTO lane missing");
    assert_eq!(l.end-l.start,1,"a segment's execution token, not its build token, selects it");
}

#[test]
fn replay_or_invalid_timestamps_never_become_bars() {
    let bytes = fixture();
    for replay in [false,true] {
        let mut d = Document::from_bytes("console-r2.prof".into(), &bytes);
        if replay {
            d.auto.as_mut().unwrap().report.status.as_mut().unwrap().flags |= ar::ST_SEGMENT_REPLAY;
        } else {
            let ps3dk_prof_viewer::model::DocState::Analysed(a) = &mut d.state else { panic!("analysis") };
            for o in &mut a.capture.auto.as_mut().unwrap().ops { o.flags |= ar::OP_CLOCK_INVALID; }
        }
        let s = freeze(&d, &bytes, 10);
        let l = s.lanes.iter().find(|l| l.id.code()=="rsx.auto").expect("AUTO lane missing");
        assert_eq!(l.start,l.end);
        assert_ne!(l.status.code(),"OBSERVED");
    }
}

#[test]
fn auto_bars_respect_snapshot_budget() {
    let bytes=fixture();
    let d=Document::from_bytes("console-r2.prof".into(),&bytes);
    let mut c=Captures::new("cap",Home::Live);
    c.bind_source(&bytes);
    c.caps.bars=0;
    let r=c.capture(&d,Some(1),Some(10),Premise {w32:true,show_extrapolated:false},"test",&mut |_|Some(bytes.clone()));
    assert!(r.is_err(),"AUTO must share the snapshot bar cap");
    assert!(c.list.is_empty());
}

#[test]
fn ext_events_are_not_hidden_by_auto_identity() {
    let bytes=fixture();
    let mut d=Document::from_bytes("console-r2.prof".into(),&bytes);
    let ps3dk_prof_viewer::model::DocState::Analysed(a)=&mut d.state else { panic!("analysis") };
    let domain=a.capture.auto.as_ref().unwrap().config.as_ref().unwrap().attached().unwrap().domain;
    let ext=a.capture.ext.as_mut().unwrap().sources.iter_mut().find(|s|s.key==ps3dk_prof::ext::SourceKey::Gpu(domain)).unwrap();
    ext.events_total=1; // Delegation must require zero actual EXT events, not merely a matching domain id.
    let s=freeze(&d,&bytes,10);
    assert_ne!(s.lanes.iter().find(|l|l.id.code()==format!("rsx.{domain}")).unwrap().status.code(),"AUTO_RECORDS");
}

#[test]
fn unclaimed_overlap_and_support_limits() {
    let bytes=fixture();
    let mut d=Document::from_bytes("console-r2.prof".into(),&bytes);
    let ps3dk_prof_viewer::model::DocState::Analysed(a)=&mut d.state else { panic!("analysis") };
    let inv=a.capture.auto.as_mut().unwrap();
    let mut op=inv.ops.iter().find(|o|o.kind==ar::KIND_CLEAR&&o.token()==Some((1,10))).unwrap().clone();
    op.flags &= !ar::OP_TOKEN;
    let mut outside=op.clone(); outside.id+=10000; outside.raw_begin=0; outside.raw_end=1;
    inv.ops=vec![op,outside];
    let s=freeze(&d,&bytes,10);
    let l=s.lanes.iter().find(|l|l.id.code()=="rsx.auto").unwrap();
    assert_eq!(l.end-l.start,1);
    assert_eq!(l.evidence.possible,1);
    assert_eq!(l.evidence.auto_unplaced,1);
    assert_eq!(s.bars[l.start as usize].class,ps3dk_prof_viewer::capture::Class::Possible);
}

#[test]
fn nested_ranges_keep_recorded_depth_without_inventing_draws() {
    let bytes=fixture();
    let mut d=Document::from_bytes("console-r2.prof".into(),&bytes);
    let ps3dk_prof_viewer::model::DocState::Analysed(a)=&mut d.state else { panic!("analysis") };
    let inv=a.capture.auto.as_mut().unwrap();
    let mut outer=inv.ops.iter().find(|o|o.kind==ar::KIND_CLEAR&&o.token()==Some((1,10))).unwrap().clone();
    outer.kind=ar::KIND_RANGE; outer.depth=1; outer.args[0]=1;
    let mut inner=outer.clone(); inner.id+=10000; inner.depth=2;
    inner.raw_end=(inner.raw_begin+inner.raw_end)/2;
    inv.ops=vec![outer,inner];
    let s=freeze(&d,&bytes,10);
    let l=s.lanes.iter().find(|l|l.id.code()=="rsx.auto").unwrap();
    let bars=&s.bars[l.start as usize..l.end as usize];
    assert_eq!(bars.len(),2);
    assert_eq!((bars[0].depth,bars[1].depth),(0,1));
    assert!(bars.iter().all(|b|s.labels[b.label as usize].contains("RANGE")&&b.exclusive.is_none()));
}

#[test]
fn mossbrook_ranges_only_snapshot() {
    let Ok(path) = std::env::var("PS3DK_SNAPSHOT_CAPTURE") else { return };
    let bytes = std::fs::read(&path).unwrap();
    let d = Document::from_bytes(path.into(), &bytes);
    for frame in [1112,1134] {
        let s = freeze(&d, &bytes, frame);
        let l = s.lanes.iter().find(|l| l.id.code() == "rsx.auto").expect("AUTO lane missing");
        let inv=d.analysis().unwrap().capture.auto.as_ref().unwrap();
        let recorded=inv.ops.iter().filter(|o|o.token()==Some((1,frame))&&o.kind==ar::KIND_RANGE&&o.raw_valid()).count();
        assert!(recorded>=16);
        assert_eq!(l.end-l.start,recorded as u32);
        for b in &s.bars[l.start as usize..l.end as usize] { assert!(s.labels[b.label as usize].contains("RANGE")); }
        assert_eq!(s.lanes.iter().find(|l| l.id.code()=="ppu.1").unwrap().status.word(),"NO ZONES IN THIS BUILD");
    }
}
