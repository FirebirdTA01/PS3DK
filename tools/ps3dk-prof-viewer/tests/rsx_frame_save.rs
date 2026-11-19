use ps3dk_prof::{reader, rsx_frame as rf};
use ps3dk_prof_viewer::capture_save::rsx_projection;
const SAMPLE: &[u8] = include_bytes!("../../ps3dk-prof/tests/fixtures/rsx_frame/sample.prof");
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
