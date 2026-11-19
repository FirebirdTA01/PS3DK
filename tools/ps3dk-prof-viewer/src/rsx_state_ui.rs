//! RSX frame register state (reader contract r5): State, Textures and Buffers tabs, the per-draw
//! change list and grouping by render target. Values come from `rsx_state::interpret`; nothing here
//! decodes registers, and an unknown value is never shown as a default.
use eframe::egui::{self, RichText};
use ps3dk_prof::rsx_frame::{self as rf, FrameView};
use ps3dk_prof::rsx_state::{self as rs, Field, Registers, StateView, Value};
use ps3dk_ui as ui;
use std::sync::Arc;

/// Interpreted tables, cached per shared register file (consecutive unchanged draws share one).
#[derive(Default)]
pub struct Cache {
    draw: Option<(Arc<Registers>, Arc<StateView>)>,
    baseline: Option<(Arc<Registers>, Arc<StateView>)>,
}
impl Cache {
    fn get(slot: &mut Option<(Arc<Registers>, Arc<StateView>)>, r: &Arc<Registers>) -> Arc<StateView> {
        if let Some((k, v)) = slot {
            if Arc::ptr_eq(k, r) {
                return v.clone();
            }
        }
        let v = Arc::new(rs::interpret(r));
        *slot = Some((r.clone(), v.clone()));
        v
    }
    pub fn draw(&mut self, r: &Arc<Registers>) -> Arc<StateView> {
        Self::get(&mut self.draw, r)
    }
    pub fn baseline(&mut self, r: &Arc<Registers>) -> Arc<StateView> {
        Self::get(&mut self.baseline, r)
    }
}

/// Field value text; unknown reads "unknown", never a default.
pub fn text(v: &Value) -> String {
    match v {
        Value::Unknown => "unknown".into(),
        v => v.text(),
    }
}
/// The value of `name` in a table (Unknown when the table has no such field).
pub fn value<'a>(fields: &'a [Field], name: &str) -> &'a Value {
    fields.iter().find(|f| f.name == name).map_or(&Value::Unknown, |f| &f.value)
}
fn val(fields: &[Field], name: &str) -> String {
    text(value(fields, name))
}
fn enabled(fields: &[Field], name: &str) -> bool {
    matches!(value(fields, name), Value::Bool(true))
}

/// Render-state groups: the field's leading path element (`blend.src_rgb` -> `blend`), first-seen order.
pub fn render_groups(render: &[Field]) -> Vec<(String, Vec<usize>)> {
    let mut out: Vec<(String, Vec<usize>)> = Vec::new();
    for (i, f) in render.iter().enumerate() {
        let g = f.name.split('.').next().unwrap_or(&f.name).to_string();
        match out.iter_mut().find(|(k, _)| *k == g) {
            Some((_, v)) => v.push(i),
            None => out.push((g, vec![i])),
        }
    }
    out
}

/// One line of the change list: register name, value before this draw, value at it.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Change {
    pub register: u32,
    pub name: String,
    pub before: String,
    pub after: String,
}
/// Registers this draw's STATE changed, against the previous draw (the capture baseline for the first
/// draw; unknown after a decoder reset).
pub fn changes(f: &FrameView, index: usize) -> Vec<Change> {
    let Some(d) = f.draws.get(index) else { return Vec::new() };
    let before = if d.raw.state_reset {
        None
    } else if index == 0 {
        f.baseline.clone()
    } else {
        f.draws[index - 1].registers.clone()
    };
    let hex = |v: Option<u32>| v.map_or_else(|| "unknown".to_string(), |v| format!("{v:#010x}"));
    d.changed
        .iter()
        .map(|&m| Change {
            register: m,
            name: rs::register_name(m),
            before: hex(before.as_ref().and_then(|r| r.get(m))),
            after: hex(d.registers.as_ref().and_then(|r| r.get(m))),
        })
        .collect()
}

/// The render target a draw writes: enabled colour surfaces and zeta with location, offset and formats.
pub fn render_target(s: &StateView) -> String {
    let sf = &s.surfaces;
    let mut parts = Vec::new();
    for n in 0..4 {
        match value(sf, &format!("targets.color{n}")) {
            Value::Bool(false) => {}
            Value::Bool(true) => parts.push(format!(
                "color{n} {} {}",
                val(sf, &format!("color{n}.location")),
                val(sf, &format!("color{n}.offset"))
            )),
            _ => parts.push(format!("color{n} unknown")),
        }
    }
    parts.push(format!("zeta {} {}", val(sf, "zeta.location"), val(sf, "zeta.offset")));
    parts.push(format!("{} / {}", val(sf, "color_format"), val(sf, "zeta_format")));
    parts.join(" · ")
}
/// Draws grouped by render target, with the reader's timed/untimed rule and order (largest sum first).
pub fn target_groups(f: &FrameView) -> Vec<rf::Group> {
    let mut groups: Vec<rf::Group> = Vec::new();
    let mut last: Option<(Arc<Registers>, String)> = None;
    for (i, d) in f.draws.iter().enumerate() {
        let key = match &d.registers {
            None => "(no register state in this capture)".to_string(),
            Some(r) => match &last {
                Some((k, key)) if Arc::ptr_eq(k, r) => key.clone(),
                _ => {
                    let key = render_target(&rs::interpret(r));
                    last = Some((r.clone(), key.clone()));
                    key
                }
            },
        };
        let g = match groups.iter().position(|g| g.key == key) {
            Some(p) => &mut groups[p],
            None => {
                groups.push(rf::Group { key, draws: Vec::new(), timed: 0, untimed: 0, sum: 0 });
                groups.last_mut().unwrap()
            }
        };
        g.draws.push(i);
        match d.timing.interval() {
            Some(v) => {
                g.timed += 1;
                g.sum += v;
            }
            None => g.untimed += 1,
        }
    }
    groups.sort_by(|a, b| b.sum.cmp(&a.sum).then(a.draws[0].cmp(&b.draws[0])));
    groups
}

const NO_STATE: &str = "This capture has no register state (decoder 1): render state, textures and buffers were not recorded.";

/// How a value cell is emphasised: changed by this draw, different from the capture baseline, unknown.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Mark {
    Changed,
    NonDefault,
    Unknown,
    Plain,
}
pub fn mark(field: &Field, baseline: Option<&Field>, changed: &[u32]) -> Mark {
    if field.value == Value::Unknown {
        Mark::Unknown
    } else if changed.binary_search(&field.register).is_ok() {
        Mark::Changed
    } else if baseline.is_some_and(|b| b.value != field.value) {
        Mark::NonDefault
    } else {
        Mark::Plain
    }
}
fn cell(p: &mut egui::Ui, field: &Field, m: Mark) {
    let pal = ui::Palette::of(p);
    let t = text(&field.value);
    let r = match m {
        Mark::Unknown => p.label(RichText::new(t).weak().italics()),
        Mark::Changed => p.label(RichText::new(t).strong().color(ui::Tone::Warning.color(&pal))),
        Mark::NonDefault => p.label(RichText::new(t).color(ui::Tone::Accent.color(&pal))),
        Mark::Plain => p.label(t),
    };
    r.on_hover_text(format!("{} ({:#06x}){}", rs::register_name(field.register), field.register, match m {
        Mark::Changed => " · changed by this draw",
        Mark::NonDefault => " · differs from the state at arming",
        Mark::Unknown => " · not written since the last decoder reset",
        Mark::Plain => "",
    }));
}
fn legend(p: &mut egui::Ui) {
    let pal = ui::Palette::of(p);
    p.horizontal_wrapped(|p| {
        p.label(RichText::new("changed by this draw").strong().color(ui::Tone::Warning.color(&pal)));
        p.label(RichText::new("differs from the state at arming").color(ui::Tone::Accent.color(&pal)));
        p.label(RichText::new("unknown").weak().italics());
    });
}
fn table(p: &mut egui::Ui, id: &str, fields: &[Field], idx: &[usize], base: Option<&[Field]>, changed: &[u32]) {
    egui::Grid::new(id).striped(true).num_columns(2).show(p, |p| {
        for &i in idx {
            let f = &fields[i];
            p.label(&f.name);
            cell(p, f, mark(f, base.and_then(|b| b.get(i)), changed));
            p.end_row();
        }
    });
}

/// Selected draw context for the state tabs.
pub struct Ctx<'a> {
    pub f: &'a FrameView,
    pub index: usize,
}

pub fn changed_list(p: &mut egui::Ui, c: &Ctx) {
    let d = &c.f.draws[c.index];
    if d.registers.is_none() {
        return;
    }
    let list = changes(c.f, c.index);
    let title = if d.raw.state_reset {
        format!("Changed since previous draw ({}; decoder reset before this draw)", list.len())
    } else {
        format!("Changed since previous draw ({})", list.len())
    };
    p.collapsing(title, |p| {
        if list.is_empty() {
            p.label("No register changed.");
            return;
        }
        egui::ScrollArea::vertical().id_salt("rsxs-changed").max_height(160.0).show(p, |p| {
            egui::Grid::new("rsxs-changed-grid").striped(true).num_columns(3).show(p, |p| {
                for ch in &list {
                    p.label(&ch.name).on_hover_text(format!("{:#06x}", ch.register));
                    p.monospace(&ch.before);
                    p.monospace(format!("→ {}", ch.after));
                    p.end_row();
                }
            });
        });
    });
}

pub fn state_tab(p: &mut egui::Ui, c: &Ctx, cache: &mut Cache) {
    let d = &c.f.draws[c.index];
    let Some(r) = &d.registers else { return ui::field_message(p, ui::Tone::Neutral, NO_STATE) };
    let s = cache.draw(r);
    let base = c.f.baseline.as_ref().map(|b| cache.baseline(b));
    legend(p);
    changed_list(p, c);
    for (g, idx) in render_groups(&s.render) {
        egui::CollapsingHeader::new(&g).id_salt(("rsxs-render", &g)).default_open(true).show(p, |p| {
            table(p, &format!("rsxs-{g}"), &s.render, &idx, base.as_ref().map(|b| b.render.as_slice()), &d.changed);
        });
    }
}

pub fn textures_tab(p: &mut egui::Ui, c: &Ctx, cache: &mut Cache) {
    let d = &c.f.draws[c.index];
    let Some(r) = &d.registers else { return ui::field_message(p, ui::Tone::Neutral, NO_STATE) };
    let s = cache.draw(r);
    let used: Vec<usize> = (0..s.textures.len()).filter(|&i| enabled(&s.textures[i], "enable")).collect();
    let unknown = (0..s.textures.len()).filter(|&i| *value(&s.textures[i], "enable") == Value::Unknown).count();
    if used.is_empty() {
        p.label("No texture slot is enabled at this draw.");
    } else {
        egui::Grid::new("rsxs-textures").striped(true).num_columns(7).show(p, |p| {
            for h in ["Slot", "Size", "Format", "Mips", "Filter (min / mag)", "Wrap (s / t / r)", "Offset · location"] {
                p.label(RichText::new(h).strong());
            }
            p.end_row();
            for &i in &used {
                let t = &s.textures[i];
                p.label(i.to_string());
                p.label(format!("{}x{}{}", val(t, "width"), val(t, "height"), match value(t, "depth") {
                    Value::Uint(n) if *n > 1 => format!("x{n}"),
                    _ => String::new(),
                }));
                p.label(format!("{}{}", val(t, "format"), if enabled(t, "linear") { " linear" } else { "" }));
                p.label(val(t, "mipmaps"));
                p.label(format!("{} / {}", val(t, "filter.min"), val(t, "filter.mag")));
                p.label(format!("{} / {} / {}", val(t, "wrap.s"), val(t, "wrap.t"), val(t, "wrap.r")));
                p.label(format!("{} · {}", val(t, "offset"), val(t, "location")));
                p.end_row();
            }
        });
    }
    if unknown > 0 {
        ui::field_message(p, ui::Tone::Neutral, &format!("{unknown} slots have an unknown enable state (not written since the last decoder reset)."));
    }
    ui::field_message(p, ui::Tone::Neutral, "Enabled slots are listed; which of them the fragment program samples is not recorded.");
}

pub fn buffers_tab(p: &mut egui::Ui, c: &Ctx, cache: &mut Cache) {
    let d = &c.f.draws[c.index];
    let Some(r) = &d.registers else { return ui::field_message(p, ui::Tone::Neutral, NO_STATE) };
    let s = cache.draw(r);
    ui::section_label(p, "Vertex attributes");
    let used: Vec<usize> = (0..s.attributes.len()).filter(|&i| enabled(&s.attributes[i], "enabled")).collect();
    if used.is_empty() {
        p.label("No vertex attribute is enabled at this draw.");
    } else {
        egui::Grid::new("rsxs-attributes").striped(true).num_columns(6).show(p, |p| {
            for h in ["Index", "Format", "Stride", "Offset · location", "Divider", "Frequency op"] {
                p.label(RichText::new(h).strong());
            }
            p.end_row();
            for &i in &used {
                let a = &s.attributes[i];
                p.label(i.to_string());
                p.label(format!("{} x{}", val(a, "type"), val(a, "size")));
                p.label(val(a, "stride"));
                p.label(format!("{} · {}", val(a, "offset"), val(a, "location")));
                p.label(val(a, "frequency"));
                p.label(val(a, "frequency_op"));
                p.end_row();
            }
        });
    }
    ui::section_label(p, "Index stream");
    if matches!(d.raw.kind, rf::DrawKind::Index | rf::DrawKind::InlineIndex) {
        let idx: Vec<usize> = (0..s.index.len()).collect();
        let base = c.f.baseline.as_ref().map(|b| cache.baseline(b));
        table(p, "rsxs-index", &s.index, &idx, base.as_ref().map(|b| b.index.as_slice()), &d.changed);
    } else {
        p.label("Not an indexed draw.");
    }
    ui::section_label(p, "Surfaces");
    let idx: Vec<usize> = (0..s.surfaces.len()).collect();
    let base = c.f.baseline.as_ref().map(|b| cache.baseline(b));
    table(p, "rsxs-surfaces", &s.surfaces, &idx, base.as_ref().map(|b| b.surfaces.as_slice()), &d.changed);
}
