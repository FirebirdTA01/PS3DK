//! AUTO draw captures. All timing, grouping and identities come from the reader.
use crate::rsx_frame_view::{program_label, provenance, sorted_draws, DrawColumn, Names, RsxFrameView};
use eframe::egui::{self, RichText};
use ps3dk_prof::rsx_frame::{self as rf, FrameStatus, FrameView, GroupBy, Half};
use ps3dk_ui as ui;
use std::sync::Arc;

/// What the RSX frames page shows for the selected view.
#[derive(Clone, Copy, PartialEq, Eq, Default, Debug)]
pub enum View {
    #[default]
    Draws,
    /// Every program with time, varying constants and disassembly.
    Shaders,
    /// A Both capture's per-draw pipelined vs isolated pairing.
    Pairs,
}

#[derive(PartialEq, Eq)]
struct RowKey {
    generation: u64,
    capture: Option<u32>,
    half: Half,
    filter: String,
    column: DrawColumn,
    descending: bool,
    group: Option<GroupBy>,
    by_target: bool,
    expanded: std::collections::BTreeSet<usize>,
    scan: u64,
}
#[derive(Default)]
pub struct Rows {
    pub ids: Vec<usize>,
    /// `ids` with instance runs collapsed into one row (expanded runs list their draws).
    pub display: Vec<crate::rsx_constants_ui::Row>,
    pub groups: Vec<rf::Group>,
}
/// Header warnings with their reasons: mismatched draws, lost decoder events, replaced text.
pub fn header_notes(f: &FrameView) -> Vec<(ui::Tone, String)> {
    let mut out = Vec::new();
    if f.totals.mismatched > 0 {
        out.push((ui::Tone::Warning, format!(
            "{} mismatched draws: their AUTO operation belongs to another draw kind or frame, so they are in no timed count or sum",
            f.totals.mismatched)));
    }
    if let Some(n) = f.end.as_ref().map(|e| e.lost_events).filter(|&n| n > 0) {
        out.push((ui::Tone::Warning, format!(
            "{n} draw events lost: the stream decoder could not hand them to the capture, so those draws are absent")));
    }
    if f.text_replaced > 0 {
        out.push((ui::Tone::Warning, format!(
            "Text replaced: {} name, label or program-name fields were not valid UTF-8 and show U+FFFD; the capture is otherwise intact",
            f.text_replaced)));
    }
    out
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
    /// Which view of the selected capture (a Both capture has a pipelined and an isolated one).
    pub half: Half,
    pub view: View,
    pub draw: Option<u32>,
    pub filter: String,
    pub column: DrawColumn,
    pub descending: bool,
    pub tab: Tab,
    pub group: Option<GroupBy>,
    /// Group by render target (viewer-side, from the register state); exclusive with `group`.
    pub by_target: bool,
    /// Instance runs (indices into the capture's `instance_runs`) shown expanded in the draw list.
    pub expanded: std::collections::BTreeSet<usize>,
    pub legacy: bool,
    pub focus: bool,
    /// The capture switched to when the live link ended, and why its timing is incomplete.
    pub link_ended: Option<(u32, String)>,
    rows: Option<(RowKey, Arc<Rows>)>,
    consumed: Option<(u32, u32)>,
    state_cache: crate::rsx_state_ui::Cache,
    shaders: crate::rsx_shader_ui::Cache,
}
impl Default for State {
    fn default() -> Self {
        Self {
            selected: None,
            half: Half::Only,
            view: View::Draws,
            shaders: Default::default(),
            draw: None,
            filter: String::new(),
            column: DrawColumn::Time,
            descending: true,
            tab: Tab::Summary,
            group: None,
            by_target: false,
            expanded: Default::default(),
            legacy: false,
            focus: false,
            link_ended: None,
            rows: None,
            consumed: None,
            state_cache: Default::default(),
        }
    }
}
impl State {
    /// The selected view: the capture `selected` and its `half`.
    pub fn current<'a>(&self, v: &'a RsxFrameView) -> Option<&'a FrameView> {
        let id = self.selected?;
        v.frames.iter().find(|f| f.id == id && f.half == self.half).or_else(|| v.frames.iter().find(|f| f.id == id))
    }
    /// Sorted and grouped draws, rebuilt only when the immutable view, the selection,
    /// filter, sort, grouping or shader-name scan (`scan`, its generation) changes.
    pub fn rows(&mut self, v: &RsxFrameView, names: &Names, scan: u64) -> Arc<Rows> {
        if let Some((k, rows)) = &self.rows {
            if k.generation == v.generation && k.capture == self.selected && k.half == self.half && k.filter == self.filter
                && k.column == self.column && k.descending == self.descending && k.group == self.group && k.by_target == self.by_target && k.expanded == self.expanded && k.scan == scan {
                return rows.clone();
            }
        }
        let rows = Arc::new(self.current(v).map_or_else(Rows::default, |f| {
            let ids = sorted_draws(f, &self.filter, self.column, self.descending, names);
            Rows {
            display: crate::rsx_constants_ui::display_rows(f, &ids, &self.expanded, (self.column==DrawColumn::Time).then_some(self.descending)),
            ids,
            groups: if self.by_target { crate::rsx_state_ui::target_groups(f) } else { self.group.map_or_else(Vec::new, |by|rf::group(f, by, names)) },
        }}));
        self.rows = Some((RowKey {generation:v.generation, capture:self.selected, half:self.half, filter:self.filter.clone(), column:self.column, descending:self.descending, group:self.group, by_target:self.by_target, expanded:self.expanded.clone(), scan}, rows.clone()));
        rows
    }
    /// The link-ended reason, shown only while that capture is selected.
    pub fn link_ended_reason(&self) -> Option<&str> {
        self.link_ended.as_ref().filter(|(id, _)| self.selected == Some(*id)).map(|(_, why)| why.as_str())
    }
    pub fn select(&mut self, f: &FrameView) {
        self.rows = None;
        self.expanded.clear();
        self.selected = Some(f.id);
        self.half = f.half;
        if self.view == View::Pairs && f.half == Half::Only {
            self.view = View::Draws;
        }
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
fn program(f: &FrameView, i: Option<usize>, names: &Names) -> (String, String) {
    i.and_then(|i| f.programs.get(i)).map_or_else(
        || ("unknown".into(), "unknown".into()),
        |p| {
            let text = program_label(p, names);
            let hover = match provenance(p) {
                Some((_, why)) => format!("{text}\n{}: {why}", p.id_text()),
                None => format!("{text}\n{}", p.id_text()),
            };
            (text, hover)
        },
    )
}
pub fn page(p: &mut egui::Ui, v: &RsxFrameView, names: &Names, fetches: &crate::shader_names::Fetches, scan: u64,
            st: &mut State, out: &mut Drawn) {
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
                st.selected.map_or("Choose capture".into(), |id| {
                    format!("Capture {id}{}", crate::rsx_modes_ui::half_suffix(st.half))
                }),
            )
            .show_ui(p, |p| {
                for f in &v.frames {
                    if p.selectable_label(
                        st.selected == Some(f.id) && st.half == f.half,
                        format!(
                            "#{} {} · frame {} · {}{}",
                            f.id,
                            f.name,
                            f.frame,
                            f.status.text(),
                            crate::rsx_modes_ui::half_suffix(f.half)
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
            .on_hover_text("Match local .vpo/.fpo files by their stable program hash, naming programs and counting their texture fetches; session only")
            .clicked()
        {
            out.shader_folder = true;
        }
    });
    let Some(f) = st.current(v) else {
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
   p.horizontal_wrapped(|p|{
     ui::badge(p,&crate::rsx_modes_ui::mode_label(f),ui::Tone::Accent);
     p.label(if f.isolated{"ISOLATED: the RSX waited for idle around each draw, so each interval is that draw's own cost."}else{"PIPELINED: reached-marker intervals can overlap. Their sum is not isolated execution cost."});
   });
   if f.half!=Half::Only {ui::field_message(p,ui::Tone::Neutral,"A Both capture has two views, each with its own timing mode and totals; they are never added together. Pairs compares them per draw.");}
   p.horizontal_wrapped(|p|{
     p.label(format!("{} draws · {} timed · {} untimed · {} missing · {} mismatched · {} primitives",f.totals.draws,f.totals.timed,f.totals.untimed,f.totals.missing,f.totals.mismatched,f.totals.primitives));
     p.label("Sum");duration(p,v,Some(f.totals.sum));p.label("Span");duration(p,v,f.totals.span.map(|(b,e)|e-b));p.label("Gaps");duration(p,v,Some(f.totals.gaps));
   });
   if f.draws_not_retained>0 {ui::field_message(p,ui::Tone::Warning,&format!("{} draw records were not retained by the reader",f.draws_not_retained));}
   if f.decoder>=5 {
     let targets=rf::overdraw(f);
     if !targets.is_empty() {p.collapsing(format!("Pixels and overdraw{}",if f.pixels_forced{" · counting forced"}else{""}),|p|{
       ui::field_message(p,ui::Tone::Neutral,PIXELS_NOTE);
       for t in &targets {p.label(format!("{} · {} draws, {} counted · {} samples{}",t.key,t.draws,t.counted,t.samples,t.overdraw.map_or_else(String::new,|o|format!(" · overdraw {o:.2}"))));}
     });}
   }
   for (tone,why) in header_notes(f) {ui::field_message(p,tone,&why);}
   if let Some(e)=&f.end {p.collapsing("Capture evidence",|p|{
     p.label(format!("{} seen / {} recorded; {} hooked / {} decoded only / {} mismatches; {} lost events",e.draws_seen,e.draws_recorded,e.hooked,e.decoded_only,e.mismatches,e.lost_events));
     p.label(format!("{} blocks / {} dropped; {} bytes streamed / {} dropped; labels refused {} / programs refused {}",e.blocks,e.blocks_dropped,e.bytes_streamed,e.bytes_dropped,e.labels_refused,e.programs_refused));
     if let Some((h,r))=e.decoder_reject{p.label(format!("Decoder rejected header {h:#010x}, reason {r}"));}
   });}
   if !f.passes.is_empty(){p.collapsing("Pass intervals",|p|{for pass in &f.passes{p.horizontal_wrapped(|p|{p.label(format!("{} · depth {} · {} draws",pass.name,pass.depth,pass.draws));duration(p,v,Some(pass.raw_end-pass.raw_begin));p.label(format!("{}..{} raw ticks",pass.raw_begin,pass.raw_end));});}});}
   p.separator();
   p.horizontal_wrapped(|p|{
     p.selectable_value(&mut st.view,View::Draws,"Draws");
     p.selectable_value(&mut st.view,View::Shaders,"Shader view");
     if f.half!=Half::Only {p.selectable_value(&mut st.view,View::Pairs,"Pairs (pipelined vs isolated)");}
   });
   match st.view {
     View::Shaders=>{crate::rsx_shader_ui::shader_page(p,v,f,names,fetches,&mut st.shaders);return;},
     View::Pairs=>{crate::rsx_modes_ui::pairs_page(p,v,f.id);return;},
     View::Draws=>{},
   }
   p.horizontal_wrapped(|p|{
     p.label("Filter");p.add(egui::TextEdit::singleline(&mut st.filter).hint_text("Label, shader, kind or ordinal").desired_width(250.0));
     egui::ComboBox::from_id_salt("rsxf-group").selected_text(if st.by_target{"Render target"}else{st.group.map_or("No grouping",group_name)}).show_ui(p,|p|{
       if p.selectable_label(st.group.is_none()&&!st.by_target,"No grouping").clicked(){st.group=None;st.by_target=false;}
       for by in [GroupBy::Program,GroupBy::Vp,GroupBy::Fp,GroupBy::Label,GroupBy::Tag,GroupBy::Pass,GroupBy::Kind]{if p.selectable_label(!st.by_target&&st.group==Some(by),group_name(by)).clicked(){st.group=Some(by);st.by_target=false;}}
       if p.selectable_label(st.by_target,"Render target").clicked(){st.group=None;st.by_target=true;}
     });
   });
   p.horizontal_wrapped(|p|{p.label("Sort");for (column,name) in [(DrawColumn::Time,"Time"),(DrawColumn::Ordinal,"Ordinal"),(DrawColumn::Kind,"Kind"),(DrawColumn::Count,"Count"),(DrawColumn::Gap,"Gap"),(DrawColumn::Samples,"Samples")]{if p.selectable_label(st.column==column,name).clicked(){if st.column==column{st.descending=!st.descending}else{st.column=column;st.descending=column!=DrawColumn::Ordinal;}}}p.label(if st.descending{"Descending"}else{"Ascending"});});
   let cached=st.rows(v,names,scan);let ids=&cached.ids;
   if st.group.is_some()||st.by_target {p.collapsing("Group totals (all draws)",|p|{let groups=&cached.groups;egui::ScrollArea::vertical().id_salt("rsxf-groups").max_height(140.0).show_rows(p,20.0,groups.len(),|p,range|{for i in range {let g=&groups[i];p.horizontal(|p|{p.label(&g.key);p.label(format!("{} timed / {} untimed",g.timed,g.untimed));duration(p,v,Some(g.sum));});}});});}
   if ids.is_empty(){p.label(if f.draws.is_empty(){"No draw records retained in this capture."}else{"No draws match the filter."});}
   draw_table(p,v,f,names,st,out,&cached.display);
   p.separator();
   let Some(di)=st.draw.and_then(|n|f.draws.iter().position(|d|d.raw.ordinal==n))else{return};let d=&f.draws[di];
   let sx=crate::rsx_state_ui::Ctx{f,index:di};
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
       let df=rf::draw_fetches(f,d,fetches);
       p.label(format!("Texture fetches {} · VP {} · FP {}",df.text(),rf::fetch_text(df.vp),rf::fetch_text(df.fp))).on_hover_text(crate::rsx_shader_ui::FETCH_HOVER);
       let px=rf::pixels(f,d,fetches);
       match px.samples {
         Some(s)=>{p.label(format!("Pixels: {s} passed samples · counting {}{}{}",px.state.name(),px.per_pixel.map_or_else(String::new,|v|format!(" · {v:.3} of the target")),px.raw_per_sample.map_or_else(String::new,|r|format!(" · {r:.2} raw ticks per sample")))).on_hover_text(PIXELS_NOTE);
           if d.raw.kind!=rf::DrawKind::Clear {ui::field_message(p,if px.early_z==rf::EarlyZ::Disabled{ui::Tone::Warning}else{ui::Tone::Neutral},&format!("Early-z {}: {}",px.early_z.name(),px.early_z.shading()));}
           if let Some(w)=px.shading_proxy {p.label(format!("{w} sample-instructions{}",px.fetch_proxy.map_or_else(String::new,|x|format!(" · {x} sample-fetches"))));}}
         None=>{p.label(format!("Pixels: not counted · {}",px.state.why_uncounted().unwrap_or("the draw was not timed")));}
       }
       let z=rf::zcull_at(f,d);
       p.label(format!("Zcull: {}{}{}{}",z.reason,z.region.map_or_else(String::new,|r|format!(" (region {})",r.index)),if z.invalidated{" · invalidated before this draw"}else{""},if z.cleared{" · zcull surface cleared before this draw"}else{""}));
       if z.direction_mismatch==Some(true){ui::field_message(p,ui::Tone::Warning,"Zcull direction disagrees with the depth function (heuristic): zcull may reject nothing.");}
       crate::rsx_state_ui::changed_list(p,&sx);
     },
     Tab::Shaders=>{for (title,id) in [("Vertex program",d.vp),("Fragment program",d.fp)]{
       ui::section_label(p,title);if let Some(prog)=id.and_then(|i|f.programs.get(i)){
         p.label(program_label(prog,names));p.monospace(prog.id_text());
         if let Some((what,why))=provenance(prog){ui::field_message(p,ui::Tone::Warning,&format!("{}{}: {why}",what[..1].to_uppercase(),&what[1..]));}p.label(format!("{} instructions · {} bytes · {} inline constant blocks · source {}",prog.instructions,prog.ucode_bytes,prog.const_blocks,prog.source.name()));
         p.label(format!("Address {:#x} (diagnostic only) · first draw {}",prog.address,prog.first_draw));
         p.label(format!("Texture fetches: {}",rf::fetch_text(id.and_then(|i|rf::program_fetches(f,i,fetches))))).on_hover_text(crate::rsx_shader_ui::FETCH_HOVER);
         if prog.unterminated{ui::field_message(p,ui::Tone::Warning,"Unterminated: no end bit within the bound");}
       }else{p.label("Program identity unavailable for this draw.");}
     }},
     Tab::State=>crate::rsx_state_ui::state_tab(p,&sx,&mut st.state_cache),
     Tab::Textures=>crate::rsx_state_ui::textures_tab(p,&sx,&mut st.state_cache,rf::draw_fetches(f,d,fetches).fp.map(|(t,_)|t).as_ref()),
     Tab::Buffers=>crate::rsx_state_ui::buffers_tab(p,&sx,&mut st.state_cache),
     Tab::Constants=>crate::rsx_constants_ui::constants_tab(p,f,di,v),
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
    rows: &[crate::rsx_constants_ui::Row],
) {
    use crate::rsx_constants_ui::{self as rc, Row};
    const ROW: f32 = 28.0;
    egui::ScrollArea::horizontal()
        .id_salt("rsxf-table-x")
        .show(p, |p| {
            p.set_min_width(860.0);
            p.spacing_mut().item_spacing = egui::vec2(8.0, 4.0);
            let label_width = (p.available_width() - 785.0).max(75.0);
            let cols = [130.0, 135.0, 120.0, label_width, 130.0, 130.0, 92.0];
            p.horizontal(|p| {
                for (name, width) in [
                    "Draw / kind",
                    "Time",
                    "Arguments",
                    "Label",
                    "Vertex program",
                    "Fragment program",
                    "Samples",
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
                .id_salt(("rsxf-draws", f.id, f.half == Half::Second))
                .max_height(200.0);
            if st.focus {
                if let Some(pos) = rows
                    .iter()
                    .position(|&r| rc::row_draws(f, r).any(|i| Some(f.draws[i].raw.ordinal) == st.draw))
                {
                    list = list
                        .vertical_scroll_offset(pos as f32 * (ROW + p.spacing().item_spacing.y));
                }
            }
            list.show_rows(p, ROW, rows.len(), |p, range| {
                for i in range {
                    let row = rows[i];
                    let span = rc::row_draws(f, row);
                    let d = &f.draws[span.start];
                    let holds = span.clone().any(|j| Some(f.draws[j].raw.ordinal) == st.draw);
                    out.rows += 1;
                    p.horizontal(|p| {
                        let text = match row {
                            Row::Draw(_) => format!("#{} {}", d.raw.ordinal, d.raw.kind.name()),
                            Row::Member(_) => format!("   #{} {}", d.raw.ordinal, d.raw.kind.name()),
                            Row::Run(r) => rc::run_label(f, r, st.expanded.contains(&r)),
                        };
                        let selected = match row {
                            Row::Run(r) => holds && !st.expanded.contains(&r),
                            _ => st.draw == Some(d.raw.ordinal),
                        };
                        let r = p.add_sized([cols[0], ROW], egui::SelectableLabel::new(selected, text));
                        if st.focus && selected {
                            r.request_focus();
                            st.focus = false;
                        }
                        if r.has_focus() {
                            out.focused = st.draw.filter(|_| holds);
                        }
                        if r.clicked() {
                            if let Row::Run(run) = row {
                                if !st.expanded.remove(&run) {
                                    st.expanded.insert(run);
                                }
                                if !holds {
                                    st.draw = Some(d.raw.ordinal);
                                }
                            } else {
                                st.draw = Some(d.raw.ordinal);
                            }
                        }
                        let (time, timing_text) = match row {
                            Row::Run(r) => {
                                let (sum, timed) = rc::run_time(f, r);
                                (v.duration(sum), format!("run total: {timed} of {} draws timed", span.len()))
                            }
                            _ => (v.duration(d.timing.interval()), d.timing.text()),
                        };
                        let mut args = if d.raw.kind == rf::DrawKind::Clear {
                            format!("mask {:#x}", d.raw.clear_mask)
                        } else {
                            format!("{} · {}", rf::primitive_name(d.raw.mode), d.raw.count)
                        };
                        if let Row::Run(r) = row {
                            args = format!("{args} · varies {}", rc::varies(&f.instance_runs[r]));
                        }
                        let cells = [
                            (time.text, format!("{}\n{}", time.hover, timing_text)),
                            (args.clone(), args),
                            (
                                d.label.clone().unwrap_or("(no label)".into()),
                                d.label.clone().unwrap_or_default(),
                            ),
                            program(f, d.vp, names),
                            program(f, d.fp, names),
                            match crate::rsx_frame_view::samples(f, d) {
                                Some(s) => (s.to_string(), "Passed samples (ZPASS counter over the draw)".into()),
                                None => ("-".into(), rf::ZpassState::of(f, d).why_uncounted()
                                    .unwrap_or("the draw was not timed").to_string()),
                            },
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
/// What a passed-sample count is (hover text and the overdraw panel).
pub const PIXELS_NOTE: &str = "Passed samples: the ZPASS counter over the draw, i.e. samples that passed every \
per-fragment test (depth, stencil, alpha, KIL; early-z and zcull rejections are not counted). Not fragments shaded: a \
program that kills fragments shades every rasterized sample. MSAA counts samples. Overdraw = samples / target area.";

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
