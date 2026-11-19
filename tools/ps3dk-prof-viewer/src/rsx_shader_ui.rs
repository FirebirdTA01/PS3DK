//! RSX frame Shader view (reader contract r7): every program of the view with its identity and provenance, the
//! draws using it, their total and mean time (in the view's one timing mode), the constants that change across those
//! draws, and the disassembly when the capture kept the program's bytes.
use crate::rsx_frame_view::{program_label, provenance, Names, RsxFrameView};
use eframe::egui::{self, RichText};
use ps3dk_prof::nv40_disasm::{self as nv, Line, LineKind};
use ps3dk_prof::rsx_frame::{FrameView, ProgramBytes, Profile, UniformKind};
use ps3dk_ui as ui;
use std::collections::{BTreeMap, BTreeSet};
use std::sync::Arc;

/// One program of the view.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct ShaderRow {
    /// Index into [`FrameView::programs`].
    pub index: usize,
    pub draws: usize,
    pub timed: usize,
    /// Sum of its timed draws' intervals (raw ticks, this view's mode only).
    pub sum: u64,
    /// Constants whose value differs between its draws: uniform name (with the slot or element) or slot/offset.
    pub varying: Vec<String>,
}
impl ShaderRow {
    pub fn mean(&self) -> Option<u64> {
        (self.timed > 0).then(|| self.sum / self.timed as u64)
    }
}

fn vp_name(f: &FrameView, i: usize, slot: u32) -> String {
    f.programs[i]
        .uniforms
        .iter()
        .find(|u| u.kind == UniformKind::VpConstant && (u.resource..u.resource + u.count).contains(&slot))
        .map_or_else(
            || format!("c{slot}"),
            |u| if u.count > 1 { format!("{}[{}] (c{slot})", u.name, slot - u.resource) } else { format!("{} (c{slot})", u.name) },
        )
}
fn fp_name(f: &FrameView, i: usize, offset: u32) -> String {
    f.programs[i]
        .uniforms
        .iter()
        .find(|u| u.kind == UniformKind::FpEmbedded && u.resource == offset)
        .map_or_else(|| format!("{offset:#x}"), |u| format!("{} ({offset:#x})", u.name))
}

/// The programs of `f` with their draws, time and varying constants; largest total time first.
pub fn shader_rows(f: &FrameView) -> Vec<ShaderRow> {
    let mut out = Vec::with_capacity(f.programs.len());
    for (i, p) in f.programs.iter().enumerate() {
        let uses: Vec<&ps3dk_prof::rsx_frame::Draw> = f
            .draws
            .iter()
            .filter(|d| match p.profile {
                Profile::Vertex => d.vp == Some(i),
                Profile::Fragment => d.fp == Some(i),
            })
            .collect();
        let times: Vec<u64> = uses.iter().filter_map(|d| d.timing.interval()).collect();
        let mut varying = Vec::new();
        match p.profile {
            Profile::Vertex => {
                let mut slots: BTreeSet<u32> = p
                    .uniforms
                    .iter()
                    .filter(|u| u.kind == UniformKind::VpConstant)
                    .flat_map(|u| u.resource..u.resource + u.count)
                    .collect();
                slots.extend(uses.iter().flat_map(|d| d.vpc_changed.iter().map(|&s| s as u32)));
                // consecutive draws share one constant file until a slot changes: compare distinct files only
                let mut files: Vec<&Arc<ps3dk_prof::rsx_state::VpConstants>> = Vec::new();
                for c in uses.iter().filter_map(|d| d.vp_constants.as_ref()) {
                    if files.last().is_none_or(|l| !Arc::ptr_eq(l, c)) {
                        files.push(c);
                    }
                }
                for s in slots {
                    let first = files.first().map(|c| c.get(s as usize));
                    if files.iter().any(|c| Some(c.get(s as usize)) != first) {
                        varying.push(vp_name(f, i, s));
                    }
                }
            }
            Profile::Fragment => {
                let mut seen: BTreeMap<u32, [u32; 4]> = BTreeMap::new();
                let mut differs: BTreeSet<u32> = BTreeSet::new();
                for c in uses.iter().filter_map(|d| d.fp_constants.as_ref()).filter(|c| c.program as usize == i + 1) {
                    for e in &c.entries {
                        if *seen.entry(e.offset).or_insert(e.words) != e.words {
                            differs.insert(e.offset);
                        }
                    }
                }
                varying.extend(differs.into_iter().map(|o| fp_name(f, i, o)));
            }
        }
        out.push(ShaderRow { index: i, draws: uses.len(), timed: times.len(), sum: times.iter().sum(), varying });
    }
    out.sort_by(|a, b| b.sum.cmp(&a.sum).then(a.index.cmp(&b.index)));
    out
}

/// Disassemble stored program bytes: vertex words big-endian as uploaded, fragment bytes as they lay in memory. A
/// truncated program gets a closing note; unknown encodings come back raw (`??` lines) from the disassembler.
pub fn disassembly(b: &ProgramBytes) -> Vec<Line> {
    let mut lines = match b.profile {
        Profile::Vertex => {
            let words: Vec<u32> = b.bytes.chunks_exact(4).map(|w| u32::from_be_bytes([w[0], w[1], w[2], w[3]])).collect();
            nv::disasm_vp(&words)
        }
        Profile::Fragment => nv::disasm_fp(&b.bytes),
    };
    if !b.complete {
        lines.push(Line {
            offset: b.bytes.len(),
            words: Vec::new(),
            text: format!("capture truncated the program: {} of {} bytes kept; the rest is missing", b.bytes.len(), b.total),
            kind: LineKind::Note,
        });
    }
    lines
}

/// Why a program has no disassembly.
pub fn bytes_missing(f: &FrameView, i: usize, fp_bytes_cap: Option<u32>) -> String {
    let p = &f.programs[i];
    if p.unreadable {
        return "Bytes not kept: the program's identity is unavailable, so no bytes are attached to it.".into();
    }
    match p.profile {
        Profile::Vertex => "Bytes not kept: vertex-program bytes are recorded by captures from the phase 4 decoder; this capture has none for this program.".into(),
        Profile::Fragment => format!(
            "Bytes not kept: fragment-program bytes are stored only when the capture is requested with 'Keep fragment \
             shader bytes' (the FP_BYTES request flag), for programs bound while the capture was armed, up to {} per capture.",
            fp_bytes_cap.filter(|&c| c > 0).map_or("the agent's byte cap".to_string(), |c| format!("{c} bytes"))),
    }
}

/// Per-view caches: the rows and the disassembly of each program opened.
#[derive(Default)]
pub struct Cache {
    key: Option<(u64, u32, ps3dk_prof::rsx_frame::Half)>,
    rows: Arc<Vec<ShaderRow>>,
    disasm: BTreeMap<(Profile, u64), Arc<Vec<Line>>>,
}
impl Cache {
    pub fn rows(&mut self, v: &RsxFrameView, f: &FrameView) -> Arc<Vec<ShaderRow>> {
        let key = Some((v.generation, f.id, f.half));
        if self.key != key {
            self.key = key;
            self.rows = Arc::new(shader_rows(f));
            self.disasm.clear();
        }
        self.rows.clone()
    }
    fn disasm(&mut self, b: &ProgramBytes) -> Arc<Vec<Line>> {
        self.disasm.entry((b.profile, b.hash)).or_insert_with(|| Arc::new(disassembly(b))).clone()
    }
}

pub fn shader_page(p: &mut egui::Ui, v: &RsxFrameView, f: &FrameView, names: &Names, cache: &mut Cache) {
    let rows = cache.rows(v, f);
    if rows.is_empty() {
        return ui::field_message(p, ui::Tone::Neutral, "No programs were recorded in this capture.");
    }
    p.label(format!(
        "{} programs · times are this view's {} intervals (total and mean over timed draws).",
        rows.len(),
        if f.isolated { "isolated" } else { "pipelined" }
    ));
    for r in rows.iter() {
        let prog = &f.programs[r.index];
        let header = format!("{} · {} · {} draws", program_label(prog, names), prog.id_text(), r.draws);
        egui::CollapsingHeader::new(header).id_salt(("rsxf-shader", f.id, f.half == ps3dk_prof::rsx_frame::Half::Second, r.index)).show(p, |p| {
            p.label(format!("{} · source {} · {} instructions", match prog.profile { Profile::Vertex => "Vertex program", Profile::Fragment => "Fragment program" }, prog.source.name(), prog.instructions));
            if let Some((what, why)) = provenance(prog) {
                ui::field_message(p, ui::Tone::Warning, &format!("{what}: {why}"));
            } else {
                ui::field_message(p, ui::Tone::Success, "Identity exact (hashed at bind while the capture was armed, or from the command stream).");
            }
            p.horizontal_wrapped(|p| {
                p.label("Total");
                let t = v.duration((r.timed > 0).then_some(r.sum));
                p.label(t.text).on_hover_text(t.hover);
                p.label("· mean");
                let m = v.duration(r.mean());
                p.label(m.text).on_hover_text(m.hover);
                p.label(format!("· {} of {} draws timed", r.timed, r.draws));
            });
            if r.varying.is_empty() {
                p.label("No constant changes across its draws.");
            } else {
                p.label(format!("Constants that change across its draws ({}): {}", r.varying.len(), r.varying.join(", ")));
            }
            match f.bytes_of(r.index) {
                None => ui::field_message(p, ui::Tone::Neutral, &bytes_missing(f, r.index, v.fp_bytes_cap)),
                Some(b) => {
                    let lines = cache.disasm(b);
                    egui::ScrollArea::vertical().id_salt(("rsxf-disasm", f.id, r.index)).max_height(320.0).show(p, |p| {
                        let pal = ui::Palette::of(p);
                        for l in lines.iter() {
                            let text = format!("{:04x}  {}", l.offset, l.text);
                            match l.kind {
                                LineKind::Instruction => p.monospace(text),
                                LineKind::Constant => p.label(RichText::new(text).monospace().weak()),
                                LineKind::Unknown => p.label(RichText::new(text).monospace().color(ui::Tone::Warning.color(&pal)))
                                    .on_hover_text("Encoding not known: shown as raw words."),
                                LineKind::Note => p.label(RichText::new(text).italics().color(ui::Tone::Warning.color(&pal))),
                            };
                        }
                    });
                }
            }
        });
    }
}
