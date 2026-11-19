//! RSX frame timing modes (reader contract r7): the request menu, the mode label of a view and the BOTH pairing
//! page. A BOTH capture is two views with one timing mode each; their intervals are never summed or mixed.
use crate::live_rsx_frame::{Mode, RequestOptions};
use crate::rsx_frame_view::RsxFrameView;
use eframe::egui::{self, RichText};
use ps3dk_prof::rsx_frame::{FrameView, Half};
use ps3dk_ui as ui;

/// The mode menu beside the Capture RSX frame button.
pub fn request_menu(p: &mut egui::Ui, o: &mut RequestOptions) {
    egui::ComboBox::from_id_salt("rsxf-request-mode").selected_text(o.mode.name()).show_ui(p, |p| {
        for m in [Mode::Pipelined, Mode::Isolated, Mode::Both] {
            p.selectable_value(&mut o.mode, m, m.name()).on_hover_text(m.hint());
        }
    });
    p.checkbox(&mut o.fp_bytes, "Keep fragment shader bytes").on_hover_text(
        "Store the fragment programs' microcode (bound while the capture is armed, up to the capture's byte cap) so \
         the Shader view can disassemble them. Vertex programs are always kept.",
    );
}

/// The view's timing mode and, for a BOTH capture, which frame it is.
pub fn mode_label(f: &FrameView) -> String {
    let mode = if f.isolated { "ISOLATED" } else { "PIPELINED" };
    match f.half {
        Half::Only => mode.into(),
        Half::First => format!("Both capture · frame 1 of 2 · {mode}"),
        Half::Second => format!("Both capture · frame 2 of 2 · {mode}"),
    }
}
/// The menu text of a view in the capture chooser.
pub fn half_suffix(h: Half) -> &'static str {
    match h {
        Half::Only => "",
        Half::First => " · pipelined half",
        Half::Second => " · isolated half",
    }
}

/// The two views of a BOTH capture: (first pipelined, second isolated).
pub fn both_views(v: &RsxFrameView, id: u32) -> Option<(&FrameView, &FrameView)> {
    let first = v.frames.iter().find(|f| f.id == id && f.half == Half::First)?;
    let second = v.frames.iter().find(|f| f.id == id && f.half == Half::Second)?;
    Some((first, second))
}

/// One pairing row: draw text and raw interval on each side (None: unpaired, or untimed).
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct PairRow {
    pub pipelined: Option<(String, Option<u64>)>,
    pub isolated: Option<(String, Option<u64>)>,
}
pub fn pair_rows(first: &FrameView, second: &FrameView) -> Vec<PairRow> {
    let side = |f: &FrameView, i: Option<usize>| {
        i.and_then(|i| f.draws.get(i)).map(|d| {
            (format!("#{} {}{}", d.raw.ordinal, d.raw.kind.name(), d.label.as_deref().map(|l| format!(" · {l}")).unwrap_or_default()),
             d.timing.interval())
        })
    };
    second.pairs.iter().map(|p| PairRow { pipelined: side(first, p.first), isolated: side(second, p.second) }).collect()
}

/// The pairing page: one row per pair, pipelined vs isolated per draw, unpaired rows marked. No row or total mixes
/// the two modes; each view's own totals are on its own page.
pub fn pairs_page(p: &mut egui::Ui, v: &RsxFrameView, id: u32) {
    let Some((first, second)) = both_views(v, id) else {
        return ui::field_message(p, ui::Tone::Neutral, "Pairing needs both frames of a Both capture; the second frame has not arrived.");
    };
    let rows = pair_rows(first, second);
    let unpaired = rows.iter().filter(|r| r.pipelined.is_none() || r.isolated.is_none()).count();
    p.label(format!(
        "{} pairs · {} unpaired. Draws are paired by kind, label and programs. Pipelined and isolated intervals measure \
         different things: compare per draw, never add them.",
        rows.len(), unpaired));
    const ROW: f32 = 24.0;
    let cols = [240.0, 120.0, 240.0, 120.0];
    p.horizontal(|p| {
        for (h, w) in ["Pipelined draw", "Pipelined time", "Isolated draw", "Isolated time"].into_iter().zip(cols) {
            p.add_sized([w, ROW], egui::Label::new(RichText::new(h).strong()));
        }
    });
    egui::ScrollArea::vertical().id_salt(("rsxf-pairs", id)).max_height(360.0).show_rows(p, ROW, rows.len(), |p, range| {
        for r in &rows[range] {
            p.horizontal(|p| {
                for (side, w) in [(&r.pipelined, cols[0]), (&r.isolated, cols[2])] {
                    match side {
                        Some((text, t)) => {
                            p.add_sized([w, ROW], egui::Label::new(text.as_str()).truncate()).on_hover_text(text.as_str());
                            let d = v.duration(*t);
                            p.add_sized([cols[1], ROW], egui::Label::new(d.text)).on_hover_text(d.hover);
                        }
                        None => {
                            p.add_sized([w, ROW], egui::Label::new(RichText::new("unpaired").italics().weak()));
                            p.add_sized([cols[1], ROW], egui::Label::new(""));
                        }
                    }
                }
            });
        }
    });
}
