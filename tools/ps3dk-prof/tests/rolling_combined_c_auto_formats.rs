//! Explicitly modeled ordinary AUTO formats. Source truth and every canonical
//! prefix are validated before the C transaction is invoked.
#[allow(dead_code)] mod common;
mod fifo_support;
mod spu_support;
mod combined_auto_support;
mod combined_auto_c_support;
mod combined_auto_invalid_support;
use common::*;
use combined_auto_support::{config,op,coverage,clock,status,put16,put32,put64};
use combined_auto_c_support::{references,parity};
use ps3dk_prof::{auto_rsx,rolling_combined as combined};

fn op_width(mask:u32)->usize {if mask&512!=0{160}else if mask>=63{144}else{128}}
fn row_width(mask:u32)->usize {if mask==3071{544}else if mask==1023{480}else if mask==511{416}else if mask>=31{384}else{352}}
/// 3071/11: the refusals by reason. Frame 7's row refused two ranges for no pair (slot 6) and one for depth (slot 0);
/// every STATUS carries those and one no_frame refusal (slot 3), which counts in no row.
fn reasons_row(mask:u32,c:&mut Chunk,frame:u64){
    if mask==3071&&frame==7 {put32(&mut c.payload,440,3);put32(&mut c.payload,480,1);put32(&mut c.payload,480+24,2);}
}
fn reasons_status(mask:u32,mut c:Chunk)->Chunk {
    if mask==3071 {c.payload.resize(256,0);put64(&mut c.payload,128,1);put64(&mut c.payload,128+24,1);put64(&mut c.payload,128+48,2);}
    c
}
fn row(mask:u32,frame:u64,observed:u64)->Chunk {
    let mut c=coverage(frame,(frame*100000,(frame+1)*100000),observed);
    c.payload.resize(row_width(mask),0);reasons_row(mask,&mut c,frame);
    if frame==7&&mask>=255 {
        // One segment visit lacked a per-draw record and is covered by the
        // later genuine whole-CALL bracket's build-excluded population.
        put64(&mut c.payload,256,observed+1);put64(&mut c.payload,280,1);
    }
    c
}
fn operation(mask:u32,id:u64,frame:u64,kind:u16,segment:bool,dropped:bool)->Chunk {
    let mut c=op(id,frame,if segment{Some(7)}else{None},(1000*id,1000*id+100));
    c.payload.resize(16+op_width(mask),0);put32(&mut c.payload,12,op_width(mask) as u32);
    let r=&mut c.payload[16..];put16(r,12,kind);
    if kind==4 {r[64..92].fill(0);put32(r,64,if frame==7{1}else{2});}
    if dropped {
        put16(r,14,64|4|16);put32(r,8,0);put64(r,16,0);
        put64(r,40,0);put64(r,48,0);put32(r,116,0);put64(r,120,0);
    }
    if kind==5 {
        r[64..92].fill(0);for i in 0..5 {put32(r,64+4*i,1);}
        put64(r,128,7);put32(r,136,fifo_support::OWNER);put16(r,140,1);
    }
    if kind==6 {
        r[64..92].fill(0);put32(r,64,1);put32(r,68,1);
        put32(r,144,0x42);put32(r,148,0x1000);put32(r,152,2);
    }
    c
}
fn base()->(File,Chunk) {
    let mut f=scenario(vec![lane(fifo_support::OWNER,vec![])]).file();
    let trailer=f.chunks.pop().unwrap();f.flags=193;
    f.chunks.push(fifo_support::rsx_domain(fifo_support::D));f.chunks.push(fifo_support::Cfg::default().chunk());
    (f,trailer)
}
fn source(mask:u32)->File {
    assert!([15,31,63,127,255,511,1023,3071].contains(&mask));
    let (mut f,trailer)=base();f.chunks.push(config(mask));
    for id in 1..=9 {let mut c=clock(id);if mask<127{put32(&mut c.payload,64,id);put32(&mut c.payload,68,id);}f.chunks.push(c);}
    f.chunks.push(operation(mask,1,7,4,false,false));
    f.chunks.push(operation(mask,2,7,3,false,false));
    f.chunks.push(row(mask,7,3));f.chunks.push(reasons_status(mask,status(0,2,1,9)));
    f.chunks.push(fifo_support::St::new(2,0,0,0,0,0,0,0).chunk());
    // The building frame's final row was already accepted. The real segment
    // records arrive at CALL/drain time, including one abandoned build.
    f.chunks.push(operation(mask,3,8,3,true,false));
    f.chunks.push(operation(mask,4,0,3,true,true));
    let mut next=5;
    if mask>=255{f.chunks.push(operation(mask,next,8,5,false,false));next+=1;}
    if mask&512!=0{f.chunks.push(operation(mask,next,8,6,false,false));next+=1;}
    f.chunks.push(operation(mask,next,8,4,false,false));
    for mut e in fifo_support::wrap(1,1,210000,30000,30000){e.frame=8;f.chunks.push(fifo_support::events(&[e]));}
    f.chunks.push(fifo_support::St::new(1,6,6,0,6,1,1,0).chunk());
    f.chunks.push(row(mask,8,0));f.chunks.push(reasons_status(mask,status(1,next,2,9)));f.chunks.push(trailer);f
}
fn refused(width:usize)->File {
    let (mut f,trailer)=base();let mut p=vec![0;width];
    put16(&mut p,0,1);put16(&mut p,2,1);put32(&mut p,4,1);
    put32(&mut p,24,9);put32(&mut p,28,(-7i32) as u32);put64(&mut p,64,fifo_support::FREQ);
    p[96..128].fill(0x2a);p[128..160].fill(0x63);
    f.chunks.push(chunk(34,p));f.chunks.push(status(1,0,0,0));
    f.chunks.push(fifo_support::St::new(1,0,0,0,0,0,0,0).chunk());f.chunks.push(trailer);f
}
fn truth(f:&File,mask:Option<u32>){
    let bytes=f.bytes();let (offsets,_)=references(&bytes);
    let c=reader::read(&bytes).unwrap();let inv=c.auto.as_ref().unwrap();
    let whole=combined::checkpoint_combined(&bytes[..16],combined::Limits::default()).unwrap();
    let retained=combined::read_combined_suffix(&whole,&bytes[16..],combined::Limits::default(),combined::ReportOptions{
        spu_profile_table:&[],causal_limits:Default::default()}).unwrap();
    let a=retained.auto.unwrap();assert!(a.final_seen);assert!(retained.terminal);
    if let Some(mask)=mask {
        let n=5+u64::from(mask>=255)+u64::from(mask&512!=0);
        assert_eq!((inv.ops_parsed,inv.frames_parsed,inv.clocks_parsed,inv.statuses),(n,2,9,2));
        assert_eq!(inv.ops.iter().filter(|o|o.flags&64!=0).count(),2);
        assert_eq!(inv.ops.iter().filter(|o|o.flags&64!=0&&o.call==0).count(),1);
        assert_eq!(inv.ops.iter().filter(|o|o.kind==5).count(),usize::from(mask>=255));
        assert_eq!(inv.ops.iter().filter(|o|o.kind==6).count(),usize::from(mask&512!=0));
        assert_eq!(inv.ops.iter().filter(|o|o.kind==4).count(),2);
        assert_eq!(a.cumulative.ops,n);assert_eq!(a.clocks.len(),9);
        if mask>=63 {
            let b=a.builds.iter().find(|b|b.owner==fifo_support::OWNER&&b.frame==7).unwrap();
            assert_eq!((b.records,b.abandoned,b.call_excluded),([0,0,3],1,u64::from(mask>=255)));
            assert!(b.coverage.is_some());
        }
        if mask==3071 {
            assert_eq!(inv.frames.iter().find(|r|r.frame==7).unwrap().range_refused,Some([1,0,0,0,0,0,2,0,0,0]));
            assert_eq!(inv.last_status.as_ref().unwrap().range_refused,Some([1,0,0,1,0,0,2,0,0,0]));
        }
        if mask&512!=0{let r=inv.ops.iter().find(|o|o.kind==6).unwrap();assert_eq!((r.depth,r.tag,r.fp_offset),(2,Some(0x42),Some(0x1000)));}
    }else{
        assert!(matches!(inv.config,Some(auto_rsx::Config::Refused{reason:9,application_rc:-7,..})));
        assert_eq!((a.cumulative.ops,a.cumulative.frames,a.cumulative.clocks,a.cumulative.statuses),(0,0,0,1));
        assert!(a.builds.is_empty()&&a.clocks.is_empty()&&a.ops.is_empty()&&a.frames.is_empty());
    }
    assert_eq!(offsets.len(),f.chunks.len()+1);
}

#[test]
fn independent_remaining_masks_and_refused_sources_validate_before_c(){
    for mask in [15,31,63,127,255,511,1023,3071]{truth(&source(mask),Some(mask));}
    for width in [160,176]{truth(&refused(width),None);}
}
macro_rules! c_format {
    ($name:ident,$mask:expr)=>{#[test] #[ignore="requires prebuilt actual C AUTO encoder"]
        fn $name(){let f=source($mask);truth(&f,Some($mask));parity(f.bytes());}};
}
c_format!(c_auto_mask15_segments_and_frame_marks,15);
c_format!(c_auto_mask31_presentation_width,31);
c_format!(c_auto_mask63_build_execution_and_abandoned_population,63);
c_format!(c_auto_mask127_bracketed_clocks_and_build_state,127);
c_format!(c_auto_mask255_whole_call_build_excluded_population,255);
c_format!(c_auto_mask511_label_coverage_width,511);
c_format!(c_auto_mask1023_tagged_ranges_and_call_rows,1023);
c_format!(c_auto_mask3071_range_refusal_tails,3071);
#[test] #[ignore="requires prebuilt actual C AUTO encoder"]
fn c_auto_refused_config160_has_no_invented_domain_or_population(){let f=refused(160);truth(&f,None);parity(f.bytes());}
#[test] #[ignore="requires prebuilt actual C AUTO encoder"]
fn c_auto_refused_config176_preserves_original_wide_payload(){let f=refused(176);truth(&f,None);parity(f.bytes());}

fn preinit_foreign(mut f:File)->File {
    for c in f.chunks.iter_mut().filter(|c|c.ty==37) {
        let flags=u16::from_le_bytes(c.payload[2..4].try_into().unwrap());
        put16(&mut c.payload,2,flags|2048);
    }
    f
}
#[test]
fn preinit_foreign_sources_validate_under_tagged_and_ring_masks_only(){
    truth(&preinit_foreign(source(1023)),Some(1023));
    let f=preinit_foreign(combined_auto_invalid_support::baseline());references(&f.bytes());
    for mask in [15,31,63,127,255,511] {
        assert_eq!(reader::read(&preinit_foreign(source(mask)).bytes()).unwrap_err().rule,ps3dk_prof::Rule::AutoStatus);
    }
}
#[test] #[ignore="requires prebuilt actual C AUTO encoder"]
fn c_auto_preinit_foreign_survives_every_checkpoint(){
    parity(preinit_foreign(source(1023)).bytes());
    parity(preinit_foreign(combined_auto_invalid_support::baseline()).bytes());
}
#[test] #[ignore="requires prebuilt actual C AUTO encoder"]
fn c_auto_preinit_foreign_is_sticky_and_unknown_flags_still_refuse(){
    use combined_auto_invalid_support::{Forgery,rejected};
    let f=preinit_foreign(combined_auto_invalid_support::baseline());
    rejected(&f,&Forgery{name:"preinit-cleared",ty:37,occurrence:1,rule:ps3dk_prof::Rule::AutoStatus,
        edit:|p|{let flags=u16::from_le_bytes(p[2..4].try_into().unwrap());put16(p,2,flags&!2048);}});
    rejected(&f,&Forgery{name:"status-unknown-4096",ty:37,occurrence:1,rule:ps3dk_prof::Rule::AutoStatus,
        edit:|p|{let flags=u16::from_le_bytes(p[2..4].try_into().unwrap());put16(p,2,flags|4096);}});
}
