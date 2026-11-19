//! AUTO draw captures. All timing, grouping and identities come from the reader.
use crate::rsx_frame_view::{sorted_draws, DrawColumn, Names, RsxFrameView};
use eframe::egui::{self, RichText};
use ps3dk_prof::rsx_frame::{self as rf, FrameStatus, FrameView, GroupBy};
use ps3dk_ui as ui;
use std::sync::Arc;

#[derive(PartialEq, Eq)]
struct RowKey {
    generation: u64,
    capture: Option<u32>,
    filter: String,
    column: DrawColumn,
    descending: bool,
    group: Option<GroupBy>,
    scan: u64,
}
#[derive(Default)]
pub struct Rows {
    pub ids: Vec<usize>,
    pub groups: Vec<rf::Group>,
}
pub fn badge(status: &FrameStatus) -> &'static str {
    match status {
        FrameStatus::Complete => "Ready",
        FrameStatus::Open => "Open",
        FrameStatus::PendingTiming { .. } => "Pending timing",
        _ => "Partial",
    }
}

#[derive(Clone, Copy, PartialEq, Eq, Default)]
pub enum Tab {
    #[default]
    Summary,
    Shaders,
    State,
    Textures,
    Buffers,
    Constants,
}
pub struct State {
    pub selected: Option<u32>,
    pub draw: Option<u32>,
    pub filter: String,
    pub column: DrawColumn,
    pub descending: bool,
    pub tab: Tab,
    pub group: Option<GroupBy>,
    pub legacy: bool,
    pub focus: bool,
    /// The capture switched to when the live link ended, and why its timing is incomplete.
    pub link_ended: Option<(u32, String)>,
    rows: Option<(RowKey, Arc<Rows>)>,
    consumed: Option<(u32, u32)>,
}
impl Default for State {
    fn default() -> Self {
        Self {
            selected: None,
            draw: None,
            filter: String::new(),
            column: DrawColumn::Time,
            descending: true,
            tab: Tab::Summary,
            group: None,
            legacy: false,
            focus: false,
            link_ended: None,
            rows: None,
            consumed: None,
        }
    }
}
impl State {
    /// Sorted and grouped draws, rebuilt only when the immutable view, the selection,
    /// filter, sort, grouping or shader-name scan (`scan`, its generation) changes.
    pub fn rows(&mut self, v: &RsxFrameView, names: &Names, scan: u64) -> Arc<Rows> {
        if let Some((k, rows)) = &self.rows {
            if k.generation == v.generation && k.capture == self.selected && k.filter == self.filter
                && k.column == self.column && k.descending == self.descending && k.group == self.group && k.scan == scan {
                return rows.clone();
            }
        }
        let rows = Arc::new(v.frames.iter().find(|f|Some(f.id)==self.selected).map_or_else(Rows::default, |f| Rows {
            ids: sorted_draws(f, &self.filter, self.column, self.descending, names),
            groups: self.group.map_or_else(Vec::new, |by|rf::group(f, by, names)),
        }));
        self.rows = Some((RowKey {generation:v.generation, capture:self.selected, filter:self.filter.clone(), column:self.column, descending:self.descending, group:self.group, scan}, rows.clone()));
        rows
    }
    /// The link-ended reason, shown only while that capture is selected.
    pub fn link_ended_reason(&self) -> Option<&str> {
        self.link_ended.as_ref().filter(|(id, _)| self.selected == Some(*id)).map(|(_, why)| why.as_str())
    }
    pub fn select(&mut self, f: &FrameView) {
        self.rows = None;
        self.selected = Some(f.id);
        self.column = DrawColumn::Time;
        self.descending = true;
        self.filter.clear();
        self.draw = sorted_draws(f, "", self.column, true, &Names::new())
            .first()
            .map(|&i| f.draws[i].raw.ordinal);
        self.focus = true;
        self.legacy = false;
    }
    pub fn follow(&mut self, t: &crate::live_rsx_frame::RequestTracker, v: &RsxFrameView) -> bool {
        let Some(ack) = t
            .ack
            .filter(|a| a.result == 0 && t.request == Some(a.request))
        else {
            return false;
        };
        if self.consumed == Some((ack.request, ack.capture)) {
            return false;
        }
        let Some(f) = v.frames.iter().find(|f| {
            f.id == ack.capture
                && f.request_id == ack.request
                && (t.ended || matches!(
                    f.status,
                    FrameStatus::Complete
                        | FrameStatus::Truncated { .. }
                        | FrameStatus::Abandoned { .. }
                ))
        }) else {
            return false;
        };
        self.select(f);
        if t.ended && matches!(f.status, FrameStatus::Open | FrameStatus::PendingTiming { .. }) {
            self.link_ended = Some((f.id, "Live link ended; this received capture has incomplete timing".into()));
        }
        self.consumed = Some((ack.request, ack.capture));
        true
    }
}
#[derive(Default)]
pub struct Drawn {
    pub rows: usize,
    pub focused: Option<u32>,
    pub status: Option<String>,
    pub save: Option<(u32, bool)>,
    pub shader_folder: bool,
}
fn duration(p: &mut egui::Ui, v: &RsxFrameView, n: Option<u64>) {
    let d = v.duration(n);
    p.label(d.text).on_hover_text(d.hover);
}
fn program(f: &FrameView, i: Option<usize>, names: &Names) -> String {
    i.and_then(|i| f.programs.get(i))
        .map_or_else(|| "unknown".into(), |p| rf::program_name(p, names))
}
pub fn page(p: &mut egui::Ui, v: &RsxFrameView, names: &Names, scan: u64, st: &mut State, out: &mut Drawn) {
    *out = Drawn::default();
    ui::page_header(
        p,
        "RSX frames",
        "Inspect the recorded draw calls, their timing and shader identities.",
    );
    if v.frames.is_empty() {
        ui::empty_state(
            p,
            "No RSX frame captures",
            if v.available {
                "Capture RSX frame from the live strip when the app is running. This file contains no retained request."
            } else {
                "Build with PS3_PROF_RSX_FRAME and an AUTO report ring to request a frame. Older EXT captures remain available separately."
            },
        );
        return;
    }
    if st.selected.is_none() && v.frames.len() == 1 {
        st.select(&v.frames[0]);
    }
    p.horizontal_wrapped(|p| {
        egui::ComboBox::from_id_salt("rsxf-capture")
            .selected_text(
                st.selected
                    .map_or("Choose capture".into(), |id| format!("Capture {id}")),
            )
            .show_ui(p, |p| {
                for f in &v.frames {
                    if p.selectable_label(
                        st.selected == Some(f.id),
                        format!(
                            "#{} {} · frame {} · {}",
                            f.id,
                            f.name,
                            f.frame,
                            f.status.text()
                        ),
                    )
                    .clicked()
                    {
                        st.select(f);
                    }
                }
            });
        if let Some(id) = st.selected {
            if p.button("Save").clicked() {
                out.save = Some((id, false));
            }
            if p.button("Save as…").clicked() {
                out.save = Some((id, true));
            }
        }
        if p.button("Shader folder…")
            .on_hover_text("Match local .vpo/.fpo files by their stable program hash; session only")
            .clicked()
        {
            out.shader_folder = true;
        }
    });
    let Some(f) = st
        .selected
        .and_then(|id| v.frames.iter().find(|f| f.id == id))
    else {
        p.label("Choose a capture to inspect its draws.");
        return;
    };
    egui::ScrollArea::vertical().id_salt("rsxf-page").show(p,|p|{
   let status=f.status.text();out.status=Some(status.clone());
   p.horizontal_wrapped(|p|{
     ui::badge(p,badge(&f.status),if matches!(f.status,FrameStatus::Complete){ui::Tone::Success}else{ui::Tone::Warning}).on_hover_text(&status);
     p.label(format!("{} · owner {} · frame {} · window {}",f.name,f.owner,f.frame,f.window));p.label(&status);
   });
   if let Some(why)=st.link_ended_reason() {ui::field_message(p,ui::Tone::Warning,why);}
   p.label(if f.isolated{"ISOLATED timing"}else{"PIPELINED: reached-marker intervals can overlap. Their sum is not isolated execution cost."});
   p.horizontal_wrapped(|p|{
     p.label(format!("{} draws · {} timed · {} untimed · {} missing · {} primitives",f.totals.draws,f.totals.timed,f.totals.untimed,f.totals.missing,f.totals.primitives));
     p.label("Sum");duration(p,v,Some(f.totals.sum));p.label("Span");duration(p,v,f.totals.span.map(|(b,e)|e-b));p.label("Gaps");duration(p,v,Some(f.totals.gaps));
   });
   if f.draws_not_retained>0 {ui::field_message(p,ui::Tone::Warning,&format!("{} draw records were not retained by the reader",f.draws_not_retained));}
   if let Some(e)=&f.end {p.collapsing("Capture evidence",|p|{
     p.label(format!("{} seen / {} recorded; {} hooked / {} decoded only / {} mismatches",e.draws_seen,e.draws_recorded,e.hooked,e.decoded_only,e.mismatches));
     p.label(format!("{} blocks / {} dropped; {} bytes streamed / {} dropped; labels refused {} / programs refused {}",e.blocks,e.blocks_dropped,e.bytes_streamed,e.bytes_dropped,e.labels_refused,e.programs_refused));
     if let Some((h,r))=e.decoder_reject{p.label(format!("Decoder rejected header {h:#010x}, reason {r}"));}
   });}
   if !f.passes.is_empty(){p.collapsing("Pass intervals",|p|{for pass in &f.passes{p.horizontal_wrapped(|p|{p.label(format!("{} · depth {} · {} draws",pass.name,pass.depth,pass.draws));duration(p,v,Some(pass.raw_end-pass.raw_begin));p.label(format!("{}..{} raw ticks",pass.raw_begin,pass.raw_end));});}});}
   p.separator();
   p.horizontal_wrapped(|p|{
     p.label("Filter");p.add(egui::TextEdit::singleline(&mut st.filter).hint_text("Label, shader, kind or ordinal").desired_width(250.0));
     egui::ComboBox::from_id_salt("rsxf-group").selected_text(st.group.map_or("No grouping",group_name)).show_ui(p,|p|{
       p.selectable_value(&mut st.group,None,"No grouping");for by in [GroupBy::Program,GroupBy::Vp,GroupBy::Fp,GroupBy::Label,GroupBy::Tag,GroupBy::Pass,GroupBy::Kind]{p.selectable_value(&mut st.group,Some(by),group_name(by));}
     });
   });
   p.horizontal_wrapped(|p|{p.label("Sort");for (column,name) in [(DrawColumn::Time,"Time"),(DrawColumn::Ordinal,"Ordinal"),(DrawColumn::Kind,"Kind"),(DrawColumn::Count,"Count"),(DrawColumn::Gap,"Gap")]{if p.selectable_label(st.column==column,name).clicked(){if st.column==column{st.descending=!st.descending}else{st.column=column;st.descending=column!=DrawColumn::Ordinal;}}}p.label(if st.descending{"Descending"}else{"Ascending"});});
   let cached=st.rows(v,names,scan);let ids=&cached.ids;
   if st.group.is_some() {p.collapsing("Group totals (all draws)",|p|{let groups=&cached.groups;egui::ScrollArea::vertical().id_salt("rsxf-groups").max_height(140.0).show_rows(p,20.0,groups.len(),|p,range|{for i in range {let g=&groups[i];p.horizontal(|p|{p.label(&g.key);p.label(format!("{} timed / {} untimed",g.timed,g.untimed));duration(p,v,Some(g.sum));});}});});}
   if ids.is_empty(){p.label(if f.draws.is_empty(){"No draw records retained in this capture."}else{"No draws match the filter."});}
   draw_table(p,v,f,names,st,out,&ids);
   p.separator();
   let Some(d)=st.draw.and_then(|n|f.draws.iter().find(|d|d.raw.ordinal==n))else{return};
   p.horizontal_wrapped(|p|{for (tab,name) in [(Tab::Summary,"Summary"),(Tab::Shaders,"Shaders"),(Tab::State,"State"),(Tab::Textures,"Textures"),(Tab::Buffers,"Buffers"),(Tab::Constants,"Constants")]{p.selectable_value(&mut st.tab,tab,name);}});
   match st.tab {
     Tab::Summary=>{
       p.label(RichText::new(format!("Draw #{} · {}",d.raw.ordinal,d.raw.kind.name())).strong());
       p.label(d.label.as_deref().unwrap_or("No app-supplied draw label"));
       p.horizontal_wrapped(|p|{duration(p,v,d.timing.interval());match d.timing{rf::Timing::Timed{raw_begin,raw_end}=>{p.label(format!("Reports {raw_begin}..{raw_end} raw ticks"));},_=>{p.label(d.timing.text());}}p.label("Gap");duration(p,v,d.gap);});
       p.label(format!("{} · first {} · count {} · primitives {} · tag {:#x}",rf::primitive_name(d.raw.mode),d.raw.first,d.raw.count,d.primitives().map_or("unavailable".into(),|n|n.to_string()),d.raw.tag));
       if matches!(d.raw.kind,rf::DrawKind::Index|rf::DrawKind::InlineIndex){p.label(format!("Index offset {:#x} · type {} · location {}",d.raw.index_offset,d.raw.index_type,d.raw.index_location));}
       if d.raw.kind==rf::DrawKind::Clear{p.label(format!("Clear mask {:#x}",d.raw.clear_mask));}
       p.label(format!("{}{}{}{}",if d.hooked(){"Hooked "}else{""},if d.decoded(){"Decoded "}else{""},if d.raw.flags&rf::DF_STATE_UNKNOWN!=0{"State unknown "}else{""},if d.op_mismatch{"AUTO kind mismatch"}else{""}));
       p.label(format!("Caller {:#x} · {} command words · PPU tick {}",d.raw.caller,d.raw.words,d.raw.tb));
     },
     Tab::Shaders=>{for (title,id) in [("Vertex program",d.vp),("Fragment program",d.fp)]{
       ui::section_label(p,title);if let Some(prog)=id.and_then(|i|f.programs.get(i)){
         p.label(rf::program_name(prog,names));p.monospace(prog.id_text());p.label(format!("{} instructions · {} bytes · {} inline constant blocks · source {}",prog.instructions,prog.ucode_bytes,prog.const_blocks,prog.source.name()));
         p.label(format!("Address {:#x} (diagnostic only) · first draw {}",prog.address,prog.first_draw));
         if prog.unterminated||prog.unreadable{ui::field_message(p,ui::Tone::Warning,&format!("Unterminated: {} · unreadable: {}",prog.unterminated,prog.unreadable));}
       }else{p.label("Program identity unavailable for this draw.");}
     }},
     _=>{ui::field_message(p,ui::Tone::Neutral,"Not recorded by RSX frame schema 1. This capture contains draw arguments and program identities; these state values are unavailable.");},
   }
 });
}
fn draw_table(
    p: &mut egui::Ui,
    v: &RsxFrameView,
    f: &FrameView,
    names: &Names,
    st: &mut State,
    out: &mut Drawn,
    ids: &[usize],
) {
    const ROW: f32 = 28.0;
    egui::ScrollArea::horizontal()
        .id_salt("rsxf-table-x")
        .show(p, |p| {
            p.set_min_width(760.0);
            p.spacing_mut().item_spacing = egui::vec2(8.0, 4.0);
            let label_width = (p.available_width() - 685.0).max(75.0);
            let cols = [130.0, 135.0, 120.0, label_width, 130.0, 130.0];
            p.horizontal(|p| {
                for (name, width) in [
                    "Draw / kind",
                    "Time",
                    "Arguments",
                    "Label",
                    "Vertex program",
                    "Fragment program",
                ]
                .into_iter()
                .zip(cols)
                {
                    p.add_sized(
                        [width, ROW],
                        egui::Label::new(RichText::new(name).strong()).truncate(),
                    );
                }
            });
            let mut list = egui::ScrollArea::vertical()
                .id_salt(("rsxf-draws", f.id))
                .max_height(200.0);
            if st.focus {
                if let Some(pos) = ids
                    .iter()
                    .position(|&i| Some(f.draws[i].raw.ordinal) == st.draw)
                {
                    list = list
                        .vertical_scroll_offset(pos as f32 * (ROW + p.spacing().item_spacing.y));
                }
            }
            list.show_rows(p, ROW, ids.len(), |p, range| {
                for i in range {
                    let d = &f.draws[ids[i]];
                    out.rows += 1;
                    p.horizontal(|p| {
                        let r = p.add_sized(
                            [cols[0], ROW],
                            egui::SelectableLabel::new(
                                st.draw == Some(d.raw.ordinal),
                                format!("#{} {}", d.raw.ordinal, d.raw.kind.name()),
                            ),
                        );
                        if st.focus && st.draw == Some(d.raw.ordinal) {
                            r.request_focus();
                            st.focus = false;
                        }
                        if r.has_focus() {
                            out.focused = Some(d.raw.ordinal);
                        }
                        if r.clicked() {
                            st.draw = Some(d.raw.ordinal);
                        }
                        let time = v.duration(d.timing.interval());
                        let args = if d.raw.kind == rf::DrawKind::Clear {
                            format!("mask {:#x}", d.raw.clear_mask)
                        } else {
                            format!("{} · {}", rf::primitive_name(d.raw.mode), d.raw.count)
                        };
                        let cells = [
                            (time.text, format!("{}\n{}", time.hover, d.timing.text())),
                            (args.clone(), args),
                            (
                                d.label.clone().unwrap_or("(no label)".into()),
                                d.label.clone().unwrap_or_default(),
                            ),
                            (program(f, d.vp, names), program(f, d.vp, names)),
                            (program(f, d.fp, names), program(f, d.fp, names)),
                        ];
                        for ((text, hover), width) in cells.into_iter().zip(cols[1..].iter()) {
                            p.add_sized([*width, ROW], egui::Label::new(text).truncate())
                                .on_hover_text(hover);
                        }
                    });
                }
            });
        });
}
fn group_name(g: GroupBy) -> &'static str {
    match g {
        GroupBy::Program => "VP + FP",
        GroupBy::Vp => "Vertex program",
        GroupBy::Fp => "Fragment program",
        GroupBy::Label => "Label",
        GroupBy::Tag => "Tag",
        GroupBy::Pass => "Pass",
        GroupBy::Kind => "Kind",
    }
}
