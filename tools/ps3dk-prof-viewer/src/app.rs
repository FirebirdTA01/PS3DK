//! The viewer application: pages, the Connect dialog and the live session
//! wiring. `main.rs` only starts it, so tests can drive `App` directly.
//! Phase 1, increments 1 and 2: local files, Overview, Timeline and Zones.
//! Phase 2: a live session from the in-app agent (Connect).

use crate::live::LinkState;
use crate::live_client::{ClientConfig, LiveClient, LiveView, Outcome};
use crate::capture::{FrameSnapshot, Premise};
use crate::capture_ctl::{Captures, Home};
use crate::capture_ui;
use crate::frames_ui;
use crate::timeline_ui;
use std::sync::Arc;
use std::time::Duration;

use crate::model::{
    overview, sort_zones, zone_cell, zone_cell_exact, zone_key, zone_sites, zones, DocState,
    Document, ExtView, TimeBase, VerdictTone, ZoneColumn, ZoneKey,
};
use eframe::egui;
use ps3dk_prof::ZoneStats;
use ps3dk_ui as ui;
use std::path::PathBuf;

pub const APP_NAME: &str = "PS3DK Profiler";

#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum Page {
    Overview,
    Timeline,
    Frames,
    Zones,
    HotPaths,
    /// RSX frame captures found in the data.
    RsxFrames,
    /// One frame snapshot (`App::shown`).
    Snapshot,
}

/// The Connect dialog's fields.
#[derive(Debug, Clone, Default)]
pub struct ConnectForm {
    pub host: String,
    pub port: String,
    pub dir: String,
    pub error: Option<String>,
}

/// A live session and what the app has taken from it so far.
pub struct Live {
    pub client: LiveClient,
    pub target: String,
    generation: u64,
    /// The session's outcome was handled (bookmarks written, or the reason they were not shown).
    bookmarks_done: bool,
}

pub struct App {
    pub doc: Option<Arc<Document>>,
    pub error: Option<String>,
    pub page: Page,
    zone_rows: Vec<ZoneStats>,
    sort: (ZoneColumn, bool),
    pub selected_zone: Option<ZoneKey>,
    pub timeline: Option<timeline_ui::TimelineState>,
    /// The Frames page selection and budget.
    pub frames: frames_ui::FramesState,
    /// The Hot paths page selection.
    pub hot: crate::hotpaths_ui::HotState,
    /// The RSX frames page selection.
    pub rsx: crate::rsx_ui::RsxState,
    pub live: Option<Live>,
    connect: Option<ConnectForm>,
    last_host: String,
    /// The current session's frame snapshots and bookmarks.
    pub captures: Captures,
    /// The snapshot on the Snapshot page (a counted reference: a deleted snapshot is refunded only once this lets go).
    pub shown: Option<Arc<FrameSnapshot>>,
    sessions: u64,
    /// Live sessions replaced before their outcome was known, with their snapshots: disconnected, polled until the
    /// outcome, bookmarked against the file it saved, then dropped.
    pub retiring: Vec<(Live, Captures)>,
    /// The one accounted retention budget every session (current and retiring) draws on.
    pub pool: Arc<crate::capture::Pool>,
}

/// Turns the dialog's fields into a checked client config, changing
/// nothing: the host must be a literal IP, the port 1..65535, and the save
/// folder a nonempty ABSOLUTE path (never relative to wherever the app was
/// started).
pub fn form_config(form: &ConnectForm) -> Result<ClientConfig, String> {
    let host = form.host.trim();
    if host.is_empty() {
        return Err("Enter the console's IP address.".into());
    }
    let port: u16 = match form.port.trim().parse() {
        Ok(p) if p > 0 => p,
        _ => return Err("The port is a number from 1 to 65535.".into()),
    };
    let dir = form.dir.trim();
    if dir.is_empty() {
        return Err("Choose a folder for the captures.".into());
    }
    let dir = PathBuf::from(dir);
    if !dir.is_absolute() {
        return Err(format!(
            "The save folder must be a full path, not {}.",
            dir.display()
        ));
    }
    let cfg = ClientConfig::new(host, port, dir);
    cfg.validate()
        .map_err(|e| format!("Cannot connect: {e}."))?;
    Ok(cfg)
}

/// `<home>/Documents/PS3DK Captures`, or `<home>/PS3DK Captures` without a
/// Documents folder; None without a home folder (the user then chooses).
pub fn default_capture_dir(home: Option<PathBuf>) -> Option<String> {
    let h = home?;
    let base = if h.join("Documents").is_dir() {
        h.join("Documents")
    } else {
        h
    };
    Some(base.join("PS3DK Captures").display().to_string())
}

/// The user's home folder from the environment (read only here).
pub fn home_dir() -> Option<PathBuf> {
    home_from(std::env::var_os("USERPROFILE"), std::env::var_os("HOME"))
}

/// The first non-empty of USERPROFILE and HOME: an empty USERPROFILE does
/// not hide a valid HOME.
pub fn home_from(
    userprofile: Option<std::ffi::OsString>,
    home: Option<std::ffi::OsString>,
) -> Option<PathBuf> {
    [userprofile, home]
        .into_iter()
        .flatten()
        .find(|h| !h.is_empty())
        .map(PathBuf::from)
}

impl App {
    pub fn new(open: Option<PathBuf>) -> App {
        let pool = crate::capture::Pool::new(crate::capture::RETAIN_BUDGET);
        let mut app = App {
            doc: None,
            error: None,
            page: Page::Overview,
            zone_rows: Vec::new(),
            sort: (ZoneColumn::Total, true),
            selected_zone: None,
            timeline: None,
            frames: frames_ui::FramesState::default(),
            hot: crate::hotpaths_ui::HotState::default(),
            rsx: crate::rsx_ui::RsxState::default(),
            live: None,
            connect: None,
            last_host: String::new(),
            captures: Captures::with_pool("none", Home::Live, pool.clone()),
            pool,
            shown: None,
            sessions: 0,
            retiring: Vec::new(),
        };
        if let Some(p) = open {
            app.open(p);
        }
        app
    }

    pub fn open(&mut self, path: PathBuf) {
        match std::fs::read(&path) {
            Ok(bytes) => {
                // Ends any live session: Drop only requests the disconnect;
                // the session's threads finish in the background.
                self.end_captures();
                self.live = None;
                let doc = Arc::new(Document::from_bytes(path.clone(), &bytes));
                self.install(doc, true);
                self.error = None;
                self.sessions += 1;
                self.captures = Captures::with_pool(
                    &format!("file:{}:{}", path.display(), self.sessions),
                    Home::File(path.clone()),
                    self.pool.clone(),
                );
                self.captures.bind_source(&bytes);
                self.captures.load_marks(&path, &bytes);
            }
            Err(e) => self.error = Some(format!("{}: {e}", path.display())),
        }
    }

    /// Shows a document. `fresh` is a different capture (reset the views);
    /// otherwise it is a newer snapshot of the same live capture (keep the
    /// selection and the timeline window).
    fn install(&mut self, doc: Arc<Document>, fresh: bool) {
        self.zone_rows = doc.analysis().map(zones).unwrap_or_default();
        sort_zones(&mut self.zone_rows, self.sort.0, self.sort.1);
        match (fresh, self.timeline.as_mut(), doc.analysis()) {
            (false, Some(st), Some(a)) => st.refresh(a),
            (_, _, a) => {
                self.selected_zone = None;
                self.timeline = a.map(timeline_ui::TimelineState::new);
            }
        }
        // Frames: a fresh capture resets the selection; a newer snapshot keeps it only where it still exists.
        match (fresh, doc.analysis(), doc.frames.as_ref()) {
            (false, Some(a), Some(ix)) => self.frames.refresh(a, ix),
            (_, _, ix) => self.frames.reset(ix),
        }
        // Hot paths: a fresh capture resets the lane and row; a newer snapshot keeps them (the page revalidates).
        if fresh {
            self.hot = crate::hotpaths_ui::HotState::default();
            // RSX frames: a newer snapshot keeps the selected cap (the page shows nothing if it is gone).
            self.rsx = crate::rsx_ui::RsxState::default();
        }
        self.doc = Some(doc);
    }

    /// Starts a live session. Everything is checked, and the session
    /// started, BEFORE any of the app's current state is replaced: on an
    /// error the open document, selection and session are untouched.
    pub fn start_live(&mut self, form: &ConnectForm) -> Result<(), String> {
        let cfg = form_config(form)?;
        std::fs::create_dir_all(&cfg.out_dir)
            .map_err(|e| format!("Cannot use {} for captures: {e}", cfg.out_dir.display()))?;
        let target = format!("{}:{}", cfg.host.trim(), cfg.port);
        let host = cfg.host.trim().to_string();
        let client = LiveClient::start(cfg)?;
        // Only now does the new session replace what was shown.
        self.end_captures();
        self.sessions += 1;
        self.captures = Captures::with_pool(&format!("live:{target}:{}", self.sessions), Home::Live, self.pool.clone());
        self.live = Some(Live {
            client,
            target,
            generation: 0,
            bookmarks_done: false,
        });
        self.doc = None;
        self.timeline = None;
        self.frames.reset(None);
        self.hot = crate::hotpaths_ui::HotState::default();
        self.rsx = crate::rsx_ui::RsxState::default();
        self.zone_rows.clear();
        self.selected_zone = None;
        self.error = None;
        self.last_host = host;
        Ok(())
    }

    /// Takes the newest snapshot of a live session, if there is one.
    fn poll_live(&mut self, ctx: &egui::Context) {
        if self.poll() {
            ctx.request_repaint_after(Duration::from_millis(100));
        }
    }

    /// One poll of the live session and of every retiring one (no egui): installs a newer snapshot, and once an
    /// outcome is known bookmarks that session's snapshots.  True while something is still running.
    pub fn poll(&mut self) -> bool {
        let mut running = false;
        let mut i = 0;
        while i < self.retiring.len() {
            let v = self.retiring[i].0.client.view();
            match &v.outcome {
                Some(o) => {
                    let (_, mut caps) = self.retiring.remove(i);
                    let note = finish_live(&mut caps, o);
                    caps.clear();
                    self.captures.message = Some((note.0, format!("Previous session: {}", note.1)));
                }
                None => {
                    running = true;
                    i += 1;
                }
            }
        }
        let Some(live) = self.live.as_mut() else {
            return running;
        };
        let v = live.client.view();
        let streaming = v.outcome.is_none();
        if v.generation != live.generation {
            let first = live.generation == 0;
            live.generation = v.generation;
            if let Some(doc) = v.doc {
                self.install(doc, first);
            }
        }
        if let Some(st) = self.timeline.as_mut() {
            st.live = streaming;
            if !streaming {
                st.follow = false;
            }
        }
        if let (Some(o), Some(l)) = (&v.outcome, self.live.as_mut()) {
            if !l.bookmarks_done {
                l.bookmarks_done = true;
                let note = finish_live(&mut self.captures, o);
                if !note.1.is_empty() {
                    self.captures.message = Some(note);
                }
            }
        }
        running || !self.live.as_ref().is_some_and(|l| l.client.finished())
    }


    /// Ends the current session's snapshots (a new session or file).  A live session whose outcome is not known yet
    /// keeps its snapshots: it is disconnected and retired until its file is published, then bookmarked.
    fn end_captures(&mut self) {
        self.shown = None;
        if self.page == Page::Snapshot {
            self.page = Page::Overview;
        }
        let unfinished = self.live.as_ref().is_some_and(|l| !l.bookmarks_done);
        let caps = std::mem::replace(&mut self.captures, Captures::with_pool("none", Home::Live, self.pool.clone()));
        if unfinished && !caps.list.is_empty() {
            let live = self.live.take().expect("unfinished implies a live session");
            live.client.disconnect();
            self.retiring.push((live, caps));
        } else {
            let mut caps = caps;
            caps.clear();
        }
    }

    /// Why "Capture full frame" is unavailable, or None.
    pub fn capture_blocked(&self) -> Option<String> {
        self.captures.blocked(self.doc.as_deref())
    }

    /// The "Capture full frame" click: the Frames selection when a frame is selected, else the newest frame
    /// (`capture::choose_frame`), under the Timeline's current premise.
    pub fn capture_frame(&mut self) {
        let Some(doc) = self.doc.clone() else { return };
        let (owner, frame) = (self.frames.sel.owner, self.frames.sel.frame);
        let premise = self
            .timeline
            .as_ref()
            .map(|t| Premise { w32: t.w32, show_extrapolated: t.show_extrapolated })
            .unwrap_or(Premise { w32: false, show_extrapolated: true });
        let taken = now_text();
        let live = self.live.as_ref();
        let path = doc.path.clone();
        let mut fetch = |n: u64| -> Option<Vec<u8>> {
            match live {
                Some(l) => l.client.received_prefix(n),
                None => std::fs::read(&path).ok().filter(|b| b.len() as u64 == n),
            }
        };
        if let Ok(s) = self.captures.capture(&doc, owner, frame, premise, &taken, &mut fetch) {
            self.shown = Some(s);
            self.page = Page::Snapshot;
        }
    }

    /// Applies a snapshot action from the sidebar or the Snapshot page.
    pub fn snapshot_action(&mut self, a: capture_ui::Action) {
        match a {
            capture_ui::Action::Show(id) => {
                if let Some(s) = self.captures.list.iter().find(|s| s.id == id) {
                    self.shown = Some(s.clone());
                    self.page = Page::Snapshot;
                }
            }
            capture_ui::Action::OpenMark(i) => {
                let Home::File(p) = self.captures.home.clone() else { return };
                if let Ok(s) = self.captures.open_mark(i, &p) {
                    self.shown = Some(s);
                    self.page = Page::Snapshot;
                }
            }
            capture_ui::Action::Delete(id) => {
                if self.shown.as_ref().is_some_and(|s| s.id == id) {
                    self.shown = None;
                    self.page = Page::Frames;
                }
                self.captures.delete(id);
                self.captures.sweep();
            }
            capture_ui::Action::Reresolve(premise) => {
                if let Some(cur) = self.shown.clone() {
                    if let Ok(s) = self.captures.reresolve(&cur, premise) {
                        self.shown = Some(s);
                    }
                }
            }
            capture_ui::Action::Back => {
                self.shown = None;
                self.page = Page::Frames;
                self.captures.sweep();
            }
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
        VerdictTone::Complete | VerdictTone::CompleteEarly => ui::Tone::Success,
        VerdictTone::Incomplete => ui::Tone::Warning,
        VerdictTone::Malformed => ui::Tone::Danger,
    }
}

/// What the app bar badge and the verdict strip say about the open document, given the live link (if any).
///
/// While the link has no outcome yet (connecting, streaming, or END received and still being checked), the document
/// is a SNAPSHOT of a capture that is still arriving.  It has no TRAILER yet, so the reader necessarily reads it as an
/// accepted prefix ("COMPLETE (ended early)") or as INCOMPLETE: that is a statement about the prefix, not an end state,
/// and it is never shown as one.  The strip then says PROVISIONAL with the bytes received and what the snapshot covers,
/// and there is no verdict badge (the bar already shows LIVE / PROVISIONAL).  A MALFORMED snapshot is shown as it is:
/// bytes that break a rule stay broken whatever arrives later.  Once the outcome is known, the reader's verdict is shown
/// unchanged.  Returns (tone, badge, strip text).
pub fn shown_verdict(d: &Document, live: Option<&LiveView>) -> (ui::Tone, Option<&'static str>, String) {
    let v = d.verdict();
    let pending = live.filter(|l| l.outcome.is_none());
    match (pending, v.tone) {
        (Some(l), VerdictTone::Complete | VerdictTone::CompleteEarly | VerdictTone::Incomplete) => {
            let covered = match d.analysis().and_then(|a| a.prefix.as_ref()) {
                Some(p) => format!("this snapshot is analysed up to byte {} ({} whole chunks)", p.cut, p.chunks),
                None if v.tone == VerdictTone::Incomplete => "this snapshot has no analysable prefix yet".to_string(),
                None => "this snapshot is analysed in full".to_string(),
            };
            let text = format!(
                "PROVISIONAL: still receiving ({} bytes so far); {covered}. {}",
                l.received,
                "The capture's end state is decided only when the link has ended."
            );
            (ui::Tone::Neutral, None, text)
        }
        _ => (tone(v.tone), Some(verdict_word(v.tone)), v.text),
    }
}

fn verdict_word(t: VerdictTone) -> &'static str {
    match t {
        VerdictTone::Complete => "COMPLETE",
        VerdictTone::CompleteEarly => "COMPLETE",
        VerdictTone::Incomplete => "INCOMPLETE",
        VerdictTone::Malformed => "MALFORMED",
    }
}

impl eframe::App for App {
    fn update(&mut self, ctx: &egui::Context, _frame: &mut eframe::Frame) {
        self.poll_live(ctx);
        let live_view = self.live.as_ref().map(|l| l.client.view());
        let doc_line = match &live_view {
            Some(v) => Some(egui::WidgetText::from(match &v.hello {
                Some(h) => format!("{} (live)", h.file_name),
                None => "connecting…".to_string(),
            })),
            None => self.doc.as_ref().map(|d| {
                egui::WidgetText::from(
                    d.path
                        .file_name()
                        .map(|n| n.to_string_lossy().into_owned())
                        .unwrap_or_default(),
                )
            }),
        };
        let streaming = live_view.as_ref().is_some_and(|v| v.outcome.is_none());
        let mut want_open = false;
        let mut want_capture = false;
        self.captures.sweep();
        let has_doc_analysis = self.doc.as_ref().and_then(|d| d.analysis()).is_some();
        let capture_blocked = self.capture_blocked();
        let mut want_connect = false;
        let mut want_disconnect = false;
        ui::app_bar(
            ctx,
            |bar| {
                ui::app_title(bar, "PF", APP_NAME, doc_line);
                if streaming {
                    bar.add_space(ui::SPACE_LG);
                    ui::badge(bar, "LIVE · PROVISIONAL", ui::Tone::Neutral);
                }
                if let Some((t, Some(word), _)) = self.doc.as_ref().map(|d| shown_verdict(d, live_view.as_ref())) {
                    bar.add_space(ui::SPACE_LG);
                    ui::badge(bar, word, t);
                }
            },
            |bar| {
                if ui::primary_button(bar, "Open…").clicked() {
                    want_open = true;
                }
                if has_doc_analysis && capture_ui::button(bar, capture_blocked.as_deref(), &mut None) {
                    want_capture = true;
                }
                if streaming {
                    if ui::danger_button(bar, "Disconnect").clicked() {
                        want_disconnect = true;
                    }
                } else if ui::quiet_button(bar, "Connect…").clicked() {
                    want_connect = true;
                }
            },
        );
        if want_open {
            self.pick_file();
        }
        if want_capture {
            self.capture_frame();
        }
        if want_disconnect {
            if let Some(l) = &self.live {
                l.client.disconnect();
            }
        }
        if want_connect {
            self.connect = Some(ConnectForm {
                host: self.last_host.clone(),
                port: "31750".into(),
                dir: default_capture_dir(home_dir()).unwrap_or_default(),
                error: None,
            });
        }
        self.connect_dialog(ctx);
        // The full verdict, always visible under the bar on every page; it
        // wraps instead of being cut off at small window sizes.
        let link_line = live_view.as_ref().map(live_line);
        let snap_msg = self.captures.message.clone();
        if self.doc.is_some() || link_line.is_some() || snap_msg.is_some() {
            let verdict = self.doc.as_ref().map(|d| shown_verdict(d, live_view.as_ref()));
            egui::TopBottomPanel::top("verdict").show(ctx, |strip| {
                strip.add_space(ui::SPACE_XS);
                // The link and the reader's verdict are separate statements.
                if let Some((t, text)) = &link_line {
                    ui::field_message(strip, *t, text);
                }
                if let Some((t, _, text)) = &verdict {
                    ui::field_message(strip, *t, text);
                }
                if let Some((bad, m)) = &snap_msg {
                    ui::field_message(strip, if *bad { ui::Tone::Warning } else { ui::Tone::Neutral }, m);
                }
                strip.add_space(ui::SPACE_XS);
            });
        }

        let has_analysis = self.doc.as_ref().and_then(|d| d.analysis()).is_some();
        let rsx_count = self.rsx_count();
        let mut side_act = None;
        let shown_id = self.shown.as_ref().filter(|_| self.page == Page::Snapshot).map(|s| s.id);
        let caps = &self.captures;
        ui::sidebar(
            ctx,
            |side| {
                ui::nav_section(side, "CAPTURE");
                if ui::nav_item(side, self.page == Page::Overview, "📋", "Overview", None).clicked()
                {
                    self.page = Page::Overview;
                }
                if has_analysis {
                    if ui::nav_item(side, self.page == Page::Timeline, "📈", "Timeline", None)
                        .clicked()
                    {
                        self.page = Page::Timeline;
                    }
                    if ui::nav_item(side, self.page == Page::Frames, "🎞", "Frames", None)
                        .clicked()
                    {
                        self.page = Page::Frames;
                    }
                    let n = self.zone_rows.len();
                    if ui::nav_item(side, self.page == Page::Zones, "📊", "Zones", Some(n))
                        .clicked()
                    {
                        self.page = Page::Zones;
                    }
                    if ui::nav_item(side, self.page == Page::HotPaths, "🔥", "Hot paths", None).clicked() {
                        self.page = Page::HotPaths;
                    }
                    if let Some(n) = rsx_count {
                        if ui::nav_item(side, self.page == Page::RsxFrames, "🖼", crate::rsx_ui::TITLE, Some(n)).clicked() {
                            self.page = Page::RsxFrames;
                        }
                    }
                }
                side_act = capture_ui::sidebar(side, caps, shown_id, &mut Vec::new());
            },
            |_| {},
        );

        match (&self.error, &self.live, &live_view) {
            (Some(e), _, _) => ui::status_bar(ctx, ui::Tone::Danger, "Could not open the file", e),
            (None, Some(l), Some(v)) => ui::status_bar(
                ctx,
                if v.local_error.is_some() {
                    ui::Tone::Danger
                } else {
                    ui::Tone::Neutral
                },
                &format!("Live: {}", l.target),
                &match &v.local_error {
                    Some(e) => e.clone(),
                    None => format!("{} bytes received, {} parses", v.received, v.parses),
                },
            ),
            _ => match &self.doc {
                Some(d) => ui::status_bar(
                    ctx,
                    ui::Tone::Neutral,
                    &d.path.display().to_string(),
                    &format!("{} bytes", d.size),
                ),
                None => ui::status_bar(ctx, ui::Tone::Neutral, "No capture open", ""),
            },
        }

        if let Some(a) = side_act {
            self.snapshot_action(a);
        }
        let page = self.page;
        if page == Page::Snapshot {
            if let Some(s) = self.shown.clone() {
                let mut act = None;
                ui::page(ctx, |p| {
                    egui::ScrollArea::vertical().show(p, |p| {
                        act = capture_ui::page(p, &s, &mut capture_ui::Drawn::default());
                    });
                });
                drop(s);
                if let Some(a) = act {
                    self.snapshot_action(a);
                }
                return;
            }
            self.page = Page::Frames;
        }
        // The timeline takes the whole page: wheel and drag belong to it.
        if page == Page::Timeline {
            if let (Some(doc), Some(st)) = (self.doc.as_ref(), self.timeline.as_mut()) {
                if let Some(a) = doc.analysis() {
                    timeline_ui::draw(ctx, &doc.foreign_tl, a, st, &mut self.selected_zone);
                    return;
                }
            }
        }
        let sort = &mut self.sort;
        let rows = &mut self.zone_rows;
        let selected = &mut self.selected_zone;
        let frames = &mut self.frames;
        let timeline = &mut self.timeline;
        let hot = &mut self.hot;
        let rsx = &mut self.rsx;
        let mut nav: Option<(u32, u64)> = None;
        ui::page(ctx, |p| {
            match &self.doc {
            None => ui::empty_state(
                p,
                "Open a capture",
                "Choose a .prof file written by libps3dkprof, or Connect to an app running the live agent.",
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
                    Page::Overview | Page::Timeline | Page::Snapshot => {
                        draw_overview(p, a, &d.ext, &d.rsx.get(a))
                    }
                    Page::Frames => match &d.frames {
                        Some(ix) => {
                            frames_ui::draw(p, a, ix, frames, timeline.as_mut(), Some(&d.foreign));
                        }
                        None => ui::field_message(p, ui::Tone::Neutral, "No frame index for this document."),
                    },
                    Page::Zones => draw_zones(p, a, rows, sort, selected),
                    Page::HotPaths => match &d.hot {
                        Some(hv) => nav = crate::hotpaths_ui::draw(p, hv, hot, Some((a, &d.foreign_hot))),
                        None => ui::field_message(p, ui::Tone::Neutral, "No hot paths for this document."),
                    },
                    Page::RsxFrames => {
                        crate::rsx_ui::page(p, a, &d.rsx.get(a), rsx, &mut crate::rsx_ui::Drawn::default())
                    }
                },
            },
        }
        });
        // A frame link on the Hot paths page opens that (owner, frame) on the Frames page.
        if let (Some((owner, frame)), Some(d)) = (nav, self.doc.clone()) {
            if let (Some(a), Some(ix)) = (d.analysis(), d.frames.as_ref()) {
                self.frames.select(a, ix, owner, Some(frame));
                self.page = Page::Frames;
            }
        }
    }
}

impl App {
    /// The RSX frames sidebar count (captures found, analysed or not), or None when the page is not offered (no
    /// analysis, or no GPU source).  Builds the Document's RSX view on first need.
    pub fn rsx_count(&self) -> Option<usize> {
        let d = self.doc.as_ref()?;
        let v = d.rsx.get(d.analysis()?);
        v.has_gpu.then(|| v.caps.captures.len() + v.caps.not_analysed.len() + v.caps.not_analysed_omitted as usize)
    }

    fn connect_dialog(&mut self, ctx: &egui::Context) {
        let Some(mut form) = self.connect.take() else {
            return;
        };
        let mut close = false;
        let mut go = false;
        ui::modal(
            ctx,
            "connect",
            "Connect to a console",
            Some("The app must be running with the live agent enabled."),
            |m| {
                ui::field_label(m, "Console IP address", None, None);
                m.text_edit_singleline(&mut form.host);
                ui::field_label(m, "Port", None, None);
                m.text_edit_singleline(&mut form.port);
                ui::field_label(m, "Save captures in", None, None);
                m.text_edit_singleline(&mut form.dir);
                if let Some(e) = &form.error {
                    ui::field_message(m, ui::Tone::Danger, e);
                }
                ui::dialog_buttons(m, |b| {
                    if ui::primary_button(b, "Connect").clicked() {
                        go = true;
                    }
                    if ui::quiet_button(b, "Cancel").clicked() {
                        close = true;
                    }
                });
            },
        );
        if go {
            match self.start_live(&form) {
                Ok(()) => close = true,
                Err(e) => form.error = Some(e),
            }
        }
        if !close {
            self.connect = Some(form);
        }
    }
}

/// The live link's own line: state and, once decided, the outcome.
pub fn live_line(v: &LiveView) -> (ui::Tone, String) {
    match &v.outcome {
        Some(Outcome::Published(p)) => {
            let mut t = format!("Live session complete; saved {}", p.path.display());
            if let Some(w) = &p.cleanup_warning {
                t.push_str(&format!(" ({w})"));
            }
            (ui::Tone::Success, t)
        }
        Some(Outcome::PublishedEarly { published, cause, cut, torn_dropped }) => {
            let mut t = format!(
                "Live session ended without a trailer ({cause}); saved {} as a complete capture up to byte {cut}",
                published.path.display()
            );
            if *torn_dropped > 0 {
                t.push_str(&format!(" ({torn_dropped} bytes of a torn last chunk dropped)"));
            }
            if let Some(w) = &published.cleanup_warning {
                t.push_str(&format!(" ({w})"));
            }
            (ui::Tone::Success, t)
        }
        Some(Outcome::Incomplete { saved, reasons }) => {
            let at = saved
                .as_ref()
                .map(|p| match &p.cleanup_warning {
                    Some(w) => format!("; kept as {} ({w})", p.path.display()),
                    None => format!("; kept as {}", p.path.display()),
                })
                .unwrap_or_default();
            (
                ui::Tone::Warning,
                format!("Live session incomplete: {}{at}", reasons.join("; ")),
            )
        }
        None => match &v.link {
            LinkState::Handshake => (ui::Tone::Neutral, "LIVE: connecting…".into()),
            LinkState::Streaming => (
                ui::Tone::Neutral,
                "LIVE, PROVISIONAL: the capture is still arriving; statistics cover what has arrived so far".into(),
            ),
            LinkState::Ended(_) => (ui::Tone::Neutral, "LIVE: END received; checking…".into()),
            LinkState::Failed(e) => (ui::Tone::Danger, format!("LIVE link failed: {e}")),
        },
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

fn draw_overview(p: &mut egui::Ui, a: &ps3dk_prof::Analysis, ext: &ExtView, rsx: &crate::rsx::RsxView) {
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
        draw_ext_sources(p, ext);
        if let Some(line) = rsx.overview_line() {
            p.add_space(ui::SPACE_LG);
            ui::section_label(p, "RSX FRAME CAPTURES");
            let tone = if rsx.caps.anomaly_count() > 0 { ui::Tone::Danger } else { ui::Tone::Neutral };
            ui::field_message(p, tone, &format!("{line} (see the RSX frames page)"));
        }
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

/// A wrapped "Label: value" line (long reasons wrap at the window width; nothing is tooltip-only).
fn fact(p: &mut egui::Ui, label: &str, value: &str) {
    p.horizontal_wrapped(|h| {
        h.label(egui::RichText::new(format!("{label}:")).strong());
        h.add(egui::Label::new(value).wrap());
    });
}

/// The EXTENSION SOURCES section: one block per source, its facts directly above its own epoch rows.  The three
/// counter states are neutral (they describe recorded values, not timing quality); warnings keep their tone.
fn draw_ext_sources(p: &mut egui::Ui, ext: &ExtView) {
    use ps3dk_prof::report::{LaneEpochs, EPOCH_LOSS_SPAN_LABEL, EPOCH_TAIL};
    let blocks = match ext {
        ExtView::None => return,
        ExtView::Empty => {
            p.add_space(ui::SPACE_LG);
            ui::section_label(p, "EXTENSION SOURCES");
            p.add(
                egui::Label::new("No SPU/RSX sources recorded; no SPU/RSX coverage is claimed.")
                    .wrap(),
            );
            return;
        }
        ExtView::Sources(b) => b,
    };
    p.add_space(ui::SPACE_LG);
    ui::section_label(p, "EXTENSION SOURCES");
    p.add(
        egui::Label::new(
            "MAPPING UNMEASURED: no SPU/RSX time is shown; states describe recorded values only.",
        )
        .wrap(),
    );
    for b in blocks {
        let s = &b.source;
        p.add_space(ui::SPACE_MD);
        ui::card(p, |c| {
            c.label(egui::RichText::new(format!("{} (domain {})", s.source, s.domain)).strong());
            c.horizontal_wrapped(|h| {
                if s.structure_complete {
                    ui::badge(h, "COMPLETE", ui::Tone::Success);
                } else {
                    ui::badge(h, "INCOMPLETE", ui::Tone::Warning);
                }
                if s.raw_valid {
                    ui::badge(h, "RAW VALID", ui::Tone::Success);
                } else {
                    ui::badge(h, "RAW NOT VALID", ui::Tone::Warning);
                }
            });
            fact(c, "Structure", &s.structure);
            fact(c, "Raw validity", &s.raw);
            fact(c, "Coverage", &s.coverage);
            fact(
                c,
                "Events",
                &format!("{} written, {} lost", s.events, s.lost),
            );
            match &b.epochs {
                LaneEpochs::NotALane => {
                    c.add(
                        egui::Label::new(
                            "Source facts only (no counter classification for this domain).",
                        )
                        .wrap(),
                    );
                }
                LaneEpochs::Inconsistent => ui::field_message(
                    c,
                    ui::Tone::Warning,
                    "Counter variation: not classified (decoded epochs inconsistent).",
                ),
                LaneEpochs::Epochs(v) => {
                    if let Some(q) = v.first().and_then(|e| e.invalidity.as_ref()) {
                        ui::field_message(c, ui::Tone::Warning, &format!("Epoch rows below: {q}"));
                    }
                    for e in v {
                        c.add_space(ui::SPACE_XS);
                        c.indent(("epoch", s.domain, e.epoch), |i| {
                            i.label(egui::RichText::new(format!("Epoch {}", e.epoch)).strong());
                            i.add(egui::Label::new(e.state_text).wrap());
                            fact(i, "Observations", &e.observations);
                            fact(i, EPOCH_LOSS_SPAN_LABEL, &e.loss_span);
                            i.add(egui::Label::new(EPOCH_TAIL).wrap());
                        });
                    }
                }
            }
        });
    }
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

/// The current UTC time of day, for a snapshot's label.
fn now_text() -> String {
    let t = std::time::SystemTime::now()
        .duration_since(std::time::UNIX_EPOCH)
        .map(|d| d.as_secs())
        .unwrap_or(0)
        % 86_400;
    format!("{:02}:{:02}:{:02} UTC", t / 3600, t / 60 % 60, t % 60)
}

/// The file a session's snapshots are bookmarked against: whatever file the live client's outcome saved, under its
/// own classification (a complete `.prof`, an accepted early ending's `.prof`, or a kept `.incomplete.prof`); None
/// when nothing was saved.
pub fn bookmark_target(o: &Outcome) -> Option<std::path::PathBuf> {
    match o {
        Outcome::Published(p) | Outcome::PublishedEarly { published: p, .. } => Some(p.path.clone()),
        Outcome::Incomplete { saved, .. } => saved.as_ref().map(|p| p.path.clone()),
    }
}

/// A live session's outcome is known: its snapshots are bookmarked next to the file the outcome saved (the live
/// client's own classification decides which file that is; this never reclassifies it), and the session is homed
/// there so later snapshots are bookmarked at once.  Returns (is a warning, note).
pub fn finish_live(caps: &mut Captures, o: &Outcome) -> (bool, String) {
    match bookmark_target(o) {
        Some(path) => match caps.write_live_bookmarks(&path, &mut crate::capture::RealIo::default()) {
            Ok(_) if caps.list.is_empty() => (false, String::new()),
            Ok(note) => (false, note),
            Err(e) => (true, e),
        },
        None => {
            caps.home = Home::Unsaved;
            if caps.list.is_empty() {
                (false, String::new())
            } else {
                (true, "Bookmarks not saved: the session saved no file. The snapshots stay in memory until the next session.".into())
            }
        }
    }
}
