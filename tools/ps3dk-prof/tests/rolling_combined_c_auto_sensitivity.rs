//! Additional strict-flag witness; ordinary source rejection precedes C.
#[allow(dead_code)] mod common;
mod fifo_support;
mod combined_auto_support;
mod combined_auto_invalid_support;
use ps3dk_prof::Rule;
use combined_auto_support::put16;
use combined_auto_invalid_support::{baseline,forged,rejected,Forgery};
fn unknown()->Forgery {
    Forgery{name:"ops-unknown-flags",ty:35,occurrence:0,rule:Rule::AutoOps,
        edit:|p|put16(p,16+14,1|2|128|256)}
}
#[test]
fn ordinary_unknown_auto_flag_is_rejected_before_c(){forged(&baseline(),&unknown());}
#[test] #[ignore="requires prebuilt actual C AUTO encoder"]
fn c_auto_unknown_flag_refusal_is_atomic(){rejected(&baseline(),&unknown());}
