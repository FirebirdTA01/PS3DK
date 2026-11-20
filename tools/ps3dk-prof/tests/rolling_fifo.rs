//! Independent ordinary FIFO fixtures control every original chunk cut.
mod common;
mod fifo_support;
use common::*;
use fifo_support::*;
use ps3dk_prof::{fifo, windows};
use ps3dk_prof::fifo::rolling::{self, Counts, Error, FrameRun, Input, Limits, Refs, WorkBudget};
use std::collections::BTreeSet;

struct Context { cap: ps3dk_prof::Capture, domains: BTreeSet<u32>, declared: BTreeSet<u32> }
impl Context {
    fn new(windowed: bool) -> Self {
        Self::at(windowed,4)
    }
    fn at(windowed: bool, frontier: usize) -> Self {
        let mut f=base(65 | if windowed {512} else {0});f.chunks.truncate(frontier.min(4));
        Self { cap: reader::read(&f.bytes()).unwrap(),
            domains: if frontier>=4 {BTreeSet::from([D])}else{BTreeSet::new()},
            declared: if frontier>=3 {BTreeSet::from([OWNER])}else{BTreeSet::new()} }
    }
    fn refs(&self) -> Refs<'_> { Refs {capture:&self.cap,rsx_domains:&self.domains,declared:&self.declared} }
}
fn inputs(chunks:&[Chunk], start:usize, end:usize) -> Vec<Input<'_>> {
    chunks.iter().enumerate().take(end).skip(start).filter(|(_,c)|matches!(c.ty,31..=33|43))
        .map(|(i,c)|Input::Fifo{index:i as u64,kind:c.ty,payload:&c.payload,windows:None}).collect()
}
fn account(frame:u64,seq:u64,window:u64,reasons:u32)->Chunk {
    let mut b=Buf::default();
    b.u16(1).u16(if reasons==0 {3}else{2}).u32(1).u32(1).u32(D).u32(OWNER).u32(reasons)
        .u64(frame).u64(window).u64(if reasons==0 {120}else{0}).u64(if reasons==0 {112}else{0})
        .u64(if reasons==0 {2}else{0}).u64(seq).u64(0);
    chunk(43,b.0)
}
fn fixture()->File {
    let mut f=base(65);
    for e in wrap(1,1,100,10,20) { f.chunks.push(events(&[e])); }
    f.chunks.push(account(7,1,0,0));
    f.chunks.push(St::new(2,6,6,0,6,1,1,0).chunk());
    for mut e in wrap(7,2,200,10,20) {e.frame=8; f.chunks.push(events(&[e]));}
    f.chunks.push(account(8,3,0,0));
    f.chunks.push(St::new(1,12,12,0,12,2,2,0).chunk());f
}
fn counts(inv:&fifo::Inventory)->Counts {
    Counts{events:inv.parsed_events,enter:inv.visible_enter,exit:inv.visible_exit,statuses:inv.statuses,accounting_rows:inv.accounting.parsed}
}
fn prefix_counts(f:&File,cut:usize)->Counts {
    if cut<5 {return Counts::default();}
    let mut prefix=f.clone();prefix.chunks.truncate(cut);
    counts(reader::read(&prefix.bytes()).unwrap().fifo.as_ref().unwrap())
}
fn repair(cp:&mut [u8]) {let n=cp.len()-4;let c=crc32(&cp[..n]);cp[n..].copy_from_slice(&c.to_le_bytes());}
fn w64(cp:&mut [u8],at:usize,n:u64){cp[at..at+8].copy_from_slice(&n.to_le_bytes());}
fn checkpoint(f:&File,cut:usize,ctx:&Context)->Vec<u8> {
    let early;
    let refs=if cut<4 {early=Context::at(ctx.cap.windows_flag,cut);early.refs()}else{ctx.refs()};
    rolling::checkpoint(refs,&inputs(&f.chunks,0,cut),cut as u64,Limits::default()).expect("FIFO rolling checkpoint unavailable")
}

#[test]
fn every_original_cut_preserves_strict_census_and_retained_interior_phases() {
    let f=fixture();let ctx=Context::new(false);let cap=reader::read(&f.bytes()).unwrap();
    let ordinary=cap.fifo.as_ref().unwrap();let report=fifo::analyze(&cap);
    for cut in 0..=f.chunks.len() {
        let cp=checkpoint(&f,cut,&ctx);
        let r=rolling::read(ctx.refs(),&cp,&inputs(&f.chunks,cut,f.chunks.len()),f.chunks.len() as u64,Limits::default()).unwrap();
        let p=prefix_counts(&f,cut);
        assert_eq!(r.prefix,p,"cut {cut}");assert_eq!(r.cumulative,counts(ordinary),"cut {cut}");
        assert_eq!(r.retained.events,ordinary.parsed_events-p.events);assert_eq!(r.retained.accounting_rows,2-p.accounting_rows);
        assert_eq!(r.status,ordinary.last_status);assert!(r.final_seen);
        assert_eq!((r.first_chunk,r.next_chunk),(cut as u64,f.chunks.len() as u64));assert_eq!(r.known_missing,1);
        for (wrap_id,first_index) in [(1,5),(2,13)] {
            if cut<=first_index {
                assert_eq!(r.wraps.iter().find(|w|w.wrap_id==wrap_id),report.wraps.iter().find(|w|w.wrap_id==wrap_id),"cut {cut}");
            }
        }
        // Prefix support never appears as retained accounting measurements.
        assert_eq!(r.accounting.len() as u64,r.retained.accounting_rows);
    }
}

#[test]
fn crossing_outer_and_phase_are_censored_but_later_inner_phase_survives() {
    let f=fixture();let ctx=Context::new(false);let cut=7; // ENTER and phase1 BEGIN are prefix-only.
    let cp=checkpoint(&f,cut,&ctx);
    let r=rolling::read(ctx.refs(),&cp,&inputs(&f.chunks,cut,f.chunks.len()),f.chunks.len() as u64,Limits::default()).unwrap();
    let w=r.wraps.iter().find(|w|w.wrap_id==1).unwrap();
    assert!(!matches!(w.outer,fifo::Phase::Complete{..}));assert!(!matches!(w.phase1,fifo::Phase::Complete{..}));
    assert!(matches!(w.phase2,fifo::Phase::Complete{min:20,max:22,..}));
    assert_eq!(r.boundary_frames,vec![FrameRun{owner:OWNER,first:7,last:7}]);
    assert!(!r.prefix_unassociated); // Frame7 is explicitly boundary-affected, never an empty zero-wait frame.
}

#[test]
fn suffix_status_still_uses_prefix_last_written_sequence_and_counts() {
    let f=fixture();let ctx=Context::new(false);let cut=f.chunks.len()-1;let cp=checkpoint(&f,cut,&ctx);
    let r=rolling::read(ctx.refs(),&cp,&inputs(&f.chunks,cut,f.chunks.len()),f.chunks.len() as u64,Limits::default()).unwrap();
    assert_eq!(r.retained.events,0);assert_eq!(r.cumulative.events,12);assert!(r.wraps.is_empty());
    assert_eq!(r.boundary_frames,vec![FrameRun{owner:OWNER,first:7,last:8}]);
    for offset in [16,40,48,56,64] { // written, last seq, started, completed, open
        let mut bad=f.clone();bad.chunks[cut].payload[offset]^=1;
        assert!(reader::read(&bad.bytes()).is_err(),"strict control offset {offset}");
        assert!(matches!(rolling::read(ctx.refs(),&cp,&inputs(&bad.chunks,cut,bad.chunks.len()),bad.chunks.len() as u64,Limits::default()),Err(Error::Malformed(_))));
    }
}

#[test]
fn exact_advance_is_canonical_and_forged_plausible_prefix_is_not_ancestry() {
    let f=fixture();let ctx=Context::new(false);
    for cut in 0..=f.chunks.len() {
        let first=checkpoint(&f,cut,&ctx);
        for end in cut..=f.chunks.len() {
            let context=Context::at(false,end);
            let advanced=rolling::advance(context.refs(),&first,&inputs(&f.chunks,cut,end),end as u64,Limits::default()).unwrap();
            assert_eq!(advanced,checkpoint(&f,end,&ctx),"{cut}->{end}");
        }
    }
    let original=checkpoint(&f,7,&ctx);let mut forged=original.clone();
    w64(&mut forged,24,8);repair(&mut forged); // individually plausible original cut, wrong canonical next frontier
    assert_ne!(forged,original);
    assert_eq!(rolling::advance(ctx.refs(),&original,&[],7,Limits::default()).unwrap(),original);
}

#[test]
fn saved_phase_frozen_fields_and_valid_clock_fence_remain_strict() {
    let f=fixture();let ctx=Context::new(false);let cp=checkpoint(&f,7,&ctx);
    for offset in [16,24,32,72,84] { // seq, wrap, tb-before, tail, request (within one-event payload)
        let mut bad=f.clone();bad.chunks[7].payload[offset]^=0x40;
        assert!(reader::read(&bad.bytes()).is_err(),"ordinary malformed offset {offset}");
        assert!(matches!(rolling::read(ctx.refs(),&cp,&inputs(&bad.chunks,7,bad.chunks.len()),bad.chunks.len() as u64,Limits::default()),Err(Error::Malformed(_))));
    }
    let mut a=ev(1,1,1,100,100);a.flags|=2;
    let b=ev(2,1,2,101,102);let mut c=ev(3,1,3,0,0);c.flags|=2;
    let d=ev(4,1,4,90,91);
    let bad=with(vec![events(&[a]),events(&[b]),events(&[c]),events(&[d])]);
    assert!(reader::read(&bad.bytes()).is_err());
    let cp=checkpoint(&bad,8,&ctx);
    assert!(matches!(rolling::read(ctx.refs(),&cp,&inputs(&bad.chunks,8,9),9,Limits::default()),Err(Error::Malformed(_))));
}

#[test]
fn saturation_and_final_fences_cannot_be_reset_by_a_cut() {
    let ctx=Context::new(false);
    let f=with(vec![St::new(6,0,0,0,0,0,0,0).chunk(),St::new(2,0,0,0,0,0,0,0).chunk()]);
    assert!(reader::read(&f.bytes()).is_err());let cp=checkpoint(&f,6,&ctx);
    assert!(matches!(rolling::read(ctx.refs(),&cp,&inputs(&f.chunks,6,7),7,Limits::default()),Err(Error::Malformed(_))));
    let f=with(vec![St::new(1,0,0,0,0,0,0,0).chunk(),account(7,1,0,0)]);
    assert!(reader::read(&f.bytes()).is_err());let cp=checkpoint(&f,6,&ctx);
    assert!(matches!(rolling::read(ctx.refs(),&cp,&inputs(&f.chunks,6,7),7,Limits::default()),Err(Error::Malformed(_))));
}

fn begin()->Chunk {let mut b=Buf::default();b.u16(1).u16(0).u32(1).u32(u32::MAX).u64(7).u8(1).u8(1).u16(0).u32(0).u32(0);chunk(41,b.0)}
fn end(last:u64)->Chunk {let mut b=Buf::default();b.u16(1).u16(0).u32(1).u8(1).u8(0).u16(0).u64(last).u32(0).u64(0);chunk(42,b.0)}
#[test]
fn accounting43_cursor_and_later_window_close_survive_eviction() {
    let ctx=Context::new(true);let config=Cfg::default().chunk();let row=account(8,5,1,0);
    let mut open=windows::Reader::new();let b=begin();open.chunk(41,&b.payload,&ctx.cap).unwrap();
    let prefix=[Input::Fifo{index:4,kind:31,payload:&config.payload,windows:None},Input::Fifo{index:6,kind:43,payload:&row.payload,windows:Some(&open)}];
    let cp=rolling::checkpoint(ctx.refs(),&prefix,7,Limits::default()).unwrap();
    let r=rolling::read(ctx.refs(),&cp,&[],7,Limits::default()).unwrap();
    assert_eq!((r.prefix.accounting_rows,r.prefix_known_missing,r.known_missing),(1,4,4));assert!(r.accounting.is_empty());
    for last in [7,8] {
        let mut w=windows::Reader::new();w.chunk(41,&b.payload,&ctx.cap).unwrap();let e=end(last);w.chunk(42,&e.payload,&ctx.cap).unwrap();
        let input=[Input::WindowClosed{index:7,window:w.windows.last().unwrap()}];
        let result=rolling::read(ctx.refs(),&cp,&input,8,Limits::default());
        assert_eq!(result.is_ok(),last==8);
        if last==7 {assert!(matches!(result,Err(Error::Malformed(_))));}
        let mut ordinary=with(vec![b.clone(),row.clone(),e]);ordinary.flags|=512;
        assert_eq!(reader::read(&ordinary.bytes()).is_ok(),last==8);
    }
    let next=account(9,(1u64<<62)+1,1,0);
    let suffix=[Input::Fifo{index:7,kind:43,payload:&next.payload,windows:Some(&open)}];
    let r=rolling::read(ctx.refs(),&cp,&suffix,8,Limits::default()).unwrap();
    assert_eq!(r.accounting.len(),1);assert_eq!(r.known_missing,(1u64<<62)-1);
    let duplicate=account(9,5,1,0);
    assert!(matches!(rolling::read(ctx.refs(),&cp,&[Input::Fifo{index:7,kind:43,payload:&duplicate.payload,windows:Some(&open)}],8,Limits::default()),Err(Error::Malformed(_))));
}

#[test]
fn repaired_checkpoint_crc_does_not_hide_impossible_typed_state() {
    let f=fixture();let ctx=Context::new(false);let original=checkpoint(&f,f.chunks.len(),&ctx);
    for (at,value,width) in [(8,2,4),(40,0x200,4),(44,u64::from(u32::MAX),4),(48,1,4),(52,1,4),
        (56,FREQ+1,8),(64,0,8),(72,13,8),(88,0,8),(112,99,8),(120,1,8)] {
        let mut cp=original.clone();if width==8 {w64(&mut cp,at,value)}else{cp[at..at+4].copy_from_slice(&(value as u32).to_le_bytes())}repair(&mut cp);
        assert!(matches!(rolling::read(ctx.refs(),&cp,&[],f.chunks.len() as u64,Limits::default()),Err(Error::Checkpoint(_)|Error::Context(_)|Error::Limit(_))),"offset {at}");
    }
    let mut cp=original.clone();cp[0]^=1;repair(&mut cp);
    assert!(matches!(rolling::read(ctx.refs(),&cp,&[],f.chunks.len() as u64,Limits::default()),Err(Error::Checkpoint(_))));
    for (at,value,width) in [(400,2,4),(404,1,4),(408,9,8),(416,6,8)] {
        let mut cp=original.clone();if width==8 {w64(&mut cp,at,value)}else{cp[at..at+4].copy_from_slice(&(value as u32).to_le_bytes())}repair(&mut cp);
        assert!(matches!(rolling::read(ctx.refs(),&cp,&[],f.chunks.len() as u64,Limits::default()),Err(Error::Checkpoint(_))),"run offset {at}");
    }
    // No checkpoints retain the prefix event history: only bounded membership,
    // one last phase event and one status, regardless of the number of wraps.
    assert!(original.len()<1024);
}

#[test]
fn membership_limits_count_sparse_runs_and_unassociated_prefix_is_explicit() {
    let ctx=Context::new(false);let mut f=base(65);
    for (i,frame) in [7,9,8].into_iter().enumerate() {
        let mut e=ev(1+i as u64*10,1+i as u64,1,100+i as u64*10,100+i as u64*10);e.frame=frame;f.chunks.push(events(&[e]));
    }
    let tight=Limits{membership_runs:1,..Limits::default()};
    assert!(matches!(rolling::checkpoint(ctx.refs(),&inputs(&f.chunks,0,7),7,tight),Err(Error::Limit(_))));
    let cp=checkpoint(&f,8,&ctx);let r=rolling::read(ctx.refs(),&cp,&[],8,tight).unwrap();
    assert_eq!(r.boundary_frames,vec![FrameRun{owner:OWNER,first:7,last:9}]);
    let mut e=ev(1,1,1,100,100);e.flags=0;e.frame_owner=0;e.frame=0;
    let f=with(vec![events(&[e])]);let cp=checkpoint(&f,6,&ctx);
    let r=rolling::read(ctx.refs(),&cp,&[],6,Limits::default()).unwrap();assert!(r.prefix_unassociated);assert!(r.boundary_frames.is_empty());
}

#[test]
fn budgets_order_context_and_transient_count_are_checked_without_partial_output() {
    let f=fixture();let ctx=Context::new(false);let cp=checkpoint(&f,5,&ctx);
    for limit in [Limits{checkpoint_bytes:63,..Limits::default()},Limits{retained_records:1,..Limits::default()},Limits{work_records:1,..Limits::default()}] {
        assert!(matches!(rolling::read(ctx.refs(),&cp,&inputs(&f.chunks,5,f.chunks.len()),f.chunks.len() as u64,limit),Err(Error::Limit(_))));
    }
    let one=inputs(&f.chunks,5,6);let twice=[one[0],one[0]];
    assert!(matches!(rolling::read(ctx.refs(),&cp,&twice,7,Limits::default()),Err(Error::Order)));
    assert!(matches!(rolling::read(ctx.refs(),&cp,&one,5,Limits::default()),Err(Error::Order)));
    let empty=BTreeSet::new();let refs=Refs{declared:&empty,..ctx.refs()};
    assert!(matches!(rolling::read(refs,&cp,&[],5,Limits::default()),Err(Error::Context(_)|Error::Malformed(_))));
    let bad=Input::Fifo{index:5,kind:34,payload:&[],windows:None};
    assert!(matches!(rolling::read(ctx.refs(),&cp,&[bad],6,Limits::default()),Err(Error::Unsupported(34))));
    let mut forged=f.chunks[5].clone();forged.payload[8..12].copy_from_slice(&u32::MAX.to_le_bytes());
    let bad=Input::Fifo{index:5,kind:32,payload:&forged.payload,windows:None};
    assert!(matches!(rolling::read(ctx.refs(),&cp,&[bad],6,Limits::default()),Err(Error::Malformed(_)|Error::Limit(_))));
}

#[test]
fn bootstrap_context_grows_without_importing_future_clock_or_owner() {
    let f=fixture();let initial=Context::at(false,0);
    let mut cp=rolling::checkpoint(initial.refs(),&[],0,Limits::default()).unwrap();
    for end in 0..=5 {
        let context=Context::at(false,end);
        let direct=rolling::checkpoint(context.refs(),&inputs(&f.chunks,0,end),end as u64,Limits::default()).unwrap();
        let start=end.saturating_sub(1);
        if end!=0 {cp=rolling::advance(context.refs(),&cp,&inputs(&f.chunks,start,end),end as u64,Limits::default()).unwrap();}
        assert_eq!(cp,direct);
        assert_eq!(u64::from_le_bytes(cp[56..64].try_into().unwrap()),if end>=2 {FREQ}else{0});
        assert_eq!(u32::from_le_bytes(cp[40..44].try_into().unwrap())&256!=0,end>=2);
    }
    let mut wrong=Context::new(false);wrong.cap.clock.as_mut().unwrap().freq_hz+=1;
    assert!(matches!(rolling::read(wrong.refs(),&cp,&[],5,Limits::default()),Err(Error::Context(_)|Error::Malformed(_))));
}

#[test]
fn huge_evicted_census_import_cost_is_typed_state_size_not_history_length() {
    let ctx=Context::new(false);let f=with(vec![events(&[ev(2,1,2,100,100)])]);
    let mut cp=checkpoint(&f,6,&ctx);let n=1u64<<40;
    w64(&mut cp,24,n+5); // metadata/config plus N genuine one-event chunks
    w64(&mut cp,32,2*n+1);w64(&mut cp,64,n); // plausible prior record work/census
    w64(&mut cp,224,2*n);w64(&mut cp,232,n); // last event seq and wrap, after 64-byte config
    repair(&mut cp);
    let limits=Limits{work_records:32,retained_records:0,..Limits::default()};
    let mut work=WorkBudget::new(32);
    let r=rolling::read_with_work(ctx.refs(),&cp,&[],n+5,limits,&mut work).unwrap();
    assert_eq!(r.prefix.events,n);assert_eq!(r.cumulative.events,n);assert!(r.wraps.is_empty());
    assert!(work.used()>0 && work.used()<=32);assert_eq!(work.used()+work.remaining(),32);
}

#[test]
fn actual_suffix_and_outer_chain_share_work_without_resetting_allowance() {
    let ctx=Context::new(false);let f=fixture();let cp=checkpoint(&f,5,&ctx);let cp2=checkpoint(&f,6,&ctx);
    let a=inputs(&f.chunks,5,6);let b=inputs(&f.chunks,6,7);
    let mut probe_a=WorkBudget::new(1000);let mut probe_b=WorkBudget::new(1000);
    rolling::read_with_work(ctx.refs(),&cp,&a,6,Limits::default(),&mut probe_a).unwrap();
    rolling::read_with_work(ctx.refs(),&cp2,&b,7,Limits::default(),&mut probe_b).unwrap();
    let mut shared=WorkBudget::new(probe_a.used()+probe_b.used()-1);
    rolling::read_with_work(ctx.refs(),&cp,&a,6,Limits::default(),&mut shared).unwrap();
    assert!(matches!(rolling::read_with_work(ctx.refs(),&cp2,&b,7,Limits::default(),&mut shared),Err(Error::Limit(_))));
    let mut many=WorkBudget::new(1000);
    let r=rolling::read_with_work(ctx.refs(),&cp,&inputs(&f.chunks,5,f.chunks.len()),f.chunks.len() as u64,Limits::default(),&mut many).unwrap();
    assert!(many.used()>=r.retained.events && many.used()>probe_a.used());
}

#[test]
fn full_u64_event_frame_membership_roundtrips_and_merges_at_max() {
    let ctx=Context::new(false);let mut f=base(65);
    for (seq,id,frame,t) in [(1,1,u64::MAX-1,100),(7,2,u64::MAX,200)] {
        let mut rows=wrap(seq,id,t,10,20);for e in &mut rows {e.frame=frame;}
        f.chunks.push(events(&rows));
    }
    f.chunks.push(St::new(1,12,12,0,12,2,2,0).chunk());
    let ordinary=reader::read(&f.bytes()).expect("ordinary FIFO accepts full-width event frame tokens");
    assert_eq!(ordinary.fifo.as_ref().unwrap().events.last().unwrap().frame_id,u64::MAX);
    let cp=checkpoint(&f,f.chunks.len(),&ctx);
    let r=rolling::read(ctx.refs(),&cp,&[],f.chunks.len() as u64,Limits::default()).unwrap();
    assert_eq!(r.boundary_frames,vec![FrameRun{owner:OWNER,first:u64::MAX-1,last:u64::MAX}]);
    assert_eq!(r.prefix.events,12);
}

#[test]
fn imported_windowed_accounting_cursor_cannot_name_window_zero() {
    let ctx=Context::new(true);let config=Cfg::default().chunk();let row=account(8,5,1,0);let b=begin();
    let mut windows=windows::Reader::new();windows.chunk(41,&b.payload,&ctx.cap).unwrap();
    let mut ordinary=with(vec![b,row.clone()]);ordinary.flags|=512;
    reader::read(&ordinary.bytes()).unwrap();
    let prefix=[Input::Fifo{index:4,kind:31,payload:&config.payload,windows:None},
        Input::Fifo{index:6,kind:43,payload:&row.payload,windows:Some(&windows)}];
    let mut cp=rolling::checkpoint(ctx.refs(),&prefix,7,Limits::default()).unwrap();
    w64(&mut cp,136,0);repair(&mut cp);
    assert!(matches!(rolling::read(ctx.refs(),&cp,&[],7,Limits::default()),Err(Error::Checkpoint(_))));
}

#[test]
fn saved_status_and_current_census_preserve_event_kind_exclusivity() {
    let ctx=Context::new(false);let mut accepted=Vec::new();
    for saturated in [false,true] {
        let f=with(vec![events(&wrap(1,1,100,10,20)),St::new(if saturated {7}else{2},6,6,0,6,1,1,0).chunk()]);
        reader::read(&f.bytes()).unwrap();let mut cp=checkpoint(&f,f.chunks.len(),&ctx);
        if saturated {
            // Both values fit individually, but no event can be ENTER and EXIT.
            for at in [72,80,144,152] {w64(&mut cp,at,6);}
        } else {
            // No event followed the saved status: its ENTER census cannot differ.
            w64(&mut cp,144,0);
        }
        repair(&mut cp);
        if !matches!(rolling::read(ctx.refs(),&cp,&[],f.chunks.len() as u64,Limits::default()),Err(Error::Checkpoint(_))) {
            accepted.push(if saturated {"ENTER+EXIT exceeds events"}else{"same-event-count status census differs"});
        }
    }
    assert!(accepted.is_empty(),"accepted impossible checkpoint census: {accepted:?}");
}
