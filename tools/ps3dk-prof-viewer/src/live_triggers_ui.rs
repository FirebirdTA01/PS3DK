//! The live view's capture-trigger row: a window name, Begin, End, a frame count and "Capture N frames".  Each
//! button's enabled state and hover reason come from the tested pure [`crate::live_triggers::buttons`]; this file
//! only draws them and returns the command that was clicked.

use eframe::egui;
use ps3dk_ui as ui;

use crate::live::{Command, MAX_WINDOW_FRAMES};
use crate::live_triggers::{buttons, Form, Triggers};

pub const TIP: &str = "Asks the running app to open or close a capture window at its next frame boundary. The app \
answers each request; the window appears in the streamed capture as it closes.";

fn button(row: &mut egui::Ui, label: &str, state: &Result<Command, String>, tip: &str) -> Option<Command> {
    match state {
        Ok(c) => ui::quiet_button(row, label).on_hover_text(tip).clicked().then(|| c.clone()),
        Err(why) => {
            let _ = row.add_enabled(false, egui::Button::new(label)).on_disabled_hover_text(why.as_str());
            None
        }
    }
}

/// Draws the row; returns the command the user clicked, if any.
pub fn row(strip: &mut egui::Ui, t: &Triggers, form: &mut Form) -> Option<Command> {
    let b = buttons(t, form);
    let mut clicked = None;
    strip.horizontal_wrapped(|row| {
        row.label("Capture triggers").on_hover_text(TIP);
        row.add(egui::TextEdit::singleline(&mut form.name).desired_width(140.0).hint_text("window name"))
            .on_hover_text("The window's name in the capture (1..63 bytes)");
        clicked = clicked.take().or(button(row, "Begin", &b.begin, "Open a window from the next frame until End"));
        clicked = clicked.take().or(button(row, "End", &b.end, "Close the window this viewer opened, after the current frame"));
        row.add_space(ui::SPACE_LG);
        row.add(egui::DragValue::new(&mut form.frames).range(1..=MAX_WINDOW_FRAMES).suffix(" frames"));
        let label = format!("Capture {} frames", form.frames);
        clicked = clicked.take().or(button(row, &label, &b.frames, "Open a window that closes itself after that many frames"));
    });
    if let Some(why) = t.blocked().filter(|_| !matches!(t.status, crate::live_triggers::Status::Pending { .. })) {
        ui::field_message(strip, ui::Tone::Neutral, &format!("Capture triggers unavailable: {why}"));
    }
    if let Some((bad, line)) = t.status_line() {
        ui::field_message(strip, if bad { ui::Tone::Warning } else { ui::Tone::Neutral }, &line);
    }
    clicked
}
