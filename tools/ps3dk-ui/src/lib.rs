//! The shared look of the PS3DK desktop tools.
//!
//! Every egui tool calls [`install`] once at start-up and builds its window
//! from the widgets in [`widgets`], so the SFO editor, the profiler and any
//! later tool share one palette, one type scale and one set of controls.
//! `docs/design/gui-theme.md` describes the layout conventions.

use egui::epaint::Shadow;
use egui::{Color32, Context, FontFamily, FontId, Margin, Rounding, Stroke, Style, TextStyle, Ui};

pub mod widgets;

pub use widgets::*;

/// Spacing steps.  Use these instead of literal pixel values.
pub const SPACE_XS: f32 = 4.0;
pub const SPACE_SM: f32 = 8.0;
pub const SPACE_MD: f32 = 12.0;
pub const SPACE_LG: f32 = 16.0;
pub const SPACE_XL: f32 = 24.0;

/// Corner radii: controls, buttons and inputs use `RADIUS_MD`; cards,
/// dialogs and popups use `RADIUS_LG`.
pub const RADIUS_SM: f32 = 4.0;
pub const RADIUS_MD: f32 = 6.0;
pub const RADIUS_LG: f32 = 10.0;

/// Width of the left navigation panel.
pub const SIDEBAR_WIDTH: f32 = 216.0;

/// Text style for card and dialog titles (16 px).
pub fn title_style() -> TextStyle {
    TextStyle::Name("title".into())
}

/// Text style for small upper-case section labels (11.5 px).
pub fn caption_style() -> TextStyle {
    TextStyle::Name("caption".into())
}

/// A line of `width` points; spares callers the float-literal fallback
/// that `Stroke::new`'s generic width argument triggers.
pub fn stroke(width: f32, color: Color32) -> Stroke {
    Stroke::new(width, color)
}

/// The colour tokens.  Widgets read colours from here, never from literals,
/// so both themes stay in step.
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct Palette {
    /// Behind the page content.
    pub bg: Color32,
    /// App bar, sidebar and status bar.
    pub panel: Color32,
    /// Cards and dialogs.
    pub surface: Color32,
    /// Inputs, table stripes and other recessed areas.
    pub surface_alt: Color32,
    /// Hovered rows and buttons.
    pub hover: Color32,
    pub border: Color32,
    pub border_strong: Color32,
    pub text: Color32,
    pub text_muted: Color32,
    pub text_faint: Color32,
    pub accent: Color32,
    pub accent_hover: Color32,
    /// Text drawn on an `accent` fill.
    pub on_accent: Color32,
    /// Selected rows and active-state fills.
    pub accent_soft: Color32,
    pub success: Color32,
    pub warning: Color32,
    pub danger: Color32,
}

impl Palette {
    pub const DARK: Palette = Palette {
        bg: Color32::from_rgb(0x0e, 0x11, 0x16),
        panel: Color32::from_rgb(0x13, 0x17, 0x1e),
        surface: Color32::from_rgb(0x19, 0x1e, 0x27),
        surface_alt: Color32::from_rgb(0x10, 0x14, 0x1a),
        hover: Color32::from_rgb(0x23, 0x2a, 0x37),
        border: Color32::from_rgb(0x26, 0x2c, 0x38),
        border_strong: Color32::from_rgb(0x3a, 0x43, 0x54),
        text: Color32::from_rgb(0xe7, 0xea, 0xf0),
        text_muted: Color32::from_rgb(0xa0, 0xa8, 0xb8),
        text_faint: Color32::from_rgb(0x6c, 0x74, 0x86),
        accent: Color32::from_rgb(0x4f, 0x8c, 0xff),
        accent_hover: Color32::from_rgb(0x6b, 0x9f, 0xff),
        on_accent: Color32::WHITE,
        accent_soft: Color32::from_rgb(0x1b, 0x2a, 0x45),
        success: Color32::from_rgb(0x3d, 0xbe, 0x84),
        warning: Color32::from_rgb(0xe5, 0xa9, 0x3c),
        danger: Color32::from_rgb(0xef, 0x5b, 0x66),
    };

    pub const LIGHT: Palette = Palette {
        bg: Color32::from_rgb(0xf4, 0xf6, 0xf9),
        panel: Color32::from_rgb(0xff, 0xff, 0xff),
        surface: Color32::from_rgb(0xff, 0xff, 0xff),
        surface_alt: Color32::from_rgb(0xf1, 0xf3, 0xf7),
        hover: Color32::from_rgb(0xe9, 0xed, 0xf3),
        border: Color32::from_rgb(0xdd, 0xe2, 0xea),
        border_strong: Color32::from_rgb(0xbf, 0xc7, 0xd3),
        text: Color32::from_rgb(0x17, 0x1b, 0x22),
        text_muted: Color32::from_rgb(0x53, 0x5c, 0x6e),
        text_faint: Color32::from_rgb(0x8a, 0x93, 0xa5),
        accent: Color32::from_rgb(0x2f, 0x6b, 0xe0),
        accent_hover: Color32::from_rgb(0x24, 0x59, 0xc2),
        on_accent: Color32::WHITE,
        accent_soft: Color32::from_rgb(0xe3, 0xec, 0xfd),
        success: Color32::from_rgb(0x1f, 0x94, 0x61),
        warning: Color32::from_rgb(0xb7, 0x79, 0x1f),
        danger: Color32::from_rgb(0xd1, 0x38, 0x4a),
    };

    /// The palette for the theme `ui` is drawn with.
    pub fn of(ui: &Ui) -> Palette {
        Self::for_dark_mode(ui.visuals().dark_mode)
    }

    /// The palette for the context's current theme.
    pub fn of_ctx(ctx: &Context) -> Palette {
        Self::for_dark_mode(ctx.style().visuals.dark_mode)
    }

    pub fn for_dark_mode(dark: bool) -> Palette {
        if dark {
            Self::DARK
        } else {
            Self::LIGHT
        }
    }
}

/// The meaning a coloured element carries.  Status messages, badges and
/// buttons pick a tone rather than a colour.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub enum Tone {
    #[default]
    Neutral,
    Accent,
    Success,
    Warning,
    Danger,
}

impl Tone {
    pub fn color(self, palette: &Palette) -> Color32 {
        match self {
            Tone::Neutral => palette.text_muted,
            Tone::Accent => palette.accent,
            Tone::Success => palette.success,
            Tone::Warning => palette.warning,
            Tone::Danger => palette.danger,
        }
    }
}

/// Install the PS3DK styles for both the dark and the light theme.  egui
/// follows the system theme by default; [`widgets::theme_picker`] lets the
/// user override it.
pub fn install(ctx: &Context) {
    ctx.set_style_of(egui::Theme::Dark, style_for(&Palette::DARK, true));
    ctx.set_style_of(egui::Theme::Light, style_for(&Palette::LIGHT, false));
}

/// The full egui style for one palette.
pub fn style_for(palette: &Palette, dark: bool) -> Style {
    let p = palette;
    let mut style = Style::default();

    style.text_styles = [
        (TextStyle::Small, FontId::new(12.0, FontFamily::Proportional)),
        (TextStyle::Body, FontId::new(14.0, FontFamily::Proportional)),
        (TextStyle::Button, FontId::new(14.0, FontFamily::Proportional)),
        (TextStyle::Monospace, FontId::new(13.0, FontFamily::Monospace)),
        (TextStyle::Heading, FontId::new(22.0, FontFamily::Proportional)),
        (title_style(), FontId::new(16.0, FontFamily::Proportional)),
        (caption_style(), FontId::new(11.5, FontFamily::Proportional)),
    ]
    .into();

    let spacing = &mut style.spacing;
    spacing.item_spacing = egui::vec2(SPACE_SM, SPACE_SM);
    spacing.button_padding = egui::vec2(12.0, 5.0);
    spacing.interact_size.y = 28.0;
    spacing.window_margin = Margin::same(SPACE_XL - 4.0);
    spacing.menu_margin = Margin::same(SPACE_SM);
    spacing.indent = SPACE_LG;
    spacing.combo_height = 320.0;

    let mut visuals = if dark {
        egui::Visuals::dark()
    } else {
        egui::Visuals::light()
    };
    visuals.panel_fill = p.bg;
    visuals.window_fill = p.surface;
    visuals.extreme_bg_color = p.surface_alt;
    visuals.faint_bg_color = p.surface_alt;
    visuals.code_bg_color = p.surface_alt;
    visuals.hyperlink_color = p.accent;
    visuals.warn_fg_color = p.warning;
    visuals.error_fg_color = p.danger;
    visuals.window_rounding = Rounding::same(RADIUS_LG);
    visuals.menu_rounding = Rounding::same(RADIUS_MD);
    visuals.window_stroke = stroke(1.0, p.border);
    let shadow_alpha = if dark { 110 } else { 40 };
    visuals.window_shadow = Shadow {
        offset: egui::vec2(0.0, 8.0),
        blur: 28.0,
        spread: 0.0,
        color: Color32::from_black_alpha(shadow_alpha),
    };
    visuals.popup_shadow = Shadow {
        offset: egui::vec2(0.0, 4.0),
        blur: 14.0,
        spread: 0.0,
        color: Color32::from_black_alpha(shadow_alpha),
    };
    visuals.selection.bg_fill = if dark {
        p.accent.gamma_multiply(0.45)
    } else {
        p.accent.gamma_multiply(0.3)
    };
    visuals.selection.stroke = stroke(1.0, p.accent);
    visuals.collapsing_header_frame = false;
    visuals.striped = false;

    let rounding = Rounding::same(RADIUS_MD);
    let w = &mut visuals.widgets;
    w.noninteractive.bg_fill = p.surface;
    w.noninteractive.weak_bg_fill = p.surface;
    w.noninteractive.bg_stroke = stroke(1.0, p.border);
    w.noninteractive.fg_stroke = stroke(1.0, p.text);
    w.noninteractive.rounding = rounding;

    w.inactive.bg_fill = p.surface_alt;
    w.inactive.weak_bg_fill = p.surface_alt;
    w.inactive.bg_stroke = stroke(1.0, p.border);
    w.inactive.fg_stroke = stroke(1.0, p.text);
    w.inactive.rounding = rounding;
    w.inactive.expansion = 0.0;

    w.hovered.bg_fill = p.hover;
    w.hovered.weak_bg_fill = p.hover;
    w.hovered.bg_stroke = stroke(1.0, p.border_strong);
    w.hovered.fg_stroke = stroke(1.5, p.text);
    w.hovered.rounding = rounding;
    w.hovered.expansion = 0.0;

    w.active.bg_fill = p.accent_soft;
    w.active.weak_bg_fill = p.accent_soft;
    w.active.bg_stroke = stroke(1.0, p.accent);
    w.active.fg_stroke = stroke(1.5, p.text);
    w.active.rounding = rounding;
    w.active.expansion = 0.0;

    w.open.bg_fill = p.hover;
    w.open.weak_bg_fill = p.hover;
    w.open.bg_stroke = stroke(1.0, p.border_strong);
    w.open.fg_stroke = stroke(1.0, p.text);
    w.open.rounding = rounding;

    style.visuals = visuals;
    style
}
