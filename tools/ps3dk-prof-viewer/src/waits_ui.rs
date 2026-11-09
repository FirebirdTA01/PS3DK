//! Virtualized automatic wait observations. No inference of blocked CPU time or causality.
use crate::waits::{elapsed, operation, Key, WaitsView, CAPTION, COVERAGE, PAINT_ROWS};
use eframe::egui::{self, Rect};
use ps3dk_ui as ui;

#[derive(Debug, Default)]
pub struct WaitState {
    pub selected: Option<Key>,
}
#[derive(Debug, Default)]
pub struct Drawn {
    pub items: Vec<(Key, Rect)>,
    pub rows_built: usize,
    pub groups_built: usize,
    pub actors_built: usize,
    pub omitted: usize,
    pub status: String,
    pub calls_rect: Option<Rect>,
    pub visible_calls: Vec<(u32, u64)>,
}

pub fn status(v: &WaitsView) -> String {
    if !v.capability {
        "UNSUPPORTED: this capture has no automatic-wait capability.".into()
    } else if let Some(why) = v.refusal {
        format!("REFUSED: {why}; no wait rows analysed.")
    } else if !v.status_present {
        "PROVISIONAL: WAIT_STATUS is absent; final loss and refusal counts are UNKNOWN.".into()
    } else {
        "RECORDED: wait status received; this does not establish full coverage.".into()
    }
}

fn site(x: u64) -> String {
    if x == 0 {
        "site UNKNOWN".into()
    } else {
        format!("site 0x{x:x}")
    }
}

fn summary(s: Option<&ps3dk_prof::waits::Summary>) -> String {
    match s {
        None => "counts UNKNOWN (no status)".into(),
        Some(s) => format!(
            "published {} / dropped {} / refused {} ({} {}) / unregistered {}; {}{}{}",
            s.published,
            s.dropped,
            s.refused,
            if s.actor == 0 {
                "profiler-internal excluded"
            } else {
                "nested/reentry"
            },
            s.nested,
            s.unregistered,
            if s.flags & 1 != 0 {
                "LOWER BOUNDS "
            } else {
                ""
            },
            if s.flags & 2 != 0 { "SATURATED " } else { "" },
            if s.flags & 4 != 0 {
                "ACTOR UNACKNOWLEDGED"
            } else {
                ""
            }
        ),
    }
}

pub fn page(p: &mut egui::Ui, v: &WaitsView, st: &mut WaitState, d: &mut Drawn) {
    *d = Drawn::default();
    ui::page_header(p, "Waits", CAPTION);
    d.status = status(v);
    ui::field_message(p, ui::Tone::Neutral, &d.status);
    p.label(COVERAGE);
    if !v.capability || v.refusal.is_some() {
        return;
    }
    if st.selected.is_some_and(|k| v.group(k).is_none()) {
        st.selected = None;
    }
    let frequency = v.config.as_ref().map(|c| c.frequency);
    egui::ScrollArea::vertical().id_salt("wait-page").show(p, |p| {
        if let Some(c) = &v.config {
            p.label(format!("Wait allocation {} bytes / ceiling {} bytes; max_actors {} lifetime incarnations, ring {} records per actor.",
                c.allocated_bytes, c.max_bytes, c.max_actors, c.ring_records));
            p.label(format!("Added SDK static payload {} bytes; added TLS payload {} bytes per thread (alignment excluded). Observation path stack bound {} bytes; whole pthread call stack UNKNOWN.", c.static_bytes, c.tls_bytes, c.stack_bytes));
            p.label("Stack bound excludes firmware/kernel, resolver/deadline prework, registration/cleanup, application callers and asynchronous frames.");
        }
        ui::section_label(p, "COVERAGE");
        p.label("CAPACITY REFUSED actors have unknown call counts. Raise max_actors within max_bytes to admit more lifetime incarnations; slots are not reused.");
        p.label(format!("Capture aggregate: {}", summary(v.aggregate.as_ref())));
        egui::ScrollArea::vertical().id_salt("wait-actors").max_height(110.0)
            .show_rows(p, 22.0, v.coverage.len(), |p, range| {
                for i in range.take(PAINT_ROWS) {
                    let a = &v.coverage[i];
                    let text = format!("actor {} {}: {} — {}", a.id, a.name, a.state, summary(a.summary.as_ref()));
                    p.add(egui::Label::new(&text).truncate()).on_hover_text(text);
                    d.actors_built += 1;
                }
            });
        p.label(format!("{} of {} actor rows drawn in this viewport.", d.actors_built, v.coverage.len()));
        ui::section_label(p, "OPERATION / CALL SITE");
        p.label("Counts describe retained calls, not every call attempted. Elapsed samples exclude OPEN, ORPHAN RETURN and regressed clocks. Concurrent actors are not summed.");
        if v.groups.is_empty() { p.label("No retained wait calls. This is not proof that no waits occurred."); }
        egui::ScrollArea::vertical().id_salt("wait-groups").max_height(180.0)
            .show_rows(p, 24.0, v.groups.len(), |p, range| {
                for i in range.take(PAINT_ROWS) {
                    let g = &v.groups[i];
                    let label = format!("{} / {} | {} retained, {} elapsed | min {}, max {} | OPEN {}, orphan {}, regressed {}, errors {}",
                        operation(g.key.0), site(g.key.1), g.calls, g.elapsed_samples,
                        elapsed(g.min_ticks, frequency), elapsed(g.max_ticks, frequency),
                        g.open, g.orphan_returns, g.regressed, g.errors);
                    let r = p.add(egui::Button::new(&label).selected(st.selected == Some(g.key)).truncate()).on_hover_text(label);
                    d.items.push((g.key, r.rect));
                    if r.clicked() { st.selected = Some(g.key); }
                    d.groups_built += 1;
                }
            });
        p.label(format!("{} of {} groups drawn in this viewport.", d.groups_built, v.groups.len()));
        if let Some(g) = st.selected.and_then(|key| v.group(key)) {
            ui::section_label(p, "RETAINED CALLS");
            p.label("Targets and sites are raw values; handle reuse and matching signals are unresolved. Frame attribution and dependency UNKNOWN.");
            let scroll = egui::ScrollArea::vertical().id_salt(("wait-calls", g.key)).max_height(360.0)
                .show_rows(p, 24.0, g.rows.len(), |p, range| {
                    for i in range.take(PAINT_ROWS) {
                        let row = &v.rows[g.rows[i]];
                        let r = &row.record;
                        d.visible_calls.push((r.actor, r.call));
                        let result = if r.kind == 2 { format!("raw {} / API {}", r.raw_result, r.api_result) } else { "return UNKNOWN".into() };
                        let text = format!("actor {} call {}: {} | {} | raw target 0x{:x}, secondary 0x{:x}, raw timeout {} | {}",
                            r.actor, r.call, row.state, elapsed(row.ticks, frequency), r.target, r.secondary, r.timeout, result);
                        p.add(egui::Label::new(&text).truncate()).on_hover_text(text);
                        d.rows_built += 1;
                    }
                });
            d.calls_rect = Some(scroll.inner_rect);
            d.omitted = g.rows.len().saturating_sub(d.rows_built);
            p.label(format!("{} retained call rows outside this viewport or paint limit.", d.omitted));
        }
    });
}
