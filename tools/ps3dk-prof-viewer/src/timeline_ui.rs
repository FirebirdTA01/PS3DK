//! The Timeline page: draws `timeline::layout` output and turns input into
//! `View` changes. All placement and hit testing lives in the tested model.

use crate::foreign::Labels;
use crate::model::{format_ms, TimeBase, ZoneKey};
use crate::timeline_foreign::{self as tf, FDrawKind, FTimeline, QueryCaps};
use std::sync::Arc;
use crate::timeline::{
    bar_details, color_slot, frame_marks, hit, layout, ruler, DrawKind, Timeline, View,
};
use eframe::egui::{self, Align2, Color32, FontId, Pos2, Rect, Sense, Shape, Stroke, Vec2};
use ps3dk_prof::Analysis;
use ps3dk_ui as ui;

const LABEL_W: f32 = 180.0;
/// Height of one depth row of bars.
pub const ROW_H: f32 = 20.0;
/// Space above the first depth row inside a track.
pub const TRACK_PAD: f32 = 4.0;
const RULER_H: f32 = 22.0;

/// Zone colours: distinct hues that read on both the dark and light theme.
const ZONE_COLORS: [Color32; 10] = [
    Color32::from_rgb(0x4c, 0x8d, 0xf6),
    Color32::from_rgb(0x2e, 0xb8, 0x8a),
    Color32::from_rgb(0xe0, 0x8a, 0x2c),
    Color32::from_rgb(0xb0, 0x6c, 0xe8),
    Color32::from_rgb(0xd9, 0x5c, 0x7a),
    Color32::from_rgb(0x3c, 0xae, 0xc8),
    Color32::from_rgb(0x9a, 0xb8, 0x3a),
    Color32::from_rgb(0xc8, 0x7e, 0x5a),
    Color32::from_rgb(0x6c, 0x7e, 0xe0),
    Color32::from_rgb(0xc4, 0xa4, 0x2c),
];

/// Per-page state that survives between frames.
pub struct TimelineState {
    pub tl: Timeline,
    pub view: View,
    box_from: Option<f32>,
    /// Where each track's bar area was drawn last frame, by track index.
    pub track_areas: Vec<Rect>,
    /// The capture is still arriving (a live session): offers Follow.
    pub live: bool,
    /// Keep the window on the newest data as snapshots arrive. Any zoom or
    /// pan by the user turns it off.
    pub follow: bool,
    /// The window shows the whole capture; a newer snapshot of a growing
    /// capture is fitted again rather than keeping the old width.
    pub fit_all: bool,
    /// SPU premise for the SPU/RSX rows: assume no decrementer wrap (W32) instead of R64.
    pub w32: bool,
    pub show_extrapolated: bool,
    /// Collapsed groups: PPU threads, SPU lanes, RSX domains.
    pub collapsed: [bool; 3],
    /// The SPU/RSX rows drawn this frame (from the Document's cache, for the current W32).
    pub foreign: Option<Arc<FTimeline>>,
    /// The axis also covers the finite interpolated SPU/RSX bounds ("Fit all"), not only the PPU data.
    pub axis_all: bool,
}

impl TimelineState {
    pub fn new(a: &Analysis) -> TimelineState {
        let tl = crate::timeline::timeline(a);
        let view = View::fit(&tl);
        TimelineState {
            tl,
            view,
            box_from: None,
            track_areas: Vec::new(),
            live: false,
            follow: false,
            fit_all: true,
            w32: false,
            show_extrapolated: true,
            collapsed: [false; 3],
            foreign: None,
            axis_all: false,
        }
    }

    /// Takes the Document's SPU/RSX rows for the current W32 (dropping any rows built under the other premise
    /// first), and for a capture with no PPU data adopts their extent as the axis.
    pub fn sync_foreign(&mut self, slot: &tf::ForeignTimelineSlot, a: &Analysis) {
        if self.foreign.as_ref().is_some_and(|f| f.w32 != self.w32) {
            self.foreign = None;
        }
        let v = slot.view(a, &self.tl, self.w32);
        if !self.tl.has_data {
            let (s, e) = v.extent.unwrap_or((0, 0));
            if (self.tl.start, self.tl.end) != (s, e) {
                self.tl.start = s;
                self.tl.end = e;
                self.view = View::fit(&self.tl);
                self.fit_all = true;
            }
        }
        // The widened axis: the interpolated SPU/RSX bounds only (never an unbounded or extrapolated extent).
        let extra = if self.axis_all { v.interp_extent } else { None };
        if self.tl.extra != extra {
            self.tl.extra = extra;
            self.view = View::fit(&self.tl);
            self.fit_all = true;
            self.box_from = None;
        }
        self.foreign = Some(v);
    }

    /// Rebuilds the tracks from a newer snapshot of the same capture,
    /// keeping the window, or moving it to the newest data under Follow, or
    /// refitting a whole-capture view. A shift-drag box in progress is kept
    /// when the window stays put, and cancelled when the window moves under
    /// it (its pixels would no longer mean the same times).
    pub fn refresh(&mut self, a: &Analysis) {
        self.tl = crate::timeline::timeline(a);
        self.foreign = None; // the next frame takes the new snapshot's rows
        let before = self.view;
        if self.follow {
            self.view.t0 = self.tl.length() - self.view.dt;
        } else if self.fit_all {
            self.view = View::fit(&self.tl);
        }
        self.view.clamp(&self.tl);
        if self.view != before {
            self.box_from = None;
        }
    }

    /// A shift-drag box is being dragged.
    pub fn box_active(&self) -> bool {
        self.box_from.is_some()
    }
}

pub fn draw(
    ctx: &egui::Context,
    slot: &tf::ForeignTimelineSlot,
    a: &Analysis,
    st: &mut TimelineState,
    selected: &mut Option<ZoneKey>,
) {
    let pal = ui::Palette::of_ctx(ctx);
    let tb = TimeBase::of(a);
    st.sync_foreign(slot, a);
    let labels = Labels::build(a);
    egui::CentralPanel::default()
        .frame(
            egui::Frame::none()
                .fill(pal.bg)
                .inner_margin(egui::Margin::symmetric(ui::SPACE_XL, ui::SPACE_LG)),
        )
        .show(ctx, |p| {
            ui::page_header(
                p,
                "Timeline",
                "Every lane's spans by depth, with frame markers. Hatched lanes are not MEASUREMENT VALID.",
            );
            if tb.freq_hz.is_none() {
                ui::field_message(
                    p,
                    ui::Tone::Warning,
                    "No usable clock: the ruler and durations are in timebase ticks.",
                );
            }
            st.track_areas.clear();
            toolbar(p, st, tb, &*selected);
            let before = st.view;
            p.add_space(ui::SPACE_SM);
            let width = (p.available_width() - LABEL_W).max(100.0);
            draw_ruler(p, st, tb, width, &pal);
            p.spacing_mut().scroll = egui::style::ScrollStyle::solid();
            let w = &mut p.visuals_mut().widgets;
            w.inactive.bg_fill = pal.text_faint;
            w.hovered.bg_fill = pal.text_muted;
            w.active.bg_fill = pal.text_muted;
            // Drag belongs to the timeline (pan and box zoom), not to the
            // lane list; the list scrolls with its bar.
            egui::ScrollArea::vertical()
                .drag_to_scroll(false)
                .auto_shrink([false, false])
                .scroll_bar_visibility(egui::scroll_area::ScrollBarVisibility::AlwaysVisible)
                .show(p, |p| {
                    let fv = st.foreign.clone().unwrap_or_default();
                    let spu = fv.tracks.iter().filter(|t| matches!(t.source, ps3dk_prof::ext::SourceKey::Lane(_))).count();
                    let rsx = fv.tracks.len() - spu;
                    if group_header(p, st, 0, &format!("PPU threads ({})", st.tl.tracks.len()), &pal) {
                        for i in 0..st.tl.tracks.len() {
                            draw_track(p, a, st, i, width, tb, selected, &pal);
                            p.add_space(ui::SPACE_XS);
                        }
                    }
                    if a.capture.ext.is_some() {
                        if let Some(n) = &fv.notice {
                            ui::field_message(p, ui::Tone::Warning, n);
                        }
                        p.add(egui::Label::new(egui::RichText::new(&fv.caption).small().color(pal.text_muted)).wrap());
                        for (g, title, want_gpu) in [(1usize, format!("SPU lanes ({spu})"), false), (2, format!("RSX domains ({rsx})"), true)] {
                            if group_header(p, st, g, &title, &pal) {
                                for (i, t) in fv.tracks.iter().enumerate() {
                                    if matches!(t.source, ps3dk_prof::ext::SourceKey::Gpu(_)) == want_gpu {
                                        draw_foreign_track(p, a, st, &labels, &fv, i, width, tb, &pal);
                                        p.add_space(ui::SPACE_XS);
                                    }
                                }
                            }
                        }
                    }
                });
            // Any change the user made to the window on the tracks ends Follow;
            // whether it now shows everything decides refitting.
            if st.view != before {
                st.follow = false;
                st.fit_all = st.view == View::fit(&st.tl);
            }
        });
}

fn toolbar(p: &mut egui::Ui, st: &mut TimelineState, tb: TimeBase, selected: &Option<ZoneKey>) {
    p.horizontal_wrapped(|p| {
        if ui::quiet_button(p, "Fit").clicked() {
            st.view = View::fit(&st.tl);
            st.follow = false;
            st.fit_all = true;
        }
        if ui::quiet_button(p, "Zoom in").clicked() {
            st.view.zoom(&st.tl, 0.5, 0.5);
            st.follow = false;
            st.fit_all = st.view == View::fit(&st.tl);
        }
        if ui::quiet_button(p, "Zoom out").clicked() {
            st.view.zoom(&st.tl, 0.5, 2.0);
            st.follow = false;
            st.fit_all = st.view == View::fit(&st.tl);
        }
        p.add_space(ui::SPACE_SM);
        p.checkbox(&mut st.w32, "SPU: assume no decrementer wrap (W32)");
        p.checkbox(&mut st.show_extrapolated, "Show extrapolated SPU/RSX bars");
        p.checkbox(&mut st.axis_all, "Fit all: axis includes SPU/RSX bounds");
        if st.live {
            p.add_space(ui::SPACE_SM);
            let mut follow = st.follow;
            if p.checkbox(&mut follow, "Follow newest data").changed() {
                st.follow = follow;
                if follow {
                    st.fit_all = false;
                    st.view.t0 = st.tl.length() - st.view.dt;
                    st.view.clamp(&st.tl);
                }
            }
        }
        p.add_space(ui::SPACE_LG);
        let span = match tb.ms(st.view.dt) {
            Some(ms) => format_ms(ms),
            None => format!("{:.0} ticks", st.view.dt),
        };
        p.label(egui::RichText::new(format!("Window {span}")).strong());
        if let Some((name, tag)) = selected {
            p.add_space(ui::SPACE_LG);
            let label = if tag.is_empty() {
                name.clone()
            } else {
                format!("{name} [{tag}]")
            };
            p.label(format!("Highlighted: {label}"));
        }
    });
    let faint = ui::Palette::of(p).text_faint;
    p.label(
        egui::RichText::new(
            "Wheel: zoom at the pointer. Drag: pan. Shift+drag: zoom to the box. Double-click: fit. Click a span: highlight its zone.",
        )
        .small()
        .color(faint),
    );
}

fn draw_ruler(p: &mut egui::Ui, st: &TimelineState, tb: TimeBase, width: f32, pal: &ui::Palette) {
    let (rect, _) = p.allocate_exact_size(Vec2::new(LABEL_W + width, RULER_H), Sense::hover());
    let painter = p.painter_at(rect);
    let x0 = rect.left() + LABEL_W;
    painter.line_segment(
        [
            Pos2::new(x0, rect.bottom()),
            Pos2::new(rect.right(), rect.bottom()),
        ],
        Stroke::new(1.0_f32, pal.border),
    );
    for (x, label) in ruler(&st.view, tb, width as f64, 90.0) {
        let x = x0 + x as f32;
        painter.line_segment(
            [
                Pos2::new(x, rect.bottom() - 6.0),
                Pos2::new(x, rect.bottom()),
            ],
            Stroke::new(1.0_f32, pal.text_faint),
        );
        painter.text(
            Pos2::new(x + 3.0, rect.top() + 2.0),
            Align2::LEFT_TOP,
            label,
            FontId::proportional(11.0),
            pal.text_muted,
        );
    }
}

#[allow(clippy::too_many_arguments)]
fn draw_track(
    p: &mut egui::Ui,
    a: &Analysis,
    st: &mut TimelineState,
    index: usize,
    width: f32,
    tb: TimeBase,
    selected: &mut Option<ZoneKey>,
    pal: &ui::Palette,
) {
    let track = &st.tl.tracks[index];
    // An invalid lane's reasons wrap under its name; the row grows to fit.
    let reasons = (!track.valid && !track.reasons.is_empty()).then(|| {
        p.fonts(|f| {
            f.layout(
                track.reasons.clone(),
                FontId::proportional(11.0),
                pal.text_muted,
                LABEL_W - ui::SPACE_SM,
            )
        })
    });
    let label_h = 34.0 + reasons.as_ref().map_or(0.0, |g| g.size().y + 4.0);
    let height = ((track.max_depth as f32 + 1.0) * ROW_H + 2.0 * TRACK_PAD)
        .max(44.0)
        .max(label_h);
    let (rect, _) = p.allocate_exact_size(Vec2::new(LABEL_W + width, height), Sense::hover());
    let label_rect =
        Rect::from_min_size(rect.min, Vec2::new(LABEL_W - ui::SPACE_SM, rect.height()));
    let area = Rect::from_min_max(Pos2::new(rect.left() + LABEL_W, rect.top()), rect.max);
    st.track_areas.push(area);
    let id = p.id().with(("track", track.incarnation));
    let resp = p.interact(area, id, Sense::click_and_drag());

    // Lane header.
    let painter = p.painter();
    let name = if track.name.is_empty() {
        format!("lane {}", track.incarnation)
    } else {
        format!("{} ({})", track.name, track.incarnation)
    };
    painter.text(
        label_rect.left_top() + Vec2::new(0.0, 2.0),
        Align2::LEFT_TOP,
        name,
        FontId::proportional(13.0),
        pal.text,
    );
    let (status, color) = if track.valid {
        ("VALID", pal.success)
    } else {
        ("NOT VALID", pal.warning)
    };
    painter.text(
        label_rect.left_top() + Vec2::new(0.0, 20.0),
        Align2::LEFT_TOP,
        status,
        FontId::proportional(11.0),
        color,
    );
    if let Some(galley) = reasons {
        let at = label_rect.left_top() + Vec2::new(0.0, 34.0);
        p.painter_at(label_rect).galley(at, galley, pal.text_muted);
    }

    // Track background, hatched when the lane is not valid.
    let painter = p.painter_at(area);
    painter.rect_filled(area, 2.0, pal.surface_alt);
    if !track.valid {
        let step = 8.0;
        let mut x = area.left() - area.height();
        while x < area.right() {
            painter.line_segment(
                [
                    Pos2::new(x, area.bottom()),
                    Pos2::new(x + area.height(), area.top()),
                ],
                Stroke::new(1.0_f32, pal.warning.gamma_multiply(0.25)),
            );
            x += step;
        }
    }
    for fx in frame_marks(&st.tl, track, &st.view, width as f64, 3.0) {
        let x = area.left() + fx as f32;
        painter.line_segment(
            [Pos2::new(x, area.top()), Pos2::new(x, area.bottom())],
            Stroke::new(1.0_f32, pal.border_strong),
        );
    }

    let draws = layout(&st.tl, track, &st.view, width as f64);
    for d in &draws {
        let top = area.top() + TRACK_PAD + d.depth as f32 * ROW_H;
        let x0 = area.left() + d.x0 as f32;
        let x1 = area.left() + d.x1 as f32;
        match d.kind {
            DrawKind::Dense { .. } => {
                let r = Rect::from_min_max(
                    Pos2::new(x0, top + 1.0),
                    Pos2::new(x1.max(x0 + 1.0), top + ROW_H - 1.0),
                );
                painter.rect_filled(r, 0.0, pal.text_faint);
            }
            DrawKind::Bar(i) => {
                let bar = &track.bars[i];
                let mut color = ZONE_COLORS[color_slot(&bar.key, ZONE_COLORS.len())];
                if selected.as_ref().is_some_and(|k| *k != bar.key) {
                    color = color.gamma_multiply(0.3);
                }
                if d.unavailable {
                    let c = Pos2::new(x0, top + ROW_H / 2.0);
                    painter.add(Shape::convex_polygon(
                        vec![
                            c + Vec2::new(0.0, -6.0),
                            c + Vec2::new(4.0, 0.0),
                            c + Vec2::new(0.0, 6.0),
                            c + Vec2::new(-4.0, 0.0),
                        ],
                        pal.warning,
                        Stroke::NONE,
                    ));
                    continue;
                }
                let r = Rect::from_min_max(
                    Pos2::new(x0, top + 1.0),
                    Pos2::new(x1.max(x0 + 1.0), top + ROW_H - 1.0),
                );
                painter.rect_filled(r, 2.0, color);
                if selected.as_ref() == Some(&bar.key) {
                    painter.rect_stroke(r, 2.0, Stroke::new(1.5_f32, pal.text));
                }
                if d.open_end {
                    // Open cap: a chevron at the data edge, not an end.
                    let ex = r.right();
                    let mid = r.center().y;
                    painter.add(Shape::line(
                        vec![
                            Pos2::new(ex - 6.0, r.top()),
                            Pos2::new(ex, mid),
                            Pos2::new(ex - 6.0, r.bottom()),
                        ],
                        Stroke::new(2.5_f32, pal.warning),
                    ));
                }
                if r.width() > 40.0 {
                    let text_clip = r.shrink2(Vec2::new(4.0, 0.0)).intersect(area);
                    p.painter_at(text_clip).text(
                        Pos2::new(text_clip.left(), r.center().y),
                        Align2::LEFT_CENTER,
                        &bar.key.0,
                        FontId::proportional(11.0),
                        Color32::WHITE,
                    );
                }
            }
        }
    }

    // Box being dragged.
    if let (Some(from), Some(pos)) = (st.box_from, resp.interact_pointer_pos()) {
        if resp.dragged() {
            let r = Rect::from_x_y_ranges(
                (area.left() + from).min(pos.x)..=(area.left() + from).max(pos.x),
                area.y_range(),
            );
            painter.rect_filled(r, 0.0, pal.accent.gamma_multiply(0.2));
        }
    }

    handle_input(p, st, index, &resp, area, width, &draws, a, tb, selected);
}

#[allow(clippy::too_many_arguments)]
fn handle_input(
    p: &mut egui::Ui,
    st: &mut TimelineState,
    index: usize,
    resp: &egui::Response,
    area: Rect,
    width: f32,
    draws: &[crate::timeline::Draw],
    a: &Analysis,
    tb: TimeBase,
    selected: &mut Option<ZoneKey>,
) {
    let w = width as f64;
    // Wheel zooms here; the lane list's own scrolling must not also take it.
    if let Some(pos) = resp.hover_pos() {
        let (dy, dx) = p.input(|i| (i.smooth_scroll_delta.y, i.smooth_scroll_delta.x));
        if dy != 0.0 || dx != 0.0 {
            let anchor = f64::from(pos.x - area.left()) / w;
            if dy != 0.0 {
                st.view.zoom(&st.tl, anchor, (-f64::from(dy) * 0.002).exp());
            }
            if dx != 0.0 {
                st.view.pan(&st.tl, -f64::from(dx) / w);
            }
            p.ctx().input_mut(|i| i.smooth_scroll_delta = Vec2::ZERO);
        }
    }
    let shift = p.input(|i| i.modifiers.shift);
    if resp.drag_started() && shift {
        // The box starts where the button went down, not where egui
        // recognised the drag (a few pixels later).
        if let Some(pos) = p.input(|i| i.pointer.press_origin()) {
            st.box_from = Some(pos.x - area.left());
        }
    }
    // A shift-drag is a box (or a box cancelled by a refresh), never a pan.
    if resp.dragged() && st.box_from.is_none() && !shift {
        // On the first drag frame, pan by everything since the press, so the
        // pixels moved before egui recognised the drag are not lost.
        let dx = match (resp.drag_started(), p.input(|i| i.pointer.press_origin())) {
            (true, Some(origin)) => resp.interact_pointer_pos().map_or(0.0, |q| q.x - origin.x),
            _ => resp.drag_delta().x,
        };
        st.view.pan(&st.tl, -f64::from(dx) / w);
    }
    if resp.drag_stopped() {
        if let (Some(from), Some(pos)) = (st.box_from.take(), resp.interact_pointer_pos()) {
            let to = pos.x - area.left();
            if (to - from).abs() > 3.0 {
                st.view
                    .select(&st.tl, f64::from(from) / w, f64::from(to) / w);
            }
        }
    }
    if resp.double_clicked() {
        st.view = View::fit(&st.tl);
        return;
    }

    let Some(pos) = resp.hover_pos() else { return };
    let rel = pos.y - area.top() - TRACK_PAD;
    if rel < 0.0 {
        return;
    }
    let depth = (rel / ROW_H) as u8;
    let x = f64::from(pos.x - area.left());
    let track = &st.tl.tracks[index];
    if let Some(i) = hit(draws, depth, x) {
        let bar = track.bars[i].clone();
        let lines = bar_details(a, &bar, tb);
        resp.clone().on_hover_ui_at_pointer(|t| {
            for (n, l) in lines.iter().enumerate() {
                if n == 0 {
                    t.label(egui::RichText::new(l).strong());
                } else {
                    t.label(l);
                }
            }
        });
        if resp.clicked() {
            *selected = if selected.as_ref() == Some(&bar.key) {
                None
            } else {
                Some(bar.key)
            };
        }
    } else if let Some(n) = draws.iter().find_map(|d| match d.kind {
        DrawKind::Dense { count } if d.depth == depth && x >= d.x0 - 1.0 && x <= d.x1 + 1.0 => {
            Some(count)
        }
        _ => None,
    }) {
        resp.clone().on_hover_ui_at_pointer(|t| {
            t.label(format!(
                "{n} spans narrower than a pixel: zoom in to see them"
            ));
        });
    } else if resp.clicked() && !shift {
        *selected = None;
    }
}

/// A collapsible group title; true when the group is expanded.
fn group_header(p: &mut egui::Ui, st: &mut TimelineState, g: usize, title: &str, pal: &ui::Palette) -> bool {
    let mark = if st.collapsed[g] { "+" } else { "-" };
    let r = p.add(
        egui::Label::new(egui::RichText::new(format!("{mark} {title}")).strong().color(pal.text)).sense(Sense::click()),
    );
    if r.clicked() {
        st.collapsed[g] = !st.collapsed[g];
    }
    !st.collapsed[g]
}

#[allow(clippy::too_many_arguments)]
fn draw_foreign_track(
    p: &mut egui::Ui,
    a: &Analysis,
    st: &mut TimelineState,
    labels: &Labels,
    fv: &FTimeline,
    index: usize,
    width: f32,
    tb: TimeBase,
    pal: &ui::Palette,
) {
    let t = &fv.tracks[index];
    let mut text = vec![t.header.clone()];
    text.extend(t.coverage.iter().cloned());
    if let Some(r) = &t.refusal {
        text.push(r.clone());
    }
    let galley = p.fonts(|f| f.layout(text.join("\n"), FontId::proportional(11.0), pal.text_muted, LABEL_W - ui::SPACE_SM));
    let label_h = 20.0 + galley.size().y + 4.0;
    let rows = if t.bars.is_empty() { 0.0 } else { t.max_depth as f32 + 1.0 };
    let height = (rows * ROW_H + 2.0 * TRACK_PAD).max(44.0).max(label_h);
    let (rect, _) = p.allocate_exact_size(Vec2::new(LABEL_W + width, height), Sense::hover());
    let label_rect = Rect::from_min_size(rect.min, Vec2::new(LABEL_W - ui::SPACE_SM, rect.height()));
    let area = Rect::from_min_max(Pos2::new(rect.left() + LABEL_W, rect.top()), rect.max);
    let id = p.id().with(("ftrack", index));
    let resp = p.interact(area, id, Sense::click_and_drag());
    let painter = p.painter();
    painter.text(label_rect.left_top() + Vec2::new(0.0, 2.0), Align2::LEFT_TOP, &t.label, FontId::proportional(13.0), pal.text);
    p.painter_at(label_rect).galley(label_rect.left_top() + Vec2::new(0.0, 20.0), galley, pal.text_muted);

    let painter = p.painter_at(area);
    painter.rect_filled(area, 2.0, pal.surface_alt);
    let w = width as f64;
    let l = tf::layout(t, &st.view, w, QueryCaps::default(), st.show_extrapolated);
    for d in &l.draws {
        let top = area.top() + TRACK_PAD + d.depth as f32 * ROW_H;
        let mid = top + ROW_H / 2.0;
        let (w0, w1) = (area.left() + d.w0 as f32, area.left() + d.w1 as f32);
        match d.kind {
            FDrawKind::Dense { .. } => {
                let r = Rect::from_min_max(Pos2::new(w0, top + 1.0), Pos2::new(w1.max(w0 + 1.0), top + ROW_H - 1.0));
                if d.extrapolated {
                    // An extrapolated run: outlined and hatched, never filled like interpolated bounds.
                    painter.rect_stroke(r, 0.0, Stroke::new(1.0_f32, pal.warning));
                    hatch(&p.painter_at(r), r, pal.warning);
                } else {
                    painter.rect_filled(r, 0.0, pal.text_faint);
                }
            }
            FDrawKind::Bar(i) => {
                let b = &t.bars[i];
                let key: ZoneKey = (labels.zone(a, b.bar.image, b.bar.desc), String::new());
                let color = ZONE_COLORS[color_slot(&key, ZONE_COLORS.len())];
                if d.tick {
                    // Under a pixel: a tick with its own hover (dashed when extrapolated).
                    let x = w0.max(area.left());
                    if d.extrapolated {
                        dashed(&painter, Pos2::new(x, top + 1.0), Pos2::new(x, top + ROW_H - 1.0), color);
                    } else {
                        painter.line_segment([Pos2::new(x, top + 1.0), Pos2::new(x, top + ROW_H - 1.0)], Stroke::new(1.5_f32, color));
                    }
                    continue;
                }
                // Whiskers: where it may have started or ended (dashed when extrapolated: a prediction).
                if d.extrapolated {
                    dashed(&painter, Pos2::new(w0, mid), Pos2::new(w1, mid), color);
                } else {
                    painter.line_segment([Pos2::new(w0, mid), Pos2::new(w1, mid)], Stroke::new(1.0_f32, color));
                }
                for (x, on) in [(w0, !d.arrow_left), (w1, !d.arrow_right)] {
                    if on {
                        painter.line_segment([Pos2::new(x, mid - 4.0), Pos2::new(x, mid + 4.0)], Stroke::new(1.0_f32, color));
                    }
                }
                if d.arrow_left {
                    let pts = vec![Pos2::new(w0 + 6.0, mid - 4.0), Pos2::new(w0, mid), Pos2::new(w0 + 6.0, mid + 4.0)];
                    painter.add(Shape::line(pts, Stroke::new(1.5_f32, color)));
                }
                if d.arrow_right {
                    let pts = vec![Pos2::new(w1 - 6.0, mid - 4.0), Pos2::new(w1, mid), Pos2::new(w1 - 6.0, mid + 4.0)];
                    painter.add(Shape::line(pts, Stroke::new(1.5_f32, color)));
                }
                // Core: where it surely ran.
                if let Some((c0, c1)) = d.core {
                    let r = Rect::from_min_max(
                        Pos2::new(area.left() + c0 as f32, top + 2.0),
                        Pos2::new(area.left() + c1 as f32, top + ROW_H - 2.0),
                    );
                    if d.extrapolated {
                        // Hatched outline: a conditional prediction, never styled like an interpolated bound.
                        painter.rect_stroke(r, 2.0, Stroke::new(1.0_f32, color));
                        hatch(&p.painter_at(r), r, color);
                    } else {
                        painter.rect_filled(r, 2.0, color);
                        if r.width() > 40.0 {
                            let clip = r.shrink2(Vec2::new(4.0, 0.0)).intersect(area);
                            p.painter_at(clip).text(
                                Pos2::new(clip.left(), r.center().y),
                                Align2::LEFT_CENTER,
                                &key.0,
                                FontId::proportional(11.0),
                                Color32::WHITE,
                            );
                        }
                    }
                }
            }
        }
    }
    if let Some(o) = l.omitted {
        let msg = match o {
            Some(n) => format!("{n} bars not drawn at this zoom; zoom in"),
            None => "additional bars omitted at this zoom (count unknown); zoom in".to_string(),
        };
        painter.text(area.right_bottom() - Vec2::new(4.0, 2.0), Align2::RIGHT_BOTTOM, msg, FontId::proportional(11.0), pal.warning);
    }
    // Wheel and drag act on the shared window, as on a PPU track.
    let wf = width as f64;
    if let Some(pos) = resp.hover_pos() {
        let (dy, dx) = p.input(|i| (i.smooth_scroll_delta.y, i.smooth_scroll_delta.x));
        if dy != 0.0 || dx != 0.0 {
            let anchor = f64::from(pos.x - area.left()) / wf;
            if dy != 0.0 {
                st.view.zoom(&st.tl, anchor, (-f64::from(dy) * 0.002).exp());
            }
            if dx != 0.0 {
                st.view.pan(&st.tl, -f64::from(dx) / wf);
            }
            p.ctx().input_mut(|i| i.smooth_scroll_delta = Vec2::ZERO);
        }
        let rel = pos.y - area.top() - TRACK_PAD;
        if rel >= 0.0 {
            let depth = (rel / ROW_H) as u8;
            let x = f64::from(pos.x - area.left());
            // One reverse paint-order test for bars and dense runs alike: the topmost primitive answers.
            let topmost = tf::hit_any(&l, depth, x);
            if let Some(tf::FHit::Bar(i)) = topmost {
                let lines = tf::details(a, labels, t, i, tb.freq_hz);
                resp.clone().on_hover_ui_at_pointer(|tt| {
                    for (n, line) in lines.iter().enumerate() {
                        if n == 0 {
                            tt.label(egui::RichText::new(line).strong());
                        } else {
                            tt.label(line);
                        }
                    }
                });
            } else if let Some(tf::FHit::Dense { count, extrapolated }) = topmost {
                let line = tf::dense_text(count, extrapolated);
                resp.clone().on_hover_ui_at_pointer(|tt| {
                    tt.label(line);
                });
            }
        }
    }
    if resp.dragged() {
        st.view.pan(&st.tl, -f64::from(resp.drag_delta().x) / wf);
    }
}

/// Diagonal hatching inside `r` (clipped by the painter).
fn hatch(painter: &egui::Painter, r: Rect, color: Color32) {
    let mut x = r.left() - r.height();
    while x < r.right() {
        painter.line_segment([Pos2::new(x, r.bottom()), Pos2::new(x + r.height(), r.top())], Stroke::new(1.0_f32, color));
        x += 6.0;
    }
}

/// A dashed segment from `a` to `b` (4 px on, 3 px off).
fn dashed(painter: &egui::Painter, a: Pos2, b: Pos2, color: Color32) {
    let len = a.distance(b);
    if len < 1.0 {
        painter.circle_filled(a, 1.0, color);
        return;
    }
    let dir = (b - a) / len;
    let mut t = 0.0;
    while t < len {
        let e = (t + 4.0).min(len);
        painter.line_segment([a + dir * t, a + dir * e], Stroke::new(1.0_f32, color));
        t += 7.0;
    }
}
