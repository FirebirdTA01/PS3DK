//! ps3dk-prof-viewer: open a ps3dk-prof capture and inspect it.
//! Phase 1, increment 1: local files, Overview and Zones.

use eframe::egui;
use ps3dk_prof::ZoneStats;
use ps3dk_prof_viewer::model::{
    overview, sort_zones, zone_cell, zone_cell_exact, zone_key, zone_sites, zones, DocState,
    Document, TimeBase, VerdictTone, ZoneColumn, ZoneKey,
};
use ps3dk_ui as ui;
use std::path::PathBuf;

const APP_NAME: &str = "PS3DK Profiler";

#[derive(Clone, Copy, PartialEq, Eq)]
enum Page {
    Overview,
    Zones,
}

struct App {
    doc: Option<Document>,
    error: Option<String>,
    page: Page,
    zone_rows: Vec<ZoneStats>,
    sort: (ZoneColumn, bool),
    selected_zone: Option<ZoneKey>,
}

impl App {
    fn new(open: Option<PathBuf>) -> App {
        let mut app = App {
            doc: None,
            error: None,
            page: Page::Overview,
            zone_rows: Vec::new(),
            sort: (ZoneColumn::Total, true),
            selected_zone: None,
        };
        if let Some(p) = open {
            app.open(p);
        }
        app
    }

    fn open(&mut self, path: PathBuf) {
        match std::fs::read(&path) {
            Ok(bytes) => {
                let doc = Document::from_bytes(path, &bytes);
                self.zone_rows = doc.analysis().map(zones).unwrap_or_default();
                sort_zones(&mut self.zone_rows, self.sort.0, self.sort.1);
                self.selected_zone = None;
                self.doc = Some(doc);
                self.error = None;
            }
            Err(e) => self.error = Some(format!("{}: {e}", path.display())),
        }
    }

    fn pick_file(&mut self) {
        if let Some(p) = rfd::FileDialog::new()
            .add_filter("ps3dk-prof capture", &["prof"])
            .pick_file()
        {
            self.open(p);
        }
    }
}

fn tone(t: VerdictTone) -> ui::Tone {
    match t {
        VerdictTone::Complete => ui::Tone::Success,
        VerdictTone::Incomplete => ui::Tone::Warning,
        VerdictTone::Malformed => ui::Tone::Danger,
    }
}

fn verdict_word(t: VerdictTone) -> &'static str {
    match t {
        VerdictTone::Complete => "COMPLETE",
        VerdictTone::Incomplete => "INCOMPLETE",
        VerdictTone::Malformed => "MALFORMED",
    }
}

impl eframe::App for App {
    fn update(&mut self, ctx: &egui::Context, _frame: &mut eframe::Frame) {
        let doc_line = self.doc.as_ref().map(|d| {
            egui::WidgetText::from(
                d.path
                    .file_name()
                    .map(|n| n.to_string_lossy().into_owned())
                    .unwrap_or_default(),
            )
        });
        let mut want_open = false;
        ui::app_bar(
            ctx,
            |bar| {
                ui::app_title(bar, "PF", APP_NAME, doc_line);
                if let Some(d) = &self.doc {
                    let v = d.verdict();
                    bar.add_space(ui::SPACE_LG);
                    ui::badge(bar, verdict_word(v.tone), tone(v.tone));
                }
            },
            |bar| {
                if ui::primary_button(bar, "Open…").clicked() {
                    want_open = true;
                }
            },
        );
        if want_open {
            self.pick_file();
        }
        // The full verdict, always visible under the bar on every page; it
        // wraps instead of being cut off at small window sizes.
        if let Some(d) = &self.doc {
            let v = d.verdict();
            egui::TopBottomPanel::top("verdict").show(ctx, |strip| {
                strip.add_space(ui::SPACE_XS);
                ui::field_message(strip, tone(v.tone), &v.text);
                strip.add_space(ui::SPACE_XS);
            });
        }

        let has_analysis = self.doc.as_ref().and_then(|d| d.analysis()).is_some();
        ui::sidebar(
            ctx,
            |side| {
                ui::nav_section(side, "CAPTURE");
                if ui::nav_item(side, self.page == Page::Overview, "📋", "Overview", None).clicked()
                {
                    self.page = Page::Overview;
                }
                if has_analysis {
                    let n = self.zone_rows.len();
                    if ui::nav_item(side, self.page == Page::Zones, "📊", "Zones", Some(n))
                        .clicked()
                    {
                        self.page = Page::Zones;
                    }
                }
            },
            |_| {},
        );

        match &self.error {
            Some(e) => ui::status_bar(ctx, ui::Tone::Danger, "Could not open the file", e),
            None => match &self.doc {
                Some(d) => ui::status_bar(
                    ctx,
                    ui::Tone::Neutral,
                    &d.path.display().to_string(),
                    &format!("{} bytes", d.size),
                ),
                None => ui::status_bar(ctx, ui::Tone::Neutral, "No capture open", ""),
            },
        }

        let page = self.page;
        let sort = &mut self.sort;
        let rows = &mut self.zone_rows;
        let selected = &mut self.selected_zone;
        ui::page(ctx, |p| match &self.doc {
            None => ui::empty_state(
                p,
                "Open a capture",
                "Choose a .prof file written by libps3dkprof.",
            ),
            Some(d) => match &d.state {
                DocState::Malformed { diagnosis } => {
                    ui::page_header(
                        p,
                        "Malformed capture",
                        "The reader refused this file; nothing in it is shown.",
                    );
                    ui::field_message(p, ui::Tone::Danger, diagnosis);
                }
                DocState::Analysed(a) => match page {
                    Page::Overview => draw_overview(p, a),
                    Page::Zones => draw_zones(p, a, rows, sort, selected),
                },
            },
        });
    }
}

fn kv_grid(p: &mut egui::Ui, id: &str, rows: &[(String, String)]) {
    egui::Grid::new(id)
        .num_columns(2)
        .striped(true)
        .spacing([ui::SPACE_LG, ui::SPACE_XS])
        .show(p, |g| {
            for (k, v) in rows {
                g.label(egui::RichText::new(k).strong());
                g.label(v);
                g.end_row();
            }
        });
}

fn draw_overview(p: &mut egui::Ui, a: &ps3dk_prof::Analysis) {
    let o = overview(a);
    ui::page_header(
        p,
        "Overview",
        "Capture identity, clock, lanes and trailer, as the reader decoded them.",
    );
    if o.minor_warning {
        ui::field_message(
            p,
            ui::Tone::Warning,
            "Minor format version above 0: read as v1.",
        );
    }
    egui::ScrollArea::vertical().show(p, |p| {
        ui::section_label(p, "IDENTITY");
        kv_grid(p, "identity", &o.identity);
        p.add_space(ui::SPACE_LG);
        ui::section_label(p, "LANES");
        egui::Grid::new("lanes")
            .num_columns(7)
            .striped(true)
            .spacing([ui::SPACE_LG, ui::SPACE_XS])
            .show(p, |g| {
                for h in [
                    "Incarnation",
                    "Thread",
                    "Records",
                    "Frames",
                    "Spans",
                    "Measurement",
                    "Reasons",
                ] {
                    g.label(egui::RichText::new(h).strong());
                }
                g.end_row();
                for l in &o.lanes {
                    g.label(l.incarnation.to_string());
                    g.label(&l.name);
                    g.label(l.written.to_string());
                    g.label(l.frames.to_string());
                    g.label(l.spans.to_string());
                    if l.valid {
                        ui::badge(g, "VALID", ui::Tone::Success);
                    } else {
                        ui::badge(g, "NOT VALID", ui::Tone::Warning);
                    }
                    let mut why = l.reasons.clone();
                    if l.loss && why.is_empty() {
                        why = "stream may be missing records".into();
                    }
                    g.label(why);
                    g.end_row();
                }
            });
        p.add_space(ui::SPACE_LG);
        ui::section_label(p, "TRAILER");
        if o.trailer.is_empty() {
            ui::field_message(
                p,
                ui::Tone::Warning,
                "No trailer: the capture did not finish.",
            );
        } else {
            kv_grid(p, "trailer", &o.trailer);
        }
    });
}

fn draw_zones(
    p: &mut egui::Ui,
    a: &ps3dk_prof::Analysis,
    rows: &mut [ZoneStats],
    sort: &mut (ZoneColumn, bool),
    selected: &mut Option<ZoneKey>,
) {
    let tb = TimeBase::of(a);
    ui::page_header(
        p,
        "Zones",
        "Named-zone statistics from valid lanes only. Click a column to sort; unknown values sort last.",
    );
    if tb.freq_hz.is_none() {
        ui::field_message(
            p,
            ui::Tone::Warning,
            "No usable clock: durations are shown in timebase ticks only.",
        );
    }
    // The page scrolls vertically; the table scrolls horizontally, with its
    // bar always shown so a wide table never hides columns silently. The bar
    // is solid (a dormant floating bar is invisible until hover), its area is
    // held to the visible page width (the page's scroll area does not bound
    // child widths), and its handle uses the faint text colour (hover: muted)
    // because the shared theme paints it almost in the page colour.
    p.spacing_mut().scroll = egui::style::ScrollStyle::solid();
    let pal = ui::Palette::of(p);
    let w = &mut p.visuals_mut().widgets;
    w.inactive.bg_fill = pal.text_faint;
    w.hovered.bg_fill = pal.text_muted;
    w.active.bg_fill = pal.text_muted;
    let width = p.clip_rect().right() - p.cursor().left();
    egui::ScrollArea::horizontal()
        .max_width(width)
        .scroll_bar_visibility(egui::scroll_area::ScrollBarVisibility::AlwaysVisible)
        .show(p, |p| {
            egui::Grid::new("zones")
                .striped(true)
                .spacing([ui::SPACE_LG, ui::SPACE_XS])
                .show(p, |g| {
                    for col in ZoneColumn::ALL {
                        let mark = if sort.0 == col {
                            if sort.1 {
                                " (desc)"
                            } else {
                                " (asc)"
                            }
                        } else {
                            ""
                        };
                        if g.button(egui::RichText::new(format!("{}{mark}", col.title())).strong())
                            .clicked()
                        {
                            *sort = if sort.0 == col {
                                (col, !sort.1)
                            } else {
                                (col, col != ZoneColumn::Name)
                            };
                            sort_zones(rows, sort.0, sort.1);
                        }
                    }
                    g.end_row();
                    for z in rows.iter() {
                        for col in ZoneColumn::ALL {
                            let text = zone_cell(z, col, tb);
                            if col == ZoneColumn::Name {
                                let key = zone_key(z);
                                let is_sel = selected.as_ref() == Some(&key);
                                let r = g.selectable_label(is_sel, text);
                                let r = r.on_hover_text(zone_sites(a, z).join("\n"));
                                if r.clicked() {
                                    *selected = Some(key);
                                }
                            } else if col == ZoneColumn::Note && !text.is_empty() {
                                g.label(
                                    egui::RichText::new(text).color(ui::Palette::of(g).warning),
                                );
                            } else if let Some(exact) = zone_cell_exact(z, col, tb) {
                                g.label(text).on_hover_text(exact);
                            } else {
                                g.label(text);
                            }
                        }
                        g.end_row();
                    }
                });
        });
}

fn main() -> eframe::Result<()> {
    let args: Vec<String> = std::env::args().skip(1).collect();
    if args.iter().any(|a| a == "-v" || a == "--version") {
        println!("ps3dk-prof-viewer {}", ps3dk_version::VERSION);
        return Ok(());
    }
    // ps3dk-prof-viewer [--page=overview|zones] [--min-size] [capture.prof]
    let page = if args.iter().any(|a| a == "--page=zones") {
        Page::Zones
    } else {
        Page::Overview
    };
    let size = if args.iter().any(|a| a == "--min-size") {
        [860.0, 540.0]
    } else {
        [1240.0, 780.0]
    };
    let open = args
        .into_iter()
        .find(|a| !a.starts_with('-'))
        .map(PathBuf::from);
    let options = eframe::NativeOptions {
        viewport: egui::ViewportBuilder::default()
            .with_title(APP_NAME)
            .with_app_id("ps3dk-prof-viewer")
            .with_inner_size(size)
            .with_min_inner_size([860.0, 540.0]),
        ..Default::default()
    };
    eframe::run_native(
        APP_NAME,
        options,
        Box::new(move |cc| {
            ui::install(&cc.egui_ctx);
            let mut app = App::new(open);
            app.page = page;
            Ok(Box::new(app))
        }),
    )
}
