//! The Hot paths page's "Functions (automatic)" source, drawn from the [`crate::funcpaths`] view model: thread and
//! interval selectors, every status and counter as visible text, an indented call tree (calls, inclusive, self, %
//! of the selection's wall elapsed, row statuses) and the LOST row apart from the tree.

use eframe::egui;
use ps3dk_prof::func_paths::Selection;
use ps3dk_prof::Analysis;
use ps3dk_ui as ui;

use crate::funcpaths::{FRow, FuncSlot, FuncView};
use crate::hotpaths_ui::HotState;

/// Indentation per tree level, in points (capped so deep trees stay on the page).
const INDENT: f32 = 12.0;
const MAX_INDENT_LEVELS: usize = 40;

fn pct(p: Option<f64>) -> String {
    p.map_or("n/a".to_string(), |p| format!("{p:.1}%"))
}

/// The view the page draws for the state (cached in the Document's slot).
pub fn current(a: &Analysis, st: &HotState, slot: &FuncSlot) -> std::sync::Arc<FuncView> {
    let sel = st
        .func_interval
        .map_or(Selection::Whole, Selection::Interval);
    let symbols = match &st.symbols {
        Some((name, Ok(s))) => Some((name.as_str(), s.as_ref())),
        _ => None,
    };
    slot.view(a, st.func_thread, sel, symbols, st.symbols_gen)
}

fn row_cells(g: &mut egui::Ui, r: &FRow) {
    g.horizontal(|h| {
        h.add_space(INDENT * r.depth.min(MAX_INDENT_LEVELS) as f32);
        let text = if r.lost {
            egui::RichText::new(&r.label)
                .strong()
                .color(h.visuals().warn_fg_color)
        } else if r.present {
            egui::RichText::new(&r.label).monospace()
        } else {
            egui::RichText::new(&r.label).monospace().weak()
        };
        h.label(text).on_hover_text(&r.path);
    });
    g.label(&r.calls);
    g.label(&r.incl);
    g.label(&r.self_time);
    let frac = r.incl_pct.map_or(0.0, |p| (p / 100.0) as f32);
    g.add(
        egui::ProgressBar::new(frac.clamp(0.0, 1.0))
            .desired_width(90.0)
            .text(pct(r.incl_pct)),
    );
    g.label(pct(r.self_pct));
    g.label(&r.flags);
    g.end_row();
}

/// Draws the Functions source.  The ELF button opens a file dialog; everything else is plain state changes.
pub fn draw(p: &mut egui::Ui, a: &Analysis, st: &mut HotState, slot: &FuncSlot) {
    let v = current(a, st, slot);
    if !v.capability {
        ui::field_message(p, ui::Tone::Neutral, &v.status);
        return;
    }
    let tone = if v.status.contains("RECORDED") {
        ui::Tone::Neutral
    } else {
        ui::Tone::Warning
    };
    ui::field_message(p, tone, &v.status);
    p.horizontal_wrapped(|h| {
        h.label(egui::RichText::new("Symbols").strong());
        match &st.symbols {
            None => {
                h.label("none (raw fn addresses)");
            }
            Some((name, Ok(_))) => {
                h.label(name.as_str());
            }
            Some((name, Err(why))) => {
                h.label(
                    egui::RichText::new(format!("{name} REFUSED: {why}"))
                        .color(h.visuals().warn_fg_color),
                );
            }
        }
        if h.button("Load ELF symbols...").clicked() {
            if let Some(path) = rfd::FileDialog::new()
                .add_filter("PPU ELF", &["elf"])
                .add_filter("All files", &["*"])
                .pick_file()
            {
                match std::fs::read(&path) {
                    Ok(b) => crate::hotpaths_ui::set_symbols(st, path.display().to_string(), &b),
                    Err(e) => {
                        st.symbols = Some((path.display().to_string(), Err(e.to_string())));
                        st.symbols_gen += 1;
                    }
                }
            }
        }
        if st.symbols.is_some() && h.button("Clear").clicked() {
            crate::hotpaths_ui::clear_symbols(st);
        }
    });
    for l in &v.lines {
        p.add(egui::Label::new(l).wrap());
    }
    p.horizontal_wrapped(|h| {
        h.label(egui::RichText::new("Thread").strong());
        for (inc, label) in &v.threads {
            if h.selectable_label(v.thread == Some(*inc), label).clicked() && v.thread != Some(*inc)
            {
                st.func_thread = Some(*inc);
                st.func_interval = None;
            }
        }
    });
    if !v.choices.is_empty() {
        let shown = v
            .choices
            .iter()
            .find(|c| {
                c.sel
                    == st
                        .func_interval
                        .map_or(Selection::Whole, Selection::Interval)
            })
            .map_or("Whole capture".to_string(), |c| c.label.clone());
        p.horizontal(|h| {
            h.label(egui::RichText::new("Interval").strong());
            egui::ComboBox::from_id_salt("func-interval")
                .selected_text(shown)
                .width(260.0)
                .show_ui(h, |c| {
                    for ch in &v.choices {
                        let want = match ch.sel {
                            Selection::Whole => None,
                            Selection::Interval(n) => Some(n),
                        };
                        c.selectable_value(&mut st.func_interval, want, &ch.label);
                    }
                });
        });
    }
    for l in &v.thread_lines {
        p.add(egui::Label::new(l).wrap());
    }
    for l in &v.selection_lines {
        p.add(egui::Label::new(l).wrap());
    }
    if let Some(n) = &v.notice {
        ui::field_message(p, ui::Tone::Warning, n);
        return;
    }
    p.add_space(ui::SPACE_SM);
    egui::ScrollArea::vertical().id_salt("func-tree").show(p, |p| {
        egui::Grid::new("func-tree-grid").num_columns(7).striped(true).spacing([ui::SPACE_LG, ui::SPACE_XS]).show(p, |g| {
            for h in ["Function (caller chain indented)", "Calls", "Inclusive (wall)", "Self (wall)", "Incl %", "Self %", "Status"] {
                g.label(egui::RichText::new(h).strong());
            }
            g.end_row();
            for r in &v.rows {
                row_cells(g, r);
            }
        });
        if let Some(m) = &v.more {
            p.label(m);
        }
        p.add_space(ui::SPACE_MD);
        match &v.lost {
            Some(l) => {
                ui::field_message(
                    p,
                    ui::Tone::Warning,
                    "LOST: calls and time whose call path is unknown (node table full or probe refused). Never a child path.",
                );
                egui::Grid::new("func-lost-grid").num_columns(7).spacing([ui::SPACE_LG, ui::SPACE_XS]).show(p, |g| {
                    row_cells(g, l);
                });
            }
            None => {
                p.label("LOST: no row (no entry lost its call path in this selection).");
            }
        }
    });
}
