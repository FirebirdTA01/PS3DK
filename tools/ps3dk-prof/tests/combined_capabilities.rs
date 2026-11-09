//! Combined capability controls: independent fixtures, PC-only integration gate.
mod common;
mod spu_support;
use common::*;
use ps3dk_prof::{reader,Rule};
// Bit 5 (function paths) is independent of every other capability.
const MASKS:[u32;32]=[0,1,3,7,8,9,11,15,16,17,19,23,24,25,27,31,32,33,35,39,40,41,43,47,48,49,51,55,56,57,59,63];
fn headers(flags:u32)->File{
 let mut f=File::new(vec![chunk(1,Hdr{max_threads:64,..Default::default()}.payload()),chunk(2,clocks_payload(1,1,0,79_800_000,0))]);
 f.flags=flags;
 if flags&2!=0 {f.chunks.push(chunk(17,spu_support::Prof::default().payload()));}
 f
}
fn config()->Chunk{
 let mut b=Buf::default(); b.u16(1).u16(0).u32(64).u32(128).u32(1_048_576).u32(806_912).u32(96).u64(79_800_000).u64(7).u32(24).u32(4).u32(1024).u32(0);chunk(22,b.0)
}
fn key(b:&mut Buf){b.u8(1).u8(0).u16(0).u32(1).u32(0).u32(0);}
fn mixed(flags:u32)->File{
 let mut f=headers(flags);f.chunks.push(chunk(4,threads_payload(&[thr(1)])));
 f.chunks.push(chunk(5,records_payload(1,&[frame(0,10,0),frame(1,20,1)])));
 if flags&8!=0 {
  let at=f.chunks.len() as u64;
  let mut b=Buf::default();b.u16(1).u16(0).u32(1).u64(0).u64(1).u64(0).u64(0).u32(1).u32(0);key(&mut b);f.chunks.push(chunk(20,b.0));
  let mut b=Buf::default();b.u16(1).u16(0).u32(1).u64(0);key(&mut b);b.u64(at).u64(0).u64(0).u64(0).u64(0).u64(0);f.chunks.push(chunk(26,b.0));
  let mut b=Buf::default();b.u16(1).u8(2).u8(0).u32(1).u64(0);key(&mut b);b.u64(at).u64(at+1).u64(0).u32(0).u32(0);f.chunks.push(chunk(21,b.0));
 }
 if flags&16!=0{
  f.chunks.push(config());
  let mut b=Buf::default();b.u16(1).u16(0).u32(2);
  for (kind,seq,tb) in [(1,1,100),(2,2,130)] {b.u32(1).u16(1).u8(kind).u8(0).u64(seq).u64(1).u64(tb).u64(19).u64(0).u64(0).u64(0x1000).u32(0).u32(2).u64(0).i32(0).i32(0).u64(0);}
  f.chunks.push(chunk(23,b.0));
  let mut b=Buf::default();b.u16(1).u16(0).u32(1);b.0.extend_from_slice(&[0;64]);b.u32(1).u32(0).u64(2).u64(2).u64(0).u64(0).u64(0).u64(0).u64(2);f.chunks.push(chunk(24,b.0));
 }
 f
}
#[test]
fn all_flag_bytes_full_partial_and_high_bytes(){
 for flags in 0..=255 {
  let f=headers(flags);let bytes=f.bytes();
  for n in 13..=16 {
   let r=reader::read(&bytes[..n]);
   if MASKS.contains(&flags){let c=r.unwrap();assert_eq!(c.waits.is_some(),flags&16!=0);assert!(c.checkpoints.is_none()||n==16);}
   else {assert_eq!(r.unwrap_err().rule,Rule::FileFlagsNonzero,"flags={flags} n={n}");}
  }
  let r=reader::read(&bytes);
  if MASKS.contains(&flags){r.unwrap();}else{assert_eq!(r.unwrap_err().rule,Rule::FileFlagsNonzero);}
 }
 for flags in MASKS {for high in 13..16 {let mut b=headers(flags).bytes();b[high]=1;for n in high+1..=16{assert_eq!(reader::read(&b[..n]).unwrap_err().rule,Rule::FileFlagsNonzero);}}}
}
#[test]
fn every_valid_mask_keeps_independent_feature_inventories(){
 for flags in MASKS {
  let c=reader::read(&mixed(flags).bytes()).unwrap();
  assert_eq!(c.checkpoints.is_some(),flags&8!=0);assert_eq!(c.waits.is_some(),flags&16!=0);
  if flags&8!=0{assert_eq!(c.source_checkpoints.as_ref().unwrap().results.len(),1);}
  if flags&16!=0{let w=ps3dk_prof::waits::analyze(&c,&Default::default());assert_eq!(w.rows.len(),1);assert_eq!(w.rows[0].ticks,Some(30));}
 }
}
#[test]
fn capability_dispatch_crc_sequence_and_profile_precedence(){
 for flags in MASKS {for ty in [19,20,21,22,23,24,25,26] {
  let mut f=headers(flags);let i=f.chunks.len();f.chunks.push(chunk(ty,vec![0;4]));
  let enabled=if [22,23,24].contains(&ty){flags&16!=0}else{flags&8!=0};
  let e=reader::read(&f.bytes()).unwrap_err();if !enabled{assert_eq!(e.rule,Rule::UnknownChunkType);}else{assert_ne!(e.rule,Rule::UnknownChunkType);}
  f.bad_crc=Some(i);f.seq_override=Some((i,99));assert_eq!(reader::read(&f.bytes()).unwrap_err().rule,Rule::ChunkCrc);
  f.bad_crc=None;assert_eq!(reader::read(&f.bytes()).unwrap_err().rule,Rule::ChunkSeqOrder);
 }}
 for flags in MASKS.into_iter().filter(|f|f&2!=0){let mut f=headers(flags);f.chunks[2]=chunk(25,vec![0;4]);assert_eq!(reader::read(&f.bytes()).unwrap_err().rule,Rule::ProfileMissing);}
}
#[test]
fn custom_budget_entrypoints_do_not_drop_the_other_feature(){
 let b=mixed(24).bytes();
 let w=reader::read_with_waits(&b,ps3dk_prof::waits::Budgets{ops:0,..Default::default()}).unwrap();
 assert!(w.waits.as_ref().unwrap().refusal.is_some());assert_eq!(w.source_checkpoints.unwrap().results.len(),1);
 let c=reader::read_with_checkpoints(&b,ps3dk_prof::checkpoints::Budgets{ops:0,..Default::default()}).unwrap();
 assert!(c.checkpoints.as_ref().unwrap().refusal.is_some());assert_eq!(ps3dk_prof::waits::analyze(&c,&Default::default()).rows.len(),1);
 let c=reader::read_with_evidence(&b,Default::default(),ps3dk_prof::dependencies::DependencyBudgets{ops:0,..Default::default()}).unwrap();
 assert!(c.dependency_status().unwrap().refusal.is_some());assert_eq!(ps3dk_prof::waits::analyze(&c,&Default::default()).rows.len(),1);
 let c=reader::read_with_source_evidence(&b,Default::default(),Default::default(),ps3dk_prof::source_checkpoints::Budgets{ops:0,..Default::default()}).unwrap();
 assert!(c.source_checkpoints.as_ref().unwrap().refusal.is_some());assert_eq!(ps3dk_prof::waits::analyze(&c,&Default::default()).rows.len(),1);
}
