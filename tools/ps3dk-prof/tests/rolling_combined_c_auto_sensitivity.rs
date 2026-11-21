//! Additional strict-flag witness; ordinary source rejection precedes C.
#[allow(dead_code)] mod common;
mod fifo_support;
mod combined_auto_support;
mod combined_auto_invalid_support;
use ps3dk_prof::{auto_rsx,rolling_combined as combined,Rule};
use combined_auto_support::put16;
use combined_auto_invalid_support::{accepted,baseline,boundaries,complete,forged,limits,nth,rejected,source,Forgery};
fn unknown()->Forgery {
    Forgery{name:"ops-unknown-flags",ty:35,occurrence:0,rule:Rule::AutoOps,
        edit:|p|put16(p,16+14,1|2|128|1024)}
}
/// The ring source with frame 8's two draws carrying ZPASS counter deltas (OPS flag 512, u32@156).
fn zpass()->common::File {
    let mut f=source();
    for (k,v) in [(2,1234u32),(3,0)]{let at=nth(&f,35,k);let p=&mut f.chunks[at].payload;put16(p,16+14,3|128|512);
        p[16+156..16+160].copy_from_slice(&v.to_le_bytes());}
    f
}
/// The ring source with frame 8's two draws timed by an isolated RSX frame capture (OPS flag 256 ISOLATED).
fn isolated()->common::File {
    let mut f=source();
    for k in [2,3]{let at=nth(&f,35,k);put16(&mut f.chunks[at].payload,16+14,3|128|256);}
    f
}
#[test]
fn ordinary_unknown_auto_flag_is_rejected_before_c(){forged(&baseline(),&unknown());}
#[test]
fn ordinary_isolated_ring_ops_validate_before_c(){
    let f=isolated();let c=complete(&f);
    let r=auto_rsx::analyze(&c);
    let iso:Vec<(u64,u64)>=r.frames.iter().map(|x|(x.frame,x.isolated_ops)).collect();
    assert_eq!(iso,vec![(7,0),(8,2)],"frame 8 is the isolated frame");
    let b=f.bytes();
    for &at in &boundaries(&b)[..f.chunks.len()]{combined::checkpoint_combined(&b[..at],limits()).expect("isolated ring ops: valid prefix");}
}
#[test] #[ignore="requires prebuilt actual C AUTO encoder"]
fn c_auto_isolated_ring_ops_are_admitted(){let f=isolated();let cut=nth(&f,35,2);accepted(&f,cut);}
#[test]
fn ordinary_zpass_ring_ops_validate_before_c(){
    let f=zpass();let c=complete(&f);
    assert!(c.auto.as_ref().unwrap().ops.iter().any(|o|o.zpass==Some(1234)),"the ZPASS delta reads back");
    let b=f.bytes();
    for &at in &boundaries(&b)[..f.chunks.len()]{combined::checkpoint_combined(&b[..at],limits()).expect("ZPASS ring ops: valid prefix");}
}
#[test]
fn ordinary_zpass_without_raw_valid_is_rejected_before_c(){
    forged(&baseline(),&Forgery{name:"ops-zpass-not-raw-valid",ty:35,occurrence:0,rule:Rule::AutoOps,
        edit:|p|put16(p,16+14,1|4|128|512)});
}
#[test] #[ignore="requires prebuilt actual C AUTO encoder"]
fn c_auto_zpass_ring_ops_are_admitted(){let f=zpass();let cut=nth(&f,35,2);accepted(&f,cut);}
#[test] #[ignore="requires prebuilt actual C AUTO encoder"]
fn c_auto_unknown_flag_refusal_is_atomic(){rejected(&baseline(),&unknown());}
