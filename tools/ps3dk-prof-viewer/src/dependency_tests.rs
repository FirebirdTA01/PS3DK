//! Production-parser and Overview draw-path checks for evidence policy limits.
use crate::{
    dependency_fixture::*,
    model::{overview, Document},
};
use eframe::egui;
use ps3dk_prof::{checkpoints::Budgets, dependencies::DependencyBudgets, Outcome};

fn file(enabled: bool) -> File {
    let mut f = File::new(vec![
        chunk(T_HEADER, Hdr::default().payload()),
        chunk(T_CLOCKS, clocks_payload(1, 1, 0, 79_800_000, 0)),
        chunk(T_THREADS, threads_payload(&[thr(1)])),
        chunk(T_RECORDS, records_payload(1, &[regerr(0, 1)])),
    ]);
    f.flags = if enabled { 8 } else { 0 };
    f
}
fn doc(enabled: bool, refuse: bool) -> Document {
    let bytes = file(enabled).bytes();
    let limits = DependencyBudgets {
        entries: if refuse { 0 } else { 262144 },
        ..DependencyBudgets::default()
    };
    let cap = ps3dk_prof::reader::read_with_evidence(&bytes, Budgets::default(), limits).unwrap();
    let a = ps3dk_prof::analysis::analyze(cap).unwrap();
    Document::from_outcome("test.prof".into(), bytes.len(), false, Outcome::Analysed(Box::new(a)))
}
#[test]
fn dependency_overview_replacement_does_not_keep_old_refusals() {
    let mut d = doc(true, true);
    let warning = overview(d.analysis().unwrap()).dependency_warning.unwrap();
    for part in [
        "Dependency evidence: INCOMPLETE",
        "entry limit",
        "chunk 3",
        "records 0",
        "pairs 0",
        "ceiling",
        "unavailable",
    ] {
        assert!(warning.contains(part), "{warning}");
    }
    for (enabled, refuse) in [(true, false), (false, true)] {
        d = doc(enabled, refuse);
        assert!(overview(d.analysis().unwrap()).dependency_warning.is_none());
    }
    d = Document::from_bytes("bad.prof".into(), b"invalid");
    assert!(d.analysis().is_none());
    d = Document::from_bytes("live.prof".into(), &file(true).bytes());
    assert!(overview(d.analysis().unwrap()).dependency_warning.is_none());
}
#[test]
fn dependency_overview_paints_the_named_warning_once() {
    fn texts(shape: &egui::Shape, out: &mut Vec<String>) {
        match shape {
            egui::Shape::Vec(v) => {
                for x in v {
                    texts(x, out);
                }
            }
            egui::Shape::Text(t) => out.push(t.galley.job.text.clone()),
            _ => {}
        }
    }
    let ctx = egui::Context::default();
    ps3dk_ui::install(&ctx);
    for refused in [true, false] {
        let d = doc(true, refused);
        let a = d.analysis().unwrap();
        let rsx = d.rsx.get(a);
        let mut painted = Vec::new();
        for _ in 0..2 {
            let output = ctx.run(
                egui::RawInput {
                    screen_rect: Some(egui::Rect::from_min_size(egui::Pos2::ZERO, egui::vec2(1600., 2000.))),
                    ..Default::default()
                },
                |ctx| {
                    egui::CentralPanel::default().show(ctx, |p| super::draw_overview(p, a, &d.ext, &rsx));
                },
            );
            painted.clear();
            for s in &output.shapes {
                texts(&s.shape, &mut painted);
            }
        }
        assert_eq!(
            painted
                .iter()
                .filter(|t| t.contains("Dependency evidence: INCOMPLETE"))
                .count(),
            usize::from(refused),
            "{painted:?}"
        );
    }
}
