//! Frame snapshot UI: the "Capture full frame" button, the sidebar list and the snapshot page.  Every function returns
//! what was clicked as an [`Action`]; the app applies it to [`crate::capture_ctl::Captures`].  Bar placement and hit
//! testing come from the tested pure [`crate::capture::layout`].

use eframe::egui::{self, Align2, Color32, FontId, Pos2, Rect, Sense, Shape, Stroke, Vec2};
use ps3dk_ui as ui;

use crate::capture::{
    layout, pt_text, CBar, Class, FrameSnapshot, Origin, PaintCaps, Premise, SDraw, SLayout, View, F_EXTRAP, F_OPEN,
    F_UNAVAIL,
};
use crate::capture_ctl::Captures;

pub const BUTTON: &str = "Capture full frame";
pub const BUTTON_TIP: &str =
    "Freezes what has arrived for the selected (or newest) frame so far, on every PPU, SPU and RSX lane. SPU and RSX data arrive late; the snapshot never claims the frame is complete.";

#[derive(Debug, Clone, PartialEq, Eq)]
pub enum Action {
    Show(u32),
    OpenMark(usize),
    Delete(u32),
    Reresolve(Premise),
    Back,
}

/// The app-bar button: disabled (with the reason on hover) when `blocked`.  `at` receives where it was drawn.
pub fn button(bar: &mut egui::Ui, blocked: Option<&str>, at: &mut Option<Rect>) -> bool {
    let r = match blocked {
        None => ui::quiet_button(bar, BUTTON).on_hover_text(BUTTON_TIP),
        Some(why) => bar.add_enabled(false, egui::Button::new(BUTTON).frame(false)).on_disabled_hover_text(why),
    };
    *at = Some(r.rect);
    blocked.is_none() && r.clicked()
}

/// The sidebar section: this session's snapshots and the open file's bookmarks.  `items` receives each entry's
/// action and rect.
pub fn sidebar(side: &mut egui::Ui, caps: &Captures, shown: Option<u32>, items: &mut Vec<(Action, Rect)>) -> Option<Action> {
    if caps.list.is_empty() && caps.marks.is_empty() && caps.refused_lines.is_empty() {
        return None;
    }
    let mut act = None;
    ui::nav_section(side, "Frame snapshots");
    for s in &caps.list {
        let tag = match &s.origin {
            Origin::Reresolved { mark, .. } => format!(" (bookmark #{mark})"),
            Origin::Frozen { .. } => String::new(),
        };
        let label = format!("{}{tag}", s.title());
        let r = ui::nav_item(side, shown == Some(s.id), "❄", &label, None);
        items.push((Action::Show(s.id), r.rect));
        if r.clicked() {
            act = Some(Action::Show(s.id));
        }
    }
    if !caps.marks.is_empty() {
        ui::nav_section(side, "Bookmarks");
        for (i, m) in caps.marks.iter().enumerate() {
            let r = &m.record;
            let label = format!(
                "#{} frame {} ({}){}",
                r.id,
                r.frame,
                if r.premise.w32 { "W32" } else { "R64" },
                if m.refused.is_some() { " - cannot open" } else { "" }
            );
            let resp = ui::nav_item(side, false, "🔖", &label, None);
            items.push((Action::OpenMark(i), resp.rect));
            let resp = match &m.refused {
                Some(why) => resp.on_hover_text(why),
                None => resp.on_hover_text("Re-resolve this frame against the whole saved file"),
            };
            if resp.clicked() {
                act = Some(Action::OpenMark(i));
            }
        }
    }
    if !caps.refused_lines.is_empty() {
        side.horizontal(|h| {
            h.add_space(ui::SPACE_SM);
            h.label(egui::RichText::new(format!("{} bookmark line(s) refused", caps.refused_lines.len())).small())
                .on_hover_text(caps.refused_lines.iter().map(|(n, e)| format!("line {n}: {e}")).collect::<Vec<_>>().join("\n"));
        });
    }
    act
}

/// Where the page drew its controls, and the bar layout it used (for tests).
#[derive(Debug, Clone, Default)]
pub struct Drawn {
    pub reresolve: Option<Rect>,
    pub delete: Option<Rect>,
    pub back: Option<Rect>,
    pub bars: Option<Rect>,
    pub strip: String,
    pub premise: String,
    pub hovered: Option<usize>,
    pub layout: Option<SLayout>,
    pub omitted: Option<String>,
}

const LABEL_W: f32 = 150.0;
const ROW_H: f32 = 14.0;
const MAX_ROWS: u8 = 8;
const HATCH_MAX: usize = 16;

/// The snapshot page.
pub fn page(p: &mut egui::Ui, s: &FrameSnapshot, drawn: &mut Drawn) -> Option<Action> {
    let mut act = None;
    let pal = ui::Palette::of(p);
    ui::page_header(p, &format!("{} snapshot", s.title()), "Observed data, frozen: the live view continues separately.");
    let (claimed, possible, other) = s.lane_counts();
    drawn.strip = match &s.origin {
        Origin::Frozen { taken } => {
            format!("FRAME {} SNAPSHOT - data observed by {taken}; later data may add to this frame.", s.frame)
        }
        Origin::Reresolved { file, mark, snap_bytes, .. } => format!(
            "RE-RESOLVED bookmark #{mark} against the whole saved file {file} ({} bytes, sha256 verified); the original snapshot was its first {snap_bytes} bytes (prefix verified). This is not the original frozen snapshot.",
            s.snap.bytes.len()
        ),
    };
    ui::field_message(p, ui::Tone::Warning, &drawn.strip);
    drawn.premise = format!(
        "premise (frozen): SPU {} · extrapolated pairs {} · analysed {} bytes ({}), sha256 {}… · sources declared: {}",
        if s.premise.w32 { "W32 (no decrementer wrap)" } else { "R64" },
        if s.premise.show_extrapolated { "shown" } else { "hidden" },
        s.snap.bytes.len(),
        if s.snap.prefix_ok { "accepted prefix allowed" } else { "strict" },
        &crate::sha256::hex(&s.snap.sha256)[..16],
        s.sources_declared
    );
    p.label(egui::RichText::new(&drawn.premise).small().color(pal.text_muted));
    p.horizontal(|h| {
        let other_premise = Premise { w32: !s.premise.w32, ..s.premise };
        let r = ui::quiet_button(h, &format!("Re-resolve with {}", if other_premise.w32 { "W32" } else { "R64" }))
            .on_hover_text("Makes a NEW snapshot of the same bytes under the other SPU premise; this one is unchanged.");
        drawn.reresolve = Some(r.rect);
        if r.clicked() {
            act = Some(Action::Reresolve(other_premise));
        }
        let r = ui::danger_button(h, "Delete snapshot");
        drawn.delete = Some(r.rect);
        if r.clicked() {
            act = Some(Action::Delete(s.id));
        }
        let r = ui::quiet_button(h, "Back");
        drawn.back = Some(r.rect);
        if r.clicked() {
            act = Some(Action::Back);
        }
    });
    if let Origin::Reresolved { recorded, .. } = &s.origin {
        let parts: Vec<String> = recorded.iter().map(|(id, st)| format!("{} {}", id.code(), st.word())).collect();
        p.add(egui::Label::new(egui::RichText::new(format!("recorded at freeze: {}", parts.join(", "))).small().color(pal.text_muted)).wrap());
    }
    p.add(
        egui::Label::new(format!(
            "{claimed} claimed (solid core) · {possible} possible: unclaimed, may overlap (faint core, outlined) · {other} of other frames overlap (not drawn) · extrapolated pairs: outlined and hatched, dashed whiskers · frame [{}, {}) = {}",
            s.tb,
            s.next_tb,
            ticks(s, (s.next_tb - s.tb) as f64)
        ))
        .wrap(),
    );
    p.add_space(ui::SPACE_SM);
    draw_bars(p, s, drawn);
    if let Some(o) = &drawn.omitted {
        ui::field_message(p, ui::Tone::Warning, o);
    }
    p.add_space(ui::SPACE_SM);
    ui::section_label(p, "Coverage, as observed");
    for l in s.lanes.iter() {
        p.horizontal(|h| {
            ui::badge(h, l.status.word(), match l.status {
                crate::capture::Status::Observed => ui::Tone::Success,
                crate::capture::Status::Partial => ui::Tone::Warning,
                crate::capture::Status::NoData => ui::Tone::Neutral,
                crate::capture::Status::Unmappable => ui::Tone::Danger,
            });
            h.label(egui::RichText::new(&*l.name).strong());
        });
        p.add(egui::Label::new(egui::RichText::new(l.evidence.lines().join("; ")).color(pal.text_muted)).wrap());
        p.add_space(ui::SPACE_XS);
    }
    act
}

fn ticks(s: &FrameSnapshot, t: f64) -> String {
    match s.freq_hz {
        Some(f) if f > 0 => crate::model::format_ms(t * 1000.0 / f as f64),
        _ => format!("{t:.0} ticks"),
    }
}

fn dashed(painter: &egui::Painter, a: Pos2, b: Pos2, c: Color32) {
    painter.extend(Shape::dashed_line(&[a, b], Stroke::new(1.0_f32, c), 4.0, 3.0));
}

/// Diagonal hatching inside `r`, at most HATCH_MAX segments.
fn hatch(painter: &egui::Painter, r: Rect, c: Color32) {
    let n = ((r.width() + r.height()) / 6.0).ceil().clamp(1.0, HATCH_MAX as f32) as usize;
    let step = (r.width() + r.height()) / n as f32;
    let p = painter.with_clip_rect(r);
    for k in 0..n {
        let x = r.left() - r.height() + step * (k as f32 + 0.5);
        p.line_segment([Pos2::new(x, r.bottom()), Pos2::new(x + r.height(), r.top())], Stroke::new(1.0_f32, c.gamma_multiply(0.8)));
    }
}

fn draw_bars(p: &mut egui::Ui, s: &FrameSnapshot, drawn: &mut Drawn) {
    let pal = ui::Palette::of(p);
    let span = (s.next_tb - s.tb) as f64;
    let (lo, hi) = s.extent;
    let rows: Vec<u8> = s.lanes.iter().map(|l| l.max_depth.min(MAX_ROWS - 1) + 1).collect();
    let mut tops = Vec::with_capacity(rows.len());
    let mut y = 0.0f32;
    for &r in &rows {
        tops.push(y);
        y += r as f32 * ROW_H + 6.0;
    }
    let height = y.max(ROW_H);
    let width = p.available_width();
    let (area, resp) = p.allocate_exact_size(Vec2::new(width, height), Sense::hover());
    drawn.bars = Some(area);
    // Row culling: only lanes whose rows meet the visible clip are laid out.
    let clip = p.clip_rect().intersect(area);
    let first = tops.iter().zip(&rows).position(|(t, r)| area.top() + t + *r as f32 * ROW_H + 6.0 >= clip.top()).unwrap_or(rows.len());
    let last = tops.iter().position(|t| area.top() + t > clip.bottom()).unwrap_or(rows.len());
    let painter = p.painter_at(area);
    let x0 = area.left() + LABEL_W;
    let w = (area.width() - LABEL_W).max(1.0) as f64;
    let view = View { t0: lo, dt: (hi - lo).max(1.0) };
    let l = layout(s, first..last.max(first), view, w, PaintCaps::default());
    let xf = |v: f64| x0 + ((v - view.t0) * w / view.dt) as f32;
    for v in [0.0, span] {
        painter.line_segment([Pos2::new(xf(v), area.top()), Pos2::new(xf(v), area.bottom())], Stroke::new(1.0_f32, pal.accent));
    }
    for (i, lane) in s.lanes.iter().enumerate().take(last.max(first)).skip(first) {
        let top = area.top() + tops[i];
        painter.text(Pos2::new(area.left() + 4.0, top + 2.0), Align2::LEFT_TOP, &*lane.name, FontId::proportional(11.0), pal.text_muted);
        let bottom = top + rows[i] as f32 * ROW_H + 3.0;
        painter.line_segment([Pos2::new(area.left(), bottom), Pos2::new(area.right(), bottom)], Stroke::new(0.5_f32, pal.border));
    }
    let pointer = resp.hover_pos();
    let mut hovered = None;
    for d in &l.draws {
        let b = &s.bars[d.bar];
        let top = area.top() + tops[d.lane as usize] + d.depth.min(MAX_ROWS - 1) as f32 * ROW_H + 1.0;
        let (w0, w1) = (x0 + d.w0 as f32, x0 + d.w1 as f32);
        if paint_one(&painter, s, b, d, top, w0, w1, x0) && pointer.is_some_and(|q| q.y >= top && q.y <= top + ROW_H && q.x >= w0 - 2.0 && q.x <= w1.max(w0 + 1.0) + 2.0) {
            hovered = Some(d.bar);
        }
    }
    drawn.omitted = match l.omitted {
        None => None,
        Some(Some(n)) => Some(format!("{n} bar(s) in view were not drawn (the per-repaint paint cap); zooming is not available on this page yet")),
        Some(None) => Some("drawing stopped at the per-repaint inspection cap; some bars in view are not shown".to_string()),
    };
    drawn.layout = Some(l);
    drawn.hovered = hovered;
    if let Some(i) = hovered {
        let lines = details(s, &s.bars[i]);
        resp.on_hover_ui_at_pointer(|t| {
            for line in &lines {
                t.label(line);
            }
        });
    }
}

/// Paints one laid-out bar; true (always) so the caller can hit-test it.
#[allow(clippy::too_many_arguments)]
fn paint_one(painter: &egui::Painter, s: &FrameSnapshot, b: &CBar, d: &SDraw, top: f32, w0: f32, w1: f32, x0: f32) -> bool {
    let color = ZONE[(b.label as usize) % ZONE.len()];
    let mid = top + ROW_H * 0.5 - 1.0;
    let _ = s;
    if d.tick {
        if d.extrapolated {
            dashed(painter, Pos2::new(w0, top), Pos2::new(w0, top + ROW_H - 2.0), color);
        } else {
            painter.line_segment([Pos2::new(w0, top), Pos2::new(w0, top + ROW_H - 2.0)], Stroke::new(1.5_f32, color));
        }
        return true;
    }
    // Whisker: where it may have started or ended (dashed when extrapolated).
    if d.extrapolated {
        dashed(painter, Pos2::new(w0, mid), Pos2::new(w1, mid), color);
    } else {
        painter.line_segment([Pos2::new(w0, mid), Pos2::new(w1, mid)], Stroke::new(1.0_f32, color));
    }
    for (x, on) in [(w0, !d.arrow_left), (w1, !d.arrow_right)] {
        if on {
            painter.line_segment([Pos2::new(x, mid - 4.0), Pos2::new(x, mid + 4.0)], Stroke::new(1.0_f32, color));
        }
    }
    if d.arrow_left {
        painter.add(Shape::line(vec![Pos2::new(w0 + 6.0, mid - 4.0), Pos2::new(w0, mid), Pos2::new(w0 + 6.0, mid + 4.0)], Stroke::new(1.5_f32, color)));
    }
    if d.arrow_right {
        painter.add(Shape::line(vec![Pos2::new(w1 - 6.0, mid - 4.0), Pos2::new(w1, mid), Pos2::new(w1 - 6.0, mid + 4.0)], Stroke::new(1.5_f32, color)));
    }
    // Core: where it surely ran (inward-rounded; absent under a pixel).
    if let Some((c0, c1)) = d.core {
        let r = Rect::from_min_max(Pos2::new(x0 + c0 as f32, top + 1.0), Pos2::new(x0 + c1 as f32, top + ROW_H - 2.0));
        if d.extrapolated {
            painter.rect_stroke(r, 1.0, Stroke::new(1.0_f32, color));
            hatch(painter, r, color);
        } else if d.class == Class::Claimed {
            painter.rect_filled(r, 1.0, color);
        } else {
            painter.rect_filled(r, 1.0, color.gamma_multiply(0.3));
            painter.rect_stroke(r, 1.0, Stroke::new(1.0_f32, color));
        }
    }
    true
}

/// The tooltip lines of one bar: exact whole-tick bounds (outward-rounded where the exact bound is fractional), with
/// openness.
pub fn details(s: &FrameSnapshot, b: &CBar) -> Vec<String> {
    let lane = &s.lanes[b.lane as usize];
    let foreign = lane.id.foreign();
    let mut out = vec![s.labels.get(b.label as usize).map(|x| x.to_string()).unwrap_or_default()];
    out.push(format!(
        "{}, depth {}: {}",
        lane.name,
        b.depth,
        match b.class {
            Class::Claimed => "CLAIMED by this frame (claimed, not necessarily running throughout it)",
            _ => "POSSIBLE: unclaimed; its bounds may overlap this frame",
        }
    ));
    let unit = if foreign { "source ticks" } else { "ticks" };
    out.push(match b.inclusive {
        Some(i) => format!("inclusive {i} {unit}"),
        None => "inclusive: unknown".into(),
    });
    out.push(match b.exclusive {
        Some(e) => format!("exclusive {e} {unit}"),
        None => "exclusive: unavailable".into(),
    });
    let t = |p| pt_text(p, s.freq_hz);
    if foreign {
        out.push(format!("begin in [{}, {}] from the frame start", t(b.b_lo), t(b.b_hi)));
        out.push(format!("end in [{}, {}]", t(b.e_lo), t(b.e_hi)));
    } else {
        out.push(format!("begin {} from the frame start", t(b.b_lo)));
        if b.flags & (F_OPEN | F_UNAVAIL) == 0 {
            out.push(format!("end {}", t(b.e_hi)));
        }
    }
    if b.flags & F_EXTRAP != 0 {
        out.push("EXTRAPOLATED: a conditional prediction, not a containment guarantee".into());
    }
    if b.flags & F_OPEN != 0 {
        out.push("no END in these bytes".into());
    }
    if b.flags & F_UNAVAIL != 0 {
        out.push("END before BEGIN: duration unavailable".into());
    }
    out
}

const ZONE: [Color32; 8] = [
    Color32::from_rgb(0x4c, 0x8d, 0xf6),
    Color32::from_rgb(0x2e, 0xb8, 0x8a),
    Color32::from_rgb(0xe0, 0x8a, 0x2c),
    Color32::from_rgb(0xb0, 0x6c, 0xe8),
    Color32::from_rgb(0xd9, 0x5c, 0x7a),
    Color32::from_rgb(0x3c, 0xae, 0xc8),
    Color32::from_rgb(0x9a, 0xb8, 0x3a),
    Color32::from_rgb(0xc8, 0x9a, 0x3c),
];
