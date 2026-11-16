//! Verify the messages actually painted by the existing empty pages.
#[path = "../../ps3dk-prof/tests/common/mod.rs"]
#[rustfmt::skip]
#[allow(clippy::all)]
mod common;
use super::*;

fn texts(out: &egui::FullOutput) -> String {
    fn walk(s: &egui::Shape, v: &mut Vec<String>) {
        match s {
            egui::Shape::Vec(xs) => xs.iter().for_each(|x| walk(x, v)),
            egui::Shape::Text(t) => v.push(t.galley.job.text.clone()),
            _ => {}
        }
    }
    let mut v = Vec::new();
    for s in &out.shapes { walk(&s.shape, &mut v); }
    v.join("\n")
}

#[test]
fn empty_pages_explain_the_recorded_absence() {
    let bytes = common::scenario(vec![]).bytes();
    common::harvest(&bytes);
    let d = Document::from_bytes("empty.prof".into(), &bytes);
    let a = d.analysis().unwrap();
    for (page, expected) in [
        (Page::Overview, "No PPU lanes recorded"),
        (Page::Zones, "No zones recorded"),
        (Page::Frames, "No FRAME records"),
        (Page::HotPaths, "No PPU spans"),
        (Page::Waits, "no automatic-wait capability"),
        (Page::Fifo, "no FIFO wrap capability"),
        (Page::Automatic, "build with -DPS3_PROF_AUTO=ON"),
        (Page::RsxFrames, "declares no RSX source"),
    ] {
        let ctx = egui::Context::default();
        ui::install(&ctx);
        let mut painted = String::new();
        for _ in 0..2 {
            let out = ctx.run(egui::RawInput { screen_rect: Some(egui::Rect::from_min_size(egui::Pos2::ZERO, egui::vec2(1500.0, 1500.0))), ..Default::default() }, |ctx| {
                egui::CentralPanel::default().show(ctx, |p| { match page {
                    Page::Overview => draw_overview(p, a, &d.ext, &d.rsx.get(a)),
                    Page::Zones => draw_zones(p, a, &mut vec![], &mut (ZoneColumn::Name, false), &mut None),
                    Page::Frames => { frames_ui::draw(p, a, d.frames.as_ref().unwrap(), &mut Default::default(), None, Some(&d.foreign)); },
                    Page::HotPaths => { crate::hotpaths_ui::page(p, d.hot.as_ref().unwrap(), &mut Default::default(), a, &d.foreign_hot, &d.func_paths); },
                    Page::Waits => crate::waits_ui::page(p, d.waits.as_ref().unwrap(), &mut Default::default(), &mut Default::default()),
                    Page::Fifo => crate::fifo_ui::page(p, d.fifo.as_ref().unwrap(), &mut Default::default()),
                    Page::Automatic => { crate::auto_ui::page(p, d.auto.as_ref().unwrap(), &mut Default::default(), None, &mut Default::default()); },
                    Page::RsxFrames => crate::rsx_ui::page(p, a, &d.rsx.get(a), &mut Default::default(), &mut Default::default()),
                    _ => unreachable!(),
                } });
            });
            painted = texts(&out);
        }
        assert!(painted.contains(expected), "{page:?}: {painted}");
        assert!(!painted.contains("no trigger"), "absence does not prove an app has no trigger");
    }
}
