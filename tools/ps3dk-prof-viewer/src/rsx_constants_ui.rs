//! RSX frame constants (reader contract r6): the Constants tab and instance-run rows of the draw list.
//! Values, names, provenance and runs come from the reader; unknown components are shown as unknown.
use eframe::egui::{self, RichText};
use ps3dk_prof::rsx_frame::{self as rf, FrameView, Profile, Provenance, UniformKind};
use ps3dk_ui as ui;
use std::collections::BTreeSet;

/// One row of the draw list: a draw, a collapsed (or expanded) instance run, or a draw shown inside an expanded run.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Row {
    Draw(usize),
    Run(usize),
    Member(usize),
}
/// Collapse instance runs in a sorted, filtered draw list: a run appears once, as `Run`; an expanded run lists all of
/// its draws after it in submission order. A run takes its first listed draw's position, except in a time sort
/// (`by_time = Some(descending)`), where it is placed by its total time (the cost being hunted), ties by first ordinal.
pub fn display_rows(f: &FrameView, ids: &[usize], expanded: &BTreeSet<usize>, by_time: Option<bool>) -> Vec<Row> {
    let mut seen = BTreeSet::new();
    let mut blocks: Vec<Vec<Row>> = Vec::with_capacity(ids.len());
    for &i in ids {
        match f.draws[i].run.filter(|&r| r < f.instance_runs.len()) {
            None => blocks.push(vec![Row::Draw(i)]),
            Some(r) => {
                if !seen.insert(r) {
                    continue;
                }
                let mut b = vec![Row::Run(r)];
                if expanded.contains(&r) {
                    let run = &f.instance_runs[r];
                    b.extend((run.first..run.first + run.len).map(Row::Member));
                }
                blocks.push(b);
            }
        }
    }
    if let Some(descending) = by_time {
        let key = |b: &Vec<Row>| match b[0] {
            Row::Run(r) => (run_time(f, r).0, f.draws[f.instance_runs[r].first].raw.ordinal),
            Row::Draw(i) | Row::Member(i) => (f.draws[i].timing.interval(), f.draws[i].raw.ordinal),
        };
        blocks.sort_by(|a, b| {
            let ((ta, oa), (tb, ob)) = (key(a), key(b));
            let t = match (ta, tb) {
                (None, None) => std::cmp::Ordering::Equal,
                (None, _) => std::cmp::Ordering::Greater,
                (_, None) => std::cmp::Ordering::Less,
                (Some(x), Some(y)) if descending => y.cmp(&x),
                (Some(x), Some(y)) => x.cmp(&y),
            };
            t.then(oa.cmp(&ob))
        });
    }
    blocks.into_iter().flatten().collect()
}
/// The draws a row stands for.
pub fn row_draws(f: &FrameView, row: Row) -> std::ops::Range<usize> {
    match row {
        Row::Draw(i) | Row::Member(i) => i..i + 1,
        Row::Run(r) => {
            let run = &f.instance_runs[r];
            run.first..run.first + run.len
        }
    }
}
/// The run's total time over its timed draws, and how many were timed.
pub fn run_time(f: &FrameView, r: usize) -> (Option<u64>, usize) {
    let run = &f.instance_runs[r];
    let times: Vec<u64> = f.draws[run.first..run.first + run.len].iter().filter_map(|d| d.timing.interval()).collect();
    (if times.is_empty() { None } else { Some(times.iter().sum()) }, times.len())
}
/// The slots and offsets whose constants differ within the run: `c4 c5 · fp 0x40`.
pub fn varies(run: &rf::InstanceRun) -> String {
    let mut parts = Vec::new();
    if !run.vp_slots.is_empty() {
        parts.push(run.vp_slots.iter().map(|s| format!("c{s}")).collect::<Vec<_>>().join(" "));
    }
    if !run.fp_offsets.is_empty() {
        parts.push(format!("fp {}", run.fp_offsets.iter().map(|o| format!("{o:#x}")).collect::<Vec<_>>().join(" ")));
    }
    if parts.is_empty() { "no constant differs".into() } else { parts.join(" · ") }
}
/// The draw/kind cell of a run row.
pub fn run_label(f: &FrameView, r: usize, open: bool) -> String {
    let run = &f.instance_runs[r];
    let (a, b) = (&f.draws[run.first], &f.draws[run.first + run.len - 1]);
    format!("{} #{}–#{} {} x{}", if open { "[-]" } else { "[+]" }, a.raw.ordinal, b.raw.ordinal, a.raw.kind.name(), run.len)
}

/// A vec4 with unknown components as `?`.
pub fn vec4(v: &[Option<f32>; 4]) -> String {
    let c: Vec<String> = v.iter().map(|x| x.map_or_else(|| "?".to_string(), |x| format!("{x}"))).collect();
    format!("({})", c.join(", "))
}
/// Badge text and tone for one FP constant component's provenance. An inline write is the last one the decoder
/// saw, not a claim about the value at GPU time.
pub fn provenance_badge(p: Provenance) -> (&'static str, ui::Tone) {
    match p {
        Provenance::Inline => ("last inline write seen", ui::Tone::Accent),
        Provenance::Container => ("container: may differ at GPU time", ui::Tone::Warning),
        Provenance::Unknown => ("unknown", ui::Tone::Neutral),
    }
}
/// The badges of a vec4's per-component provenance: one when all four agree, else one per differing component
/// (`x y: last inline write seen`, `z w: container ...`).
pub fn provenance_badges(p: [Provenance; 4]) -> Vec<(String, ui::Tone)> {
    if p.iter().all(|&x| x == p[0]) {
        let (t, tone) = provenance_badge(p[0]);
        return vec![(t.to_string(), tone)];
    }
    let mut out: Vec<(Provenance, String)> = Vec::new();
    for (k, c) in ['x', 'y', 'z', 'w'].into_iter().enumerate() {
        match out.iter_mut().find(|(q, _)| *q == p[k]) {
            Some((_, lanes)) => {
                lanes.push(' ');
                lanes.push(c);
            }
            None => out.push((p[k], c.to_string())),
        }
    }
    out.into_iter()
        .map(|(q, lanes)| {
            let (t, tone) = provenance_badge(q);
            (format!("{lanes}: {t}"), tone)
        })
        .collect()
}

/// An FP constant's values with Unknown components as None: their words are the container fallback the runtime
/// stores, not the value the draw ran with.
pub fn fp_values(e: &rf::FpConstant) -> [Option<f32>; 4] {
    let f = e.floats();
    std::array::from_fn(|k| (e.provenance[k] != Provenance::Unknown).then_some(f[k]))
}
/// The raw words of an FP constant, Unknown components marked as container fallback (a diagnostic, never a value).
pub fn fp_raw(e: &rf::FpConstant) -> String {
    let w: Vec<String> = (0..4)
        .map(|k| {
            if e.provenance[k] == Provenance::Unknown {
                format!("{:#010x} (container fallback, not the draw's value)", e.words[k])
            } else {
                format!("{:#010x}", e.words[k])
            }
        })
        .collect();
    format!("raw words: {}", w.join(", "))
}
/// One per-instance FP cell: values with '?' for unknown components, the per-component provenance badges and the
/// raw-word diagnostic; None when the draw has no constant at that offset.
pub fn fp_cell(d: &rf::Draw, offset: u32) -> Option<(String, Vec<(String, ui::Tone)>, String)> {
    let e = fp_entry(d, d.fp, offset)?;
    Some((vec4(&fp_values(e)), provenance_badges(e.provenance), fp_raw(e)))
}

/// One Constants row: a named uniform or an unnamed slot/offset.
#[derive(Debug, Clone, PartialEq)]
pub struct ConstRow {
    pub profile: Profile,
    /// Uniform name, or None for an unnamed slot (VP literal pool) or FP block.
    pub name: Option<String>,
    /// VP first slot or FP ucode offset.
    pub resource: u32,
    pub values: Vec<[Option<f32>; 4]>,
    /// FP: each component's provenance (None for VP: the stream is exact).
    pub provenance: Option<[Provenance; 4]>,
    /// Changed since the previous draw (VP: a slot in `vpc_changed`; FP: different words or provenance).
    pub changed: bool,
}
fn fp_entry(d: &rf::Draw, fp: Option<usize>, offset: u32) -> Option<&rf::FpConstant> {
    d.fp_constants
        .as_ref()
        .filter(|c| fp.is_some_and(|i| c.program as usize == i + 1))
        .and_then(|c| c.entries.iter().find(|e| e.offset == offset))
}
/// The Constants tab rows of draw `index`: VP uniforms, FP uniforms, then unnamed VP slots and FP blocks.
pub fn rows(f: &FrameView, index: usize) -> Vec<ConstRow> {
    let d = &f.draws[index];
    let prev = index.checked_sub(1).map(|i| &f.draws[i]);
    let fp_changed = |offset: u32| {
        let now = fp_entry(d, d.fp, offset).map(|e| (e.words, e.provenance));
        let before = prev.and_then(|p| fp_entry(p, d.fp, offset)).map(|e| (e.words, e.provenance));
        prev.is_some() && now != before
    };
    let mut out: Vec<ConstRow> = rf::uniforms(f, d)
        .into_iter()
        .map(|mut u| {
            if let Some(pv) = u.provenance {
                for v in &mut u.values {
                    for k in 0..4 {
                        if pv[k] == Provenance::Unknown {
                            v[k] = None;
                        }
                    }
                }
            }
            let changed = match u.profile {
                Profile::Vertex => (u.resource..u.resource + u.values.len() as u32)
                    .any(|s| d.vpc_changed.contains(&(s as u16))),
                Profile::Fragment => fp_changed(u.resource),
            };
            ConstRow { profile: u.profile, name: Some(u.name), resource: u.resource, values: u.values,
                provenance: u.provenance, changed }
        })
        .collect();
    out.sort_by_key(|r| (r.profile == Profile::Fragment, r.resource));
    // unnamed VP slots: known, not covered by a VP uniform of this draw's VP
    if let Some(v) = &d.vp_constants {
        let named: BTreeSet<u32> = d
            .vp
            .map(|i| &f.programs[i].uniforms)
            .into_iter()
            .flatten()
            .filter(|u| u.kind == UniformKind::VpConstant)
            .flat_map(|u| u.resource..u.resource + u.count)
            .collect();
        for s in 0..ps3dk_prof::rsx_state::VP_CONSTS as u32 {
            if !named.contains(&s) && v.get(s as usize).is_some() {
                out.push(ConstRow { profile: Profile::Vertex, name: None, resource: s,
                    values: vec![v.floats(s as usize)], provenance: None,
                    changed: d.vpc_changed.contains(&(s as u16)) });
            }
        }
    }
    // unnamed FP blocks: entries at no uniform's offset
    if let (Some(i), Some(c)) = (d.fp, &d.fp_constants) {
        if c.program as usize == i + 1 {
            let named: BTreeSet<u32> = f.programs[i]
                .uniforms
                .iter()
                .filter(|u| u.kind == UniformKind::FpEmbedded)
                .map(|u| u.resource)
                .collect();
            for e in c.entries.iter().filter(|e| !named.contains(&e.offset)) {
                out.push(ConstRow { profile: Profile::Fragment, name: None, resource: e.offset,
                    values: vec![fp_values(e)], provenance: Some(e.provenance), changed: fp_changed(e.offset) });
            }
        }
    }
    out
}

fn const_table(p: &mut egui::Ui, id: &str, rows: &[&ConstRow]) {
    let pal = ui::Palette::of(p);
    egui::ScrollArea::vertical().id_salt(id).max_height(260.0).show(p, |p| {
        egui::Grid::new((id, "grid")).striped(true).num_columns(4).show(p, |p| {
            for r in rows {
                let who = match (&r.name, r.profile) {
                    (Some(n), _) => n.clone(),
                    (None, Profile::Vertex) => format!("c{} (unnamed)", r.resource),
                    (None, Profile::Fragment) => format!("{:#x} (unnamed)", r.resource),
                };
                p.label(who);
                p.monospace(match r.profile {
                    Profile::Vertex if r.values.len() > 1 => format!("c{}..c{}", r.resource, r.resource + r.values.len() as u32 - 1),
                    Profile::Vertex => format!("c{}", r.resource),
                    Profile::Fragment => format!("{:#x}", r.resource),
                });
                let text = r.values.iter().map(vec4).collect::<Vec<_>>().join("\n");
                let t = if r.changed {
                    RichText::new(text).monospace().strong().color(ui::Tone::Warning.color(&pal))
                } else {
                    RichText::new(text).monospace()
                };
                let resp = p.label(t);
                if r.changed {
                    resp.on_hover_text("changed since the previous draw");
                }
                match r.provenance {
                    Some(pv) => {
                        p.horizontal_wrapped(|p| {
                            for (t, tone) in provenance_badges(pv) {
                                ui::badge(p, &t, tone);
                            }
                        });
                    }
                    None => {
                        p.label("");
                    }
                }
                p.end_row();
            }
        });
    });
}

pub fn constants_tab(p: &mut egui::Ui, f: &FrameView, index: usize, v: &crate::rsx_frame_view::RsxFrameView) {
    let d = &f.draws[index];
    if d.vp_constants.is_none() && d.fp_constants.is_none() {
        return ui::field_message(p, ui::Tone::Neutral,
            "This capture has no constants (recorded only with the phase 3 decoder).");
    }
    let pal = ui::Palette::of(p);
    p.horizontal_wrapped(|p| {
        p.label(RichText::new("changed since the previous draw").strong().color(ui::Tone::Warning.color(&pal)));
        p.label("· ? = component not written since the last decoder reset");
    });
    let all = rows(f, index);
    for (title, profile, prog) in [("Vertex program constants", Profile::Vertex, d.vp), ("Fragment program constants", Profile::Fragment, d.fp)] {
        ui::section_label(p, title);
        if let Some(pr) = prog.and_then(|i| f.programs.get(i)).filter(|pr| pr.uniforms_truncated) {
            ui::field_message(p, ui::Tone::Warning, &format!("{} has more than 32 named uniforms; the rest are listed unnamed.", rf::program_name(pr, &Default::default())));
        }
        if profile == Profile::Fragment {
            if d.fp_constants.as_ref().is_some_and(|c| c.truncated) {
                ui::field_message(p, ui::Tone::Warning, "The fragment program has more constant blocks than were captured.");
            }
        }
        let rows: Vec<&ConstRow> = all.iter().filter(|r| r.profile == profile).collect();
        if rows.is_empty() {
            p.label("None recorded for this draw.");
        } else {
            const_table(p, &format!("rsxc-{title}"), &rows);
        }
    }
    if let Some(r) = d.run.filter(|&r| r < f.instance_runs.len()) {
        run_instances(p, f, r, v);
    }
}

/// Per-instance values of the slots that differ within a run, with the run's total time.
fn run_instances(p: &mut egui::Ui, f: &FrameView, r: usize, v: &crate::rsx_frame_view::RsxFrameView) {
    let run = &f.instance_runs[r];
    let (sum, timed) = run_time(f, r);
    ui::section_label(p, &format!("Instance run x{}", run.len));
    p.horizontal_wrapped(|p| {
        p.label(format!("Varies: {} · total", varies(run)));
        let d = v.duration(sum);
        p.label(d.text).on_hover_text(d.hover);
        p.label(format!("({timed} of {} timed)", run.len));
    });
    let cols = 2 + run.vp_slots.len() + run.fp_offsets.len();
    egui::ScrollArea::both().id_salt(("rsxc-run", r)).max_height(220.0).show(p, |p| {
        egui::Grid::new(("rsxc-run-grid", r)).striped(true).num_columns(cols).show(p, |p| {
            p.label(RichText::new("Draw").strong());
            p.label(RichText::new("Time").strong());
            for s in &run.vp_slots {
                p.label(RichText::new(format!("c{s}")).strong());
            }
            for o in &run.fp_offsets {
                p.label(RichText::new(format!("fp {o:#x}")).strong());
            }
            p.end_row();
            for d in &f.draws[run.first..run.first + run.len] {
                p.label(format!("#{}", d.raw.ordinal));
                let t = v.duration(d.timing.interval());
                p.label(t.text).on_hover_text(t.hover);
                for &s in &run.vp_slots {
                    p.monospace(d.vp_constants.as_ref().map_or_else(|| "unknown".into(), |c| vec4(&c.floats(s as usize))));
                }
                for &o in &run.fp_offsets {
                    match fp_cell(d, o) {
                        None => {
                            p.label(RichText::new("unknown").italics().weak());
                        }
                        Some((values, badges, raw)) => {
                            p.vertical(|p| {
                                p.monospace(values).on_hover_text(raw);
                                p.horizontal_wrapped(|p| {
                                    for (t, tone) in badges {
                                        ui::badge(p, &t, tone);
                                    }
                                });
                            });
                        }
                    }
                }
                p.end_row();
            }
        });
    });
}
