//! Independent malformed-source and fail-atomic C AUTO acceptance controls.
//! Requires ordinary source validity/refusal first; the C executable sees source only.
#[allow(dead_code)] mod common;
mod fifo_support;
mod combined_auto_support;
mod combined_auto_invalid_support;
use combined_auto_invalid_support::*;
use ps3dk_prof::auto_rsx::{self,MapState};

#[test]
fn independent_auto_sources_and_repaired_semantic_forgeries_are_decided_before_c(){
    let f=baseline();
    for case in ring_forgeries().into_iter().chain(wire_forgeries()){forged(&f,&case);}
    for flags in [auto_rsx::CK_TIMEOUT,auto_rsx::CK_REGRESSION,auto_rsx::CK_READ_REFUSED]{
        let f=failed_clock(flags);let c=complete(&f);let a=c.auto.as_ref().unwrap();
        assert_eq!(a.clocks.len(),10);let failed=a.clocks.iter().find(|a|a.id==10).unwrap();
        assert_eq!((failed.flags,failed.raw,failed.repeated),(flags,0,0));
        assert_eq!(matches!(auto_rsx::qualify(a),MapState::Unqualified(_)),flags==auto_rsx::CK_REGRESSION);
    }
    for invalid in [false,true]{let f=exclusion(invalid);let c=complete(&f);let a=c.auto.as_ref().unwrap();
        let row=a.frames.iter().find(|r|r.frame==7).unwrap();assert_eq!(row.invalid_evidence(),invalid);
        assert_eq!((row.ring_full[2],row.ring_state[2]),if invalid{(0,1)}else{(1,0)});
    }
}

#[test] #[ignore="requires prebuilt private C AUTO encoder fixture"]
fn c_auto_ring_generation_geometry_and_state_refusals_are_atomic(){
    let f=baseline();let cut=nth(&f,35,2);accepted(&f,cut);
    for case in ring_forgeries(){rejected(&f,&case);}
}

#[test] #[ignore="requires prebuilt private C AUTO encoder fixture"]
fn c_auto_config_shapes_reserved_fields_and_failed_clock_refusals_are_atomic(){
    let f=baseline();let cut=nth(&f,35,2);accepted(&f,cut);
    for case in wire_forgeries(){rejected(&f,&case);}
}

#[test] #[ignore="requires prebuilt private C AUTO encoder fixture"]
fn c_auto_failed_clock_support_survives_eviction_without_becoming_valid(){
    baseline();
    for flags in [auto_rsx::CK_TIMEOUT,auto_rsx::CK_REGRESSION,auto_rsx::CK_READ_REFUSED]{
        let f=failed_clock(flags);let original=complete(&f);let cut=nth(&f,35,2);let r=accepted(&f,cut);
        assert!(r.terminal&&!r.torn_tail);let a=r.auto.as_ref().unwrap();
        assert_eq!(a.clocks.iter().map(|a|&a.value).collect::<Vec<_>>(),original.auto.as_ref().unwrap().clocks.iter().collect::<Vec<_>>());
        let failed=a.clocks.iter().find(|a|a.value.id==10).unwrap();assert!(failed.index<(cut as u64));
        assert_eq!((failed.value.flags,failed.value.raw,failed.value.repeated),(flags,0,0));
        assert_eq!(a.ops.iter().map(|o|o.value.id).collect::<Vec<_>>(),vec![3,4]);
        assert_eq!(matches!(r.auto_report.as_ref().unwrap().map,MapState::Unqualified(_)),flags==auto_rsx::CK_REGRESSION);
    }
}

#[test] #[ignore="requires prebuilt private C AUTO encoder fixture"]
fn c_auto_ring_state_exclusion_remains_invalid_and_ring_full_remains_capacity(){
    baseline();
    for invalid in [false,true]{let f=exclusion(invalid);let original=complete(&f);let cut=nth(&f,35,2);let r=accepted(&f,cut);
        let a=r.auto.as_ref().unwrap();assert_eq!(a.invalid_exclusion_seen,invalid);
        assert_eq!(a.frames.iter().map(|f|f.value.frame).collect::<Vec<_>>(),vec![8],"prefix coverage is support, not displayed output");
        let saved=a.builds.iter().find(|b|b.frame==7).unwrap().coverage.as_ref().unwrap();
        assert_eq!(saved.value,original.auto.as_ref().unwrap().frames.iter().find(|f|f.frame==7).unwrap().clone());
        assert!(saved.index<(cut as u64));assert_eq!(saved.value.invalid_evidence(),invalid);
        assert_eq!(a.ops.iter().map(|o|o.value.id).collect::<Vec<_>>(),vec![3,4]);
        let report=r.auto_report.as_ref().unwrap();let frame=report.frames.iter().find(|f|f.view.frame==8).unwrap();
        assert_eq!(frame.view.rsx_busy_ppu,Some((180000,180000)));assert_eq!(frame.view.fifo_wait,Some(60007));
    }
}
