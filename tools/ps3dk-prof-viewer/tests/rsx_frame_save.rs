use ps3dk_prof::{reader, rsx_frame as rf};
use ps3dk_prof_viewer::capture_save::rsx_projection;
const SAMPLE: &[u8] = include_bytes!("../../ps3dk-prof/tests/fixtures/rsx_frame/sample.prof");
#[path = "../../ps3dk-prof/tests/common/mod.rs"]
#[rustfmt::skip]
mod common;
#[path = "../../ps3dk-prof/tests/rsx_frame_support/mod.rs"]
#[rustfmt::skip]
mod rsx_frame_support;

#[test]
fn small_frame_from_twenty_megabyte_source_saves_in_a_small_workspace() {
    use rsx_frame_support::*;
    use ps3dk_prof_viewer::{app::App, model::Document};
    use std::sync::Arc;
    let source = reader::read(SAMPLE).unwrap();
    let selected = rf::frames(&source).remove(0);
    let other_frame = source.lanes[&selected.owner].frames.iter().find(|f| f.id != selected.frame).unwrap().id;
    let mut extra = vec![XBegin::live(2, selected.owner, other_frame, "unselected large capture").chunk()];
    for block in 0..100 {
        let mut b = common::Buf::default();
        for i in 0..2200 { XDraw::decoded(block*2200+i, 1).encode(&mut b); }
        extra.push(draws_chunk(2, block, 2200, &b.0));
    }
    let mut end = XEnd::done(2);
    end.n = [100,0,220000,220000,0,220000,0,u32::MAX,0,0,0,0];
    end.bytes = (220000*96,0);
    end.untimed = 220000;
    extra.push(end.chunk());
    let bytes = with_rsxf(SAMPLE, extra).bytes();
    assert!(bytes.len() > 20 << 20);
    let dir=std::env::temp_dir().join(format!("rsxf-large-save-{}",std::process::id()));
    std::fs::create_dir_all(&dir).unwrap();
    let src=dir.join("large.prof"); std::fs::write(&src,&bytes).unwrap();
    let mut app=App::new(None);
    app.install(Arc::new(Document::from_bytes(src, &bytes)),true);
    app.captures.bind_source(&bytes);
    // Enough for bounded preparation and this tiny frame, not 70x the source.
    app.pool=ps3dk_prof_viewer::capture::Pool::new(128 << 20);
    let dst=dir.join("selected.prof");
    app.save_rsx_frame_to(selected.id,dst.clone()); app.wait_save();
    assert!(matches!(&app.save_result,Some((false,_))),"{:?}",app.save_result);
    let saved=std::fs::read(&dst).unwrap();
    assert!(saved.len() < 100 << 10);
    assert_eq!(rf::frames(&reader::read(&saved).unwrap())[0],selected);
    std::fs::remove_dir_all(dir).unwrap();
}
#[test]
fn selected_frame_reopens_identically_and_saved_export_can_be_copied() {
    let cap = reader::read(SAMPLE).unwrap();
    let f = rf::frames(&cap).remove(0);
    let bytes = rsx_projection(SAMPLE, &f).unwrap();
    let out = reader::read(&bytes).unwrap();
    assert_eq!(rf::frames(&out).iter().find(|v| v.id == f.id), Some(&f));
    assert_eq!(rsx_projection(&bytes, &f).unwrap(), bytes);
    let mut forged = f.clone();
    forged.name.push_str(" changed");
    assert!(rsx_projection(SAMPLE, &forged).is_err());
}
#[test]
fn open_capture_stays_open_after_save() {
    let mut cut = 16;
    while cut + 20 <= SAMPLE.len() {
        let ty = u32::from_le_bytes(SAMPLE[cut..cut + 4].try_into().unwrap());
        if ty == 48 {
            break;
        }
        let n = u32::from_le_bytes(SAMPLE[cut + 4..cut + 8].try_into().unwrap()) as usize;
        cut += n + 20;
    }
    let bytes = &SAMPLE[..cut];
    let cap = reader::read(bytes).unwrap();
    let f = rf::frames(&cap).remove(0);
    assert_ne!(f.status, rf::FrameStatus::Complete);
    let saved = rsx_projection(bytes, &f).unwrap();
    assert_eq!(rf::frames(&reader::read(&saved).unwrap())[0], f);
}
