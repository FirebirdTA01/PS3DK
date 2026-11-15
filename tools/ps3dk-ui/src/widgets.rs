//! Building blocks for a PS3DK tool window.
//!
//! A window is an [`app_bar`] on top, an optional [`sidebar`] of
//! [`nav_item`]s on the left, a [`status_bar`] at the bottom and the page in
//! [`page`], made of [`card`]s.  Everything here takes its colours from
//! [`Palette`], so it follows the active theme.

use std::hash::Hash;

use egui::{
    Align, Align2, Color32, Context, FontId, Frame, Id, Layout, Margin, Order, Response, RichText,
    Rounding, Sense, Stroke, Ui, WidgetInfo, WidgetText, WidgetType,
};

use crate::{
    caption_style, stroke, title_style, Palette, Tone, RADIUS_LG, RADIUS_MD, SIDEBAR_WIDTH, SPACE_LG,
    SPACE_MD, SPACE_SM, SPACE_XL, SPACE_XS,
};

// ---------------------------------------------------------------------------
// Window chrome
// ---------------------------------------------------------------------------

/// The bar across the top of the window: the tool's mark and name on the
/// left (`left`), its document actions on the right (`right`, laid out right
/// to left, so add the primary action first).
pub fn app_bar(
    ctx: &Context,
    left: impl FnOnce(&mut Ui),
    right: impl FnOnce(&mut Ui),
) {
    let p = Palette::of_ctx(ctx);
    egui::TopBottomPanel::top("ps3dk_app_bar")
        .frame(
            Frame::none()
                .fill(p.panel)
                .inner_margin(Margin::symmetric(SPACE_LG, SPACE_MD - 2.0)),
        )
        .show(ctx, |ui| {
            ui.horizontal(|ui| {
                left(ui);
                ui.with_layout(Layout::right_to_left(Align::Center), right);
            });
        });
}

/// The tool's mark: its two-letter monogram on an accent tile, then its name
/// and an optional document line underneath.  Returns the document line's
/// response, so the caller can attach a tooltip (the full path, say).
pub fn app_title(
    ui: &mut Ui,
    monogram: &str,
    name: &str,
    document: Option<WidgetText>,
) -> Option<Response> {
    brand_mark(ui, monogram, 30.0);
    ui.add_space(SPACE_XS);
    ui.vertical(|ui| {
        ui.spacing_mut().item_spacing.y = 0.0;
        ui.label(RichText::new(name).text_style(title_style()).strong());
        document.map(|document| ui.label(document))
    })
    .inner
}

/// An accent tile with a short monogram, the icon every PS3DK tool uses.
pub fn brand_mark(ui: &mut Ui, monogram: &str, size: f32) -> Response {
    let p = Palette::of(ui);
    let (rect, response) = ui.allocate_exact_size(egui::vec2(size, size), Sense::hover());
    if ui.is_rect_visible(rect) {
        let painter = ui.painter();
        painter.rect_filled(rect, Rounding::same(size * 0.26), p.accent);
        painter.text(
            rect.center(),
            Align2::CENTER_CENTER,
            monogram,
            FontId::proportional(size * 0.42),
            p.on_accent,
        );
    }
    response
}

/// The navigation column on the left.  Fill it with [`nav_section`] and
/// [`nav_item`]; anything added with `bottom` sticks to the bottom edge.
pub fn sidebar(
    ctx: &Context,
    contents: impl FnOnce(&mut Ui),
    bottom: impl FnOnce(&mut Ui),
) {
    let p = Palette::of_ctx(ctx);
    egui::SidePanel::left("ps3dk_sidebar")
        .exact_width(SIDEBAR_WIDTH)
        .resizable(false)
        .frame(
            Frame::none()
                .fill(p.panel)
                .inner_margin(Margin::symmetric(SPACE_SM + 2.0, SPACE_LG)),
        )
        .show(ctx, |ui| {
            ui.with_layout(Layout::bottom_up(Align::Min), |ui| {
                bottom(ui);
                ui.with_layout(Layout::top_down(Align::Min), contents);
            });
        });
}

/// A small upper-case label that heads a group of nav items or fields.
pub fn nav_section(ui: &mut Ui, label: &str) {
    let p = Palette::of(ui);
    ui.add_space(SPACE_SM);
    ui.horizontal(|ui| {
        ui.add_space(SPACE_SM);
        ui.label(
            RichText::new(label.to_uppercase())
                .text_style(caption_style())
                .color(p.text_faint),
        );
    });
    ui.add_space(SPACE_XS);
}

/// One page link in the sidebar.  `count` shows a number on the right
/// (entries on that page, say); `None` hides it.
pub fn nav_item(
    ui: &mut Ui,
    selected: bool,
    icon: &str,
    label: &str,
    count: Option<usize>,
) -> Response {
    let p = Palette::of(ui);
    let height = 34.0;
    let (rect, response) =
        ui.allocate_exact_size(egui::vec2(ui.available_width(), height), Sense::click());
    response.widget_info(|| WidgetInfo::selected(WidgetType::SelectableLabel, true, selected, label));
    if ui.is_rect_visible(rect) {
        let painter = ui.painter();
        if selected {
            painter.rect_filled(rect, Rounding::same(RADIUS_MD), p.accent_soft);
            let bar = egui::Rect::from_min_size(
                rect.left_top() + egui::vec2(0.0, 8.0),
                egui::vec2(3.0, height - 16.0),
            );
            painter.rect_filled(bar, Rounding::same(1.5), p.accent);
        } else if response.hovered() {
            painter.rect_filled(rect, Rounding::same(RADIUS_MD), p.hover);
        }
        let text_color = if selected { p.text } else { p.text_muted };
        let icon_color = if selected { p.accent } else { p.text_faint };
        painter.text(
            rect.left_center() + egui::vec2(SPACE_LG, 0.0),
            Align2::CENTER_CENTER,
            icon,
            FontId::proportional(15.0),
            icon_color,
        );
        painter.text(
            rect.left_center() + egui::vec2(SPACE_LG + 18.0, 0.0),
            Align2::LEFT_CENTER,
            label,
            FontId::proportional(14.0),
            text_color,
        );
        if let Some(count) = count {
            painter.text(
                rect.right_center() - egui::vec2(SPACE_MD, 0.0),
                Align2::RIGHT_CENTER,
                count.to_string(),
                FontId::proportional(12.0),
                p.text_faint,
            );
        }
    }
    response.on_hover_cursor(egui::CursorIcon::PointingHand)
}

/// The bar across the bottom: a tone dot and the latest message on the left,
/// `detail` (document facts, say) on the right.
pub fn status_bar(ctx: &Context, tone: Tone, message: &str, detail: &str) {
    let p = Palette::of_ctx(ctx);
    egui::TopBottomPanel::bottom("ps3dk_status_bar")
        .frame(
            Frame::none()
                .fill(p.panel)
                .inner_margin(Margin::symmetric(SPACE_LG, SPACE_XS + 2.0)),
        )
        .show(ctx, |ui| {
            // Right to left, so the detail keeps its width and the message
            // truncates into whatever is left.
            ui.with_layout(Layout::right_to_left(Align::Center), |ui| {
                ui.label(RichText::new(detail).small().color(p.text_faint));
                ui.add_space(SPACE_LG);
                ui.with_layout(Layout::left_to_right(Align::Center), |ui| {
                    let (dot, _) = ui.allocate_exact_size(egui::vec2(8.0, 8.0), Sense::hover());
                    ui.painter()
                        .circle_filled(dot.center(), 4.0, tone.color(&p));
                    let color = match tone {
                        Tone::Danger => p.danger,
                        _ => p.text_muted,
                    };
                    ui.add(
                        egui::Label::new(RichText::new(message).small().color(color)).truncate(),
                    )
                    .on_hover_text(message);
                });
            });
        });
}

/// The scrolling page area: a background fill, comfortable margins and a
/// readable maximum width.
pub fn page(ctx: &Context, contents: impl FnOnce(&mut Ui)) {
    let p = Palette::of_ctx(ctx);
    egui::CentralPanel::default()
        .frame(Frame::none().fill(p.bg))
        .show(ctx, |ui| {
            egui::ScrollArea::vertical()
                .auto_shrink([false, false])
                .show(ui, |ui| {
                    Frame::none()
                        .inner_margin(Margin::symmetric(SPACE_XL + 4.0, SPACE_XL))
                        .show(ui, |ui| {
                            let width = ui.available_width().min(1080.0);
                            ui.set_max_width(width);
                            contents(ui);
                        });
                });
        });
}

/// A page title with a one-line explanation under it.
pub fn page_header(ui: &mut Ui, title: &str, subtitle: &str) {
    let p = Palette::of(ui);
    ui.label(RichText::new(title).heading().strong().color(p.text));
    if !subtitle.is_empty() {
        ui.add_space(-SPACE_XS);
        ui.label(RichText::new(subtitle).color(p.text_muted));
    }
    ui.add_space(SPACE_LG);
}

// ---------------------------------------------------------------------------
// Content
// ---------------------------------------------------------------------------

/// A raised panel that groups related content.  Cards fill the page width.
pub fn card<R>(ui: &mut Ui, contents: impl FnOnce(&mut Ui) -> R) -> R {
    let p = Palette::of(ui);
    let inner = Frame::none()
        .fill(p.surface)
        .stroke(stroke(1.0, p.border))
        .rounding(Rounding::same(RADIUS_LG))
        .inner_margin(Margin::same(SPACE_LG + 2.0))
        .show(ui, |ui| {
            ui.set_width(ui.available_width());
            contents(ui)
        })
        .inner;
    ui.add_space(SPACE_MD);
    inner
}

/// A card's title row: the title, an optional muted subtitle, and room on the
/// right (`trailing`, laid out right to left) for a badge or an action.
pub fn card_header(
    ui: &mut Ui,
    title: &str,
    subtitle: Option<&str>,
    trailing: impl FnOnce(&mut Ui),
) {
    let p = Palette::of(ui);
    ui.horizontal(|ui| {
        ui.vertical(|ui| {
            ui.spacing_mut().item_spacing.y = 2.0;
            ui.label(RichText::new(title).text_style(title_style()).strong().color(p.text));
            if let Some(subtitle) = subtitle {
                ui.label(RichText::new(subtitle).small().color(p.text_muted));
            }
        });
        ui.with_layout(Layout::right_to_left(Align::Min), trailing);
    });
    ui.add_space(SPACE_SM);
}

/// An upper-case label inside a card, heading a group of fields.
pub fn section_label(ui: &mut Ui, label: &str) {
    let p = Palette::of(ui);
    ui.add_space(SPACE_XS);
    ui.label(
        RichText::new(label.to_uppercase())
            .text_style(caption_style())
            .color(p.text_faint),
    );
}

/// A thin rule between groups inside a card.
pub fn divider(ui: &mut Ui) {
    let p = Palette::of(ui);
    ui.add_space(SPACE_XS);
    let (rect, _) = ui.allocate_exact_size(egui::vec2(ui.available_width(), 1.0), Sense::hover());
    ui.painter().rect_filled(rect, Rounding::ZERO, p.border);
    ui.add_space(SPACE_XS);
}

/// The left side of a form row: the field's name, its machine key in small
/// monospace underneath, and a help marker when `help` is given.  Use it as
/// the first column of a two-column [`egui::Grid`].
pub fn field_label(ui: &mut Ui, label: &str, key: Option<&str>, help: Option<&str>) {
    let p = Palette::of(ui);
    ui.vertical(|ui| {
        ui.spacing_mut().item_spacing.y = 1.0;
        ui.horizontal(|ui| {
            ui.spacing_mut().item_spacing.x = SPACE_XS + 2.0;
            ui.label(RichText::new(label).color(p.text));
            if let Some(help) = help {
                help_marker(ui, help);
            }
        });
        if let Some(key) = key {
            ui.label(RichText::new(key).monospace().size(11.5).color(p.text_faint));
        }
    });
}

/// A small circled "i" that shows `text` on hover.
pub fn help_marker(ui: &mut Ui, text: &str) -> Response {
    let p = Palette::of(ui);
    let (rect, response) = ui.allocate_exact_size(egui::vec2(14.0, 14.0), Sense::hover());
    if ui.is_rect_visible(rect) {
        let color = if response.hovered() { p.accent } else { p.text_faint };
        ui.painter()
            .circle_stroke(rect.center(), 6.0, stroke(1.0, color));
        ui.painter().text(
            rect.center() + egui::vec2(0.0, 0.5),
            Align2::CENTER_CENTER,
            "i",
            FontId::proportional(9.5),
            color,
        );
    }
    response.on_hover_text(text)
}

/// Text under a field: an error in the danger tone, or a muted hint.
pub fn field_message(ui: &mut Ui, tone: Tone, text: &str) {
    let p = Palette::of(ui);
    let color = match tone {
        Tone::Neutral => p.text_faint,
        other => other.color(&p),
    };
    ui.label(RichText::new(text).small().color(color));
}

/// A pill-shaped label.  Use it for short states ("unsaved", "speculative")
/// rather than for anything the user acts on.
pub fn badge(ui: &mut Ui, text: &str, tone: Tone) -> Response {
    let p = Palette::of(ui);
    let (fill, outline, color) = match tone {
        Tone::Neutral => (p.surface_alt, p.border, p.text_muted),
        other => {
            let color = other.color(&p);
            let fill = if ui.visuals().dark_mode {
                color.gamma_multiply(0.16)
            } else {
                color.gamma_multiply(0.12)
            };
            (fill, color.gamma_multiply(0.45), color)
        }
    };
    Frame::none()
        .fill(fill)
        .stroke(stroke(1.0, outline))
        .rounding(Rounding::same(9.0))
        .inner_margin(Margin::symmetric(7.0, 1.0))
        .show(ui, |ui| {
            ui.label(RichText::new(text).size(11.5).color(color));
        })
        .response
}

/// A centred message for a page with nothing on it yet.
pub fn empty_state(ui: &mut Ui, title: &str, body: &str) {
    let p = Palette::of(ui);
    ui.vertical_centered(|ui| {
        ui.add_space(SPACE_XL);
        ui.label(RichText::new(title).text_style(title_style()).color(p.text));
        ui.label(RichText::new(body).color(p.text_muted));
        ui.add_space(SPACE_XL);
    });
}

/// A keyboard shortcut drawn as a key cap.
pub fn kbd(ui: &mut Ui, keys: &str) -> Response {
    let p = Palette::of(ui);
    Frame::none()
        .fill(p.surface_alt)
        .stroke(stroke(1.0, p.border_strong))
        .rounding(Rounding::same(4.0))
        .inner_margin(Margin::symmetric(5.0, 0.0))
        .show(ui, |ui| {
            ui.label(RichText::new(keys).monospace().size(11.0).color(p.text_muted));
        })
        .response
}

// ---------------------------------------------------------------------------
// Controls
// ---------------------------------------------------------------------------

/// The one main action of a view (Save, Create).  Give each view at most one.
pub fn primary_button(ui: &mut Ui, text: &str) -> Response {
    let p = Palette::of(ui);
    let button = egui::Button::new(RichText::new(text).color(p.on_accent).strong())
        .fill(p.accent)
        .stroke(Stroke::NONE);
    let response = ui.add(button);
    response.on_hover_cursor(egui::CursorIcon::PointingHand)
}

/// A button that destroys or discards something.
pub fn danger_button(ui: &mut Ui, text: &str) -> Response {
    let p = Palette::of(ui);
    let button = egui::Button::new(RichText::new(text).color(Color32::WHITE).strong())
        .fill(p.danger)
        .stroke(Stroke::NONE);
    ui.add(button)
}

/// A frameless button for low-emphasis actions next to content.
pub fn quiet_button(ui: &mut Ui, text: &str) -> Response {
    let p = Palette::of(ui);
    ui.add(egui::Button::new(RichText::new(text).color(p.accent)).frame(false))
        .on_hover_cursor(egui::CursorIcon::PointingHand)
}

/// A square icon button with a tooltip, for app-bar actions.
pub fn icon_button(ui: &mut Ui, icon: &str, tooltip: &str) -> Response {
    ui.add(egui::Button::new(RichText::new(icon).size(15.0)).min_size(egui::vec2(32.0, 28.0)))
        .on_hover_text(tooltip)
}

/// An on/off switch.  Prefer it to a checkbox for settings that take effect
/// at once.
pub fn toggle(ui: &mut Ui, on: &mut bool) -> Response {
    let p = Palette::of(ui);
    let size = egui::vec2(32.0, 18.0);
    let (rect, mut response) = ui.allocate_exact_size(size, Sense::click());
    if response.clicked() {
        *on = !*on;
        response.mark_changed();
    }
    response.widget_info(|| WidgetInfo::selected(WidgetType::Checkbox, ui.is_enabled(), *on, ""));
    if ui.is_rect_visible(rect) {
        let t = ui.ctx().animate_bool(response.id, *on);
        let radius = rect.height() / 2.0;
        let mut track = if *on { p.accent } else { p.border_strong };
        if !ui.is_enabled() {
            track = track.gamma_multiply(0.5);
        }
        ui.painter().rect_filled(rect, Rounding::same(radius), track);
        let x = egui::lerp((rect.left() + radius)..=(rect.right() - radius), t);
        ui.painter()
            .circle_filled(egui::pos2(x, rect.center().y), radius - 3.0, Color32::WHITE);
    }
    response.on_hover_cursor(egui::CursorIcon::PointingHand)
}

/// A switch with its label; clicking the label flips it too.  `trailing`
/// draws after the label (a badge, say).
pub fn toggle_row(
    ui: &mut Ui,
    on: &mut bool,
    label: &str,
    trailing: impl FnOnce(&mut Ui),
) -> Response {
    let p = Palette::of(ui);
    ui.horizontal(|ui| {
        let mut response = toggle(ui, on);
        let text_color = if *on { p.text } else { p.text_muted };
        // Label and trailing badge wrap together inside the remaining width,
        // so a long label never pushes the badge into a neighbouring column.
        let label_clicked = ui
            .horizontal_wrapped(|ui| {
                let clicked = ui
                    .add(
                        egui::Label::new(RichText::new(label).color(text_color))
                            .wrap()
                            .sense(Sense::click()),
                    )
                    .clicked();
                trailing(ui);
                clicked
            })
            .inner;
        if label_clicked {
            *on = !*on;
            response.mark_changed();
        }
        response
    })
    .inner
}

/// A row of mutually exclusive options drawn as one joined control.
pub fn segmented<T: PartialEq + Clone>(ui: &mut Ui, value: &mut T, options: &[(T, &str)]) -> bool {
    let p = Palette::of(ui);
    let mut changed = false;
    Frame::none()
        .fill(p.surface_alt)
        .stroke(stroke(1.0, p.border))
        .rounding(Rounding::same(RADIUS_MD))
        .inner_margin(Margin::same(2.0))
        .show(ui, |ui| {
            ui.horizontal(|ui| {
                ui.spacing_mut().item_spacing.x = 2.0;
                for (option, label) in options {
                    let selected = *value == *option;
                    let text = if selected {
                        RichText::new(*label).color(p.on_accent)
                    } else {
                        RichText::new(*label).color(p.text_muted)
                    };
                    let button = egui::Button::new(text)
                        .fill(if selected { p.accent } else { Color32::TRANSPARENT })
                        .stroke(Stroke::NONE)
                        .rounding(Rounding::same(RADIUS_MD - 2.0));
                    if ui.add(button).clicked() && !selected {
                        *value = option.clone();
                        changed = true;
                    }
                }
            });
        });
    changed
}

/// A search field with a clear button.  Returns true when the text changed.
pub fn search_box(ui: &mut Ui, text: &mut String, hint: &str, width: f32) -> bool {
    let p = Palette::of(ui);
    let mut changed = false;
    Frame::none()
        .fill(p.surface_alt)
        .stroke(stroke(1.0, p.border))
        .rounding(Rounding::same(RADIUS_MD))
        .inner_margin(Margin::symmetric(SPACE_SM, 3.0))
        .show(ui, |ui| {
            ui.set_width(width);
            ui.horizontal(|ui| {
                ui.label(RichText::new("🔍").size(12.0).color(p.text_faint));
                let clear_width = if text.is_empty() { 0.0 } else { 22.0 };
                let edit = egui::TextEdit::singleline(text)
                    .hint_text(hint)
                    .frame(false)
                    .desired_width(ui.available_width() - clear_width);
                changed |= ui.add(edit).changed();
                if !text.is_empty()
                    && ui
                        .add(egui::Button::new(RichText::new("✖").size(11.0)).frame(false))
                        .on_hover_text("Clear")
                        .clicked()
                {
                    text.clear();
                    changed = true;
                }
            });
        });
    changed
}

/// Light / Dark / System, as a segmented control.  The choice applies at
/// once.
pub fn theme_picker(ui: &mut Ui) {
    let mut preference = ui.ctx().options(|options| options.theme_preference);
    if segmented(
        ui,
        &mut preference,
        &[
            (egui::ThemePreference::System, "System"),
            (egui::ThemePreference::Light, "Light"),
            (egui::ThemePreference::Dark, "Dark"),
        ],
    ) {
        ui.ctx().set_theme(preference);
    }
}

// ---------------------------------------------------------------------------
// Dialogs
// ---------------------------------------------------------------------------

/// A centred dialog over a dimmed window.  The rest of the window ignores
/// the mouse while it is open.  Returns what `contents` returns.
pub fn modal<R>(
    ctx: &Context,
    id: impl Hash,
    title: &str,
    subtitle: Option<&str>,
    contents: impl FnOnce(&mut Ui) -> R,
) -> R {
    let id = Id::new(id);
    let screen = ctx.screen_rect();
    egui::Area::new(id.with("scrim"))
        .order(Order::PanelResizeLine)
        .fixed_pos(screen.min)
        .show(ctx, |ui| {
            ui.painter()
                .rect_filled(screen, Rounding::ZERO, Color32::from_black_alpha(150));
            ui.allocate_rect(screen, Sense::click_and_drag());
        });

    let p = Palette::of_ctx(ctx);
    egui::Window::new(title)
        .id(id)
        .title_bar(false)
        .collapsible(false)
        .resizable(false)
        .anchor(Align2::CENTER_CENTER, [0.0, -40.0])
        .frame(
            Frame::window(&ctx.style())
                .fill(p.surface)
                .rounding(Rounding::same(RADIUS_LG + 2.0))
                .inner_margin(Margin::same(SPACE_XL)),
        )
        .show(ctx, |ui| {
            ui.set_width(420.0);
            card_header(ui, title, subtitle, |_| {});
            contents(ui)
        })
        .and_then(|response| response.inner)
        .expect("a modal window is always shown")
}

/// The button row at the bottom of a dialog, laid out right to left: add
/// the primary action first.
pub fn dialog_buttons(ui: &mut Ui, contents: impl FnOnce(&mut Ui)) {
    ui.add_space(SPACE_LG);
    let size = egui::vec2(ui.available_width(), ui.spacing().interact_size.y);
    ui.allocate_ui_with_layout(size, Layout::right_to_left(Align::Center), contents);
}

/// A popup anchored under `button` that stays open until the user clicks
/// outside it.  Returns the popup's inner value while it is open.
pub fn popover<R>(
    ui: &mut Ui,
    button: &Response,
    width: f32,
    contents: impl FnOnce(&mut Ui) -> R,
) -> Option<R> {
    let popup_id = button.id.with("popover");
    if button.clicked() {
        ui.memory_mut(|memory| memory.toggle_popup(popup_id));
    }
    egui::popup_below_widget(
        ui,
        popup_id,
        button,
        egui::PopupCloseBehavior::CloseOnClickOutside,
        |ui| {
            ui.set_width(width);
            Frame::none()
                .inner_margin(Margin::same(SPACE_SM))
                .show(ui, contents)
                .inner
        },
    )
}
