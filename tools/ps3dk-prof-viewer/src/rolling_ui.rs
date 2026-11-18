//! The "Rolling (retained)" page: what a rolling capture's retained window
//! holds, as the reader projected it. Nothing here is a whole-capture claim.

use crate::rolling_doc::{verdict_counts, store_rows, RollingDoc, NOTE};
use crate::app::Page;
use eframe::egui;
use ps3dk_ui as ui;

pub const TITLE: &str = "Rolling (retained)";

/// The pages that need a complete capture: listed but shown as an empty state for a rolling file.
pub const CAPTURE_PAGES: &[(Page, &str)] = &[
    (Page::Overview, "Overview"),
    (Page::Timeline, "Timeline"),
    (Page::Frames, "Frames"),
    (Page::Zones, "Zones"),
    (Page::HotPaths, "Hot paths"),
    (Page::Waits, "Waits"),
    (Page::Fifo, "FIFO wrap waits"),
    (Page::Automatic, "Automatic"),
    (Page::Passes, "Passes"),
    (Page::RsxFrames, "RSX frames"),
];

/// The empty state for a page that needs a complete capture.
pub const NEEDS_CAPTURE_TITLE: &str = "Needs a complete capture";
pub const NEEDS_CAPTURE_BODY: &str = "This file is a rolling capture: only its newest segments are retained and the \
reader builds no whole-capture analysis from them. This page is not shown for it; see Rolling (retained) for what \
the retained window holds.";

fn kv(p: &mut egui::Ui, id: &str, rows: &[(String, String)]) {
    egui::Grid::new(id)
        .num_columns(2)
        .striped(true)
        .spacing([ui::SPACE_LG, ui::SPACE_XS])
        .show(p, |g| {
            for (k, v) in rows {
                g.label(egui::RichText::new(k).strong());
                g.add(egui::Label::new(v).wrap());
                g.end_row();
            }
        });
}

fn opt<T: ToString>(v: &Option<T>) -> String {
    v.as_ref().map(ToString::to_string).unwrap_or_else(|| "unavailable".into())
}

pub fn page(p: &mut egui::Ui, d: &RollingDoc) {
    let s = &d.summary;
    ui::page_header(p, TITLE, "A bounded rolling capture: the retained window only, as the reader validated it.");
    ui::field_message(p, ui::Tone::Warning, NOTE);
    egui::ScrollArea::vertical().show(p, |p| {
        p.add_space(ui::SPACE_LG);
        ui::section_label(p, "STORE (RETAINED WINDOW)");
        kv(p, "rolling-store", &store_rows(d));

        p.add_space(ui::SPACE_LG);
        ui::section_label(p, "PPU ZONES (RETAINED ONLY)");
        if s.ppu.zones.is_empty() {
            ui::field_message(p, ui::Tone::Neutral, "No PPU zones in the retained segments.");
        } else {
            egui::Grid::new("rolling-zones").striped(true).show(p, |g| {
                for h in ["Zone", "Completed", "Left-censored", "Open", "Total ticks (retained)"] {
                    g.strong(h);
                }
                g.end_row();
                for z in &s.ppu.zones {
                    g.label(&z.name);
                    g.label(z.completed.to_string());
                    g.label(z.left_censored.to_string());
                    g.label(z.open.to_string());
                    g.label(opt(&z.total_ticks));
                    g.end_row();
                }
            });
        }
        if !s.ppu.lanes.is_empty() {
            p.add_space(ui::SPACE_SM);
            egui::Grid::new("rolling-lanes").striped(true).show(p, |g| {
                for h in ["Lane", "Prefix records (claim)", "Retained records", "Retained gap sum", "Final entry"] {
                    g.strong(h);
                }
                g.end_row();
                for l in &s.ppu.lanes {
                    g.label(l.id.to_string());
                    g.label(l.prefix_written.to_string());
                    g.label(l.retained_written.to_string());
                    g.label(l.retained_gap_sum.to_string());
                    g.label(if l.final_present { "present" } else { "absent" });
                    g.end_row();
                }
            });
        }
        for why in &s.ppu.incomplete {
            ui::field_message(p, ui::Tone::Warning, why);
        }

        if let Some(spu) = &s.spu {
            p.add_space(ui::SPACE_LG);
            ui::section_label(p, "SPU (RETAINED, CAUSAL SUPPORT ONLY)");
            p.add(egui::Label::new(&spu.note).wrap());
            if let Some(r) = &spu.refusal {
                ui::field_message(p, ui::Tone::Warning, &format!("Causal placement refused: {r}"));
            }
            if spu.sources.is_empty() {
                ui::field_message(p, ui::Tone::Neutral, "No SPU sources in the retained segments.");
            } else {
                egui::Grid::new("rolling-spu").striped(true).show(p, |g| {
                    for h in ["Source", "Retained events", "Prefix events (claim)", "Retained lost", "Pairs", "Structure"] {
                        g.strong(h);
                    }
                    g.end_row();
                    for x in &spu.sources {
                        g.label(&x.source);
                        g.label(x.retained_events.to_string());
                        g.label(x.prefix_events.to_string());
                        g.label(x.retained_lost.to_string());
                        g.label(x.pairs.to_string());
                        g.label(&x.structure);
                        g.end_row();
                    }
                });
            }
        }

        p.add_space(ui::SPACE_LG);
        ui::section_label(p, "AUTOMATIC RSX (RETAINED FRAMES ONLY)");
        match &s.auto {
            None => ui::field_message(p, ui::Tone::Neutral, "The retained segments carry no AUTO projection."),
            Some(a) => {
                ui::field_message(p, ui::Tone::Neutral, &format!("Clock map: {}", a.map_line));
                let tally = verdict_counts(a)
                    .iter()
                    .map(|(l, n)| format!("{n} {l}"))
                    .collect::<Vec<_>>()
                    .join(", ");
                p.add(egui::Label::new(format!("{} retained frames: {}", a.frames.len(), if tally.is_empty() { "none".into() } else { tally })).wrap());
                egui::Grid::new("rolling-auto").striped(true).show(p, |g| {
                    for h in ["Owner", "Frame", "Verdict", "RSX busy (raw)", "RSX busy (PPU lo..hi)", "Unavailable", "Reason"] {
                        g.strong(h);
                    }
                    g.end_row();
                    for f in &a.frames {
                        g.label(f.owner.to_string());
                        g.label(f.frame.to_string());
                        g.label(f.verdict_label);
                        g.label(opt(&f.rsx_busy_raw));
                        g.label(f.rsx_busy_ppu.map(|(lo, hi)| format!("{lo}..{hi}")).unwrap_or_else(|| "unavailable".into()));
                        g.label(if f.unavailable.is_empty() { "-".to_string() } else { f.unavailable.join(", ") });
                        g.add(egui::Label::new(&f.reason).wrap());
                        g.end_row();
                    }
                });
            }
        }

        p.add_space(ui::SPACE_LG);
        ui::section_label(p, "FIFO (RETAINED ONLY)");
        match &s.fifo {
            None => ui::field_message(p, ui::Tone::Neutral, "The retained segments carry no FIFO projection."),
            Some(f) => {
                let rows = vec![
                    ("Final status in retained part".to_string(), if f.final_seen { "seen" } else { "not seen" }.to_string()),
                    ("Retained events".to_string(), f.retained_events.to_string()),
                    ("Retained accounting rows".to_string(), f.retained_accounting_rows.to_string()),
                    ("Known missing".to_string(), f.known_missing.to_string()),
                    ("Prefix events without tokens".to_string(), if f.prefix_unassociated { "yes" } else { "no" }.to_string()),
                    (
                        "Frames with dropped-prefix events".to_string(),
                        if f.boundary_frames.is_empty() {
                            "none".to_string()
                        } else {
                            f.boundary_frames
                                .iter()
                                .map(|(o, a, b)| format!("owner {o}: {a}..{b}"))
                                .collect::<Vec<_>>()
                                .join("; ")
                        },
                    ),
                ];
                kv(p, "rolling-fifo", &rows);
                if f.unavailable_window_rows!=0{
                    ui::field_message(p,ui::Tone::Neutral,&format!(
                        "Window bounds unavailable: {} retained row(s), {} cumulatively. The declarations were evicted; these rows provide no window-specific evidence.",
                        f.retained_unavailable_window_rows,f.unavailable_window_rows));
                }
            }
        }
    });
}
