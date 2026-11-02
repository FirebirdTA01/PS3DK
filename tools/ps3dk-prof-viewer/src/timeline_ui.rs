//! The Timeline page: draws `timeline::layout` output and turns input into
//! `View` changes. All placement and hit testing lives in the tested model.

use crate::model::{format_ms, TimeBase, ZoneKey};
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
        }
    }
}

pub fn draw(
    ctx: &egui::Context,
    a: &Analysis,
    st: &mut TimelineState,
    selected: &mut Option<ZoneKey>,
) {
    let pal = ui::Palette::of_ctx(ctx);
    let tb = TimeBase::of(a);
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
                    for i in 0..st.tl.tracks.len() {
                        draw_track(p, a, st, i, width, tb, selected, &pal);
                        p.add_space(ui::SPACE_XS);
                    }
                });
        });
}

fn toolbar(p: &mut egui::Ui, st: &mut TimelineState, tb: TimeBase, selected: &Option<ZoneKey>) {
    p.horizontal(|p| {
        if ui::quiet_button(p, "Fit").clicked() {
            st.view = View::fit(&st.tl);
        }
        if ui::quiet_button(p, "Zoom in").clicked() {
            st.view.zoom(&st.tl, 0.5, 0.5);
        }
        if ui::quiet_button(p, "Zoom out").clicked() {
            st.view.zoom(&st.tl, 0.5, 2.0);
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
    if resp.dragged() && st.box_from.is_none() {
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
