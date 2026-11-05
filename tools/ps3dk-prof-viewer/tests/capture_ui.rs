//! Frame snapshot UI driven with synthetic egui input: the button, the sidebar and the page are drawn in a headless
//! `egui::Context`, clicked with pointer events, and what they return is applied to a real `App`.

#[path = "../../ps3dk-prof/tests/common/mod.rs"]
#[rustfmt::skip]
#[allow(clippy::all)]
mod common;

use std::path::PathBuf;
use std::sync::Arc;

use common::*;
use eframe::egui::{self, Event, Modifiers, PointerButton, Pos2, RawInput, Rect, Vec2};
use ps3dk_prof_viewer::app::{App, Page};
use ps3dk_prof_viewer::capture::{Origin, Premise, Status};
use ps3dk_prof_viewer::capture_ctl::{Captures, Home};
use ps3dk_prof_viewer::capture_ui::{self, Action, Drawn};

struct Ui {
    ctx: egui::Context,
    time: f64,
}

impl Ui {
    fn new() -> Ui {
        let ctx = egui::Context::default();
        ps3dk_ui::install(&ctx);
        Ui { ctx, time: 0.0 }
    }

    fn frame<R: Default>(&mut self, events: Vec<Event>, mut f: impl FnMut(&mut egui::Ui) -> R) -> R {
        self.time += 1.0 / 60.0;
        let input = RawInput {
            screen_rect: Some(Rect::from_min_size(Pos2::ZERO, Vec2::new(1400.0, 1000.0))),
            time: Some(self.time),
            modifiers: Modifiers::NONE,
            events,
            ..Default::default()
        };
        let mut out = R::default();
        let _ = self.ctx.run(input, |ctx| {
            egui::CentralPanel::default().show(ctx, |p| out = f(p));
        });
        out
    }

    /// Draws `f` until it reports a rect, clicks its centre, and returns what `f` returned on the release frame.
    fn click<R: Default + Clone>(&mut self, mut f: impl FnMut(&mut egui::Ui, &mut Option<Rect>) -> R) -> R {
        let mut at = None;
        self.frame(vec![], |p| f(p, &mut at));
        let pos = at.expect("drawn").center();
        let press = |pressed| Event::PointerButton { pos, button: PointerButton::Primary, pressed, modifiers: Modifiers::NONE };
        self.frame(vec![Event::PointerMoved(pos)], |p| f(p, &mut None));
        self.frame(vec![press(true)], |p| f(p, &mut None));
        self.frame(vec![press(false)], |p| f(p, &mut None))
    }
}

/// FRAMEs 0..3 at 1000, 4000, 9000, 12000 on lane 1 with a zone in each of the first three frames.
fn write_capture(name: &str) -> PathBuf {
    let recs = seqd(
        0,
        [
            vec![frame(0, 1_000, 0)],
            zone(0, 0, 0, 1_100, 1, 0),
            vec![end(0, 0, 0, 1_400, 1), frame(0, 4_000, 1)],
            zone(0, 0, 0, 4_100, 2, 0),
            vec![end(0, 0, 0, 4_400, 2), frame(0, 9_000, 2)],
            zone(0, 0, 0, 9_100, 3, 0),
            vec![end(0, 0, 0, 9_400, 3), frame(0, 12_000, 3)],
        ]
        .concat(),
    );
    let d = PathBuf::from(env!("CARGO_TARGET_TMPDIR")).join(format!("capture-ui-{name}-{}", std::process::id()));
    std::fs::create_dir_all(&d).unwrap();
    let p = d.join("ui.prof");
    std::fs::write(&p, scenario(vec![lane(1, recs)]).bytes()).unwrap();
    let _ = std::fs::remove_file(d.join("ui.frames.txt"));
    p
}

fn click_button(ui: &mut Ui, app: &mut App) -> bool {
    let blocked = app.capture_blocked();
    let clicked = ui.click(|p, at| capture_ui::button(p, blocked.as_deref(), at));
    if clicked {
        app.capture_frame();
    }
    clicked
}

fn page_click(ui: &mut Ui, app: &mut App, which: fn(&Drawn) -> Option<Rect>) -> Option<Action> {
    let s = app.shown.clone().expect("a snapshot is shown");
    let act = ui.click(|p, at| {
        let mut d = Drawn::default();
        let a = capture_ui::page(p, &s, &mut d);
        if at.is_none() {
            *at = which(&d);
        }
        a
    });
    drop(s);
    if let Some(a) = act.clone() {
        app.snapshot_action(a);
    }
    act
}

fn sidebar_click(ui: &mut Ui, app: &mut App, want: Action) {
    let caps: &Captures = &app.captures;
    let act = ui.click(|p, at| {
        let mut items = Vec::new();
        let a = capture_ui::sidebar(p, caps, None, &mut items);
        if at.is_none() {
            *at = items.iter().find(|(x, _)| *x == want).map(|(_, r)| *r);
        }
        a
    });
    assert_eq!(act, Some(want));
    app.snapshot_action(act.unwrap());
}

#[test]
fn the_button_captures_the_newest_frame_or_the_frames_selection() {
    let mut ui = Ui::new();
    let mut app = App::new(Some(write_capture("choice")));
    assert_eq!(app.capture_blocked(), None);
    assert!(click_button(&mut ui, &mut app));
    let s = app.shown.clone().unwrap();
    assert_eq!((s.frame, s.tb, s.next_tb), (2, 9_000, 12_000), "newest measured frame");
    assert_eq!(app.page, Page::Snapshot);
    // a Frames selection wins
    let doc = app.doc.clone().unwrap();
    app.frames.select(doc.analysis().unwrap(), doc.frames.as_ref().unwrap(), 1, Some(0));
    assert!(click_button(&mut ui, &mut app));
    assert_eq!(app.shown.as_ref().unwrap().frame, 0);
    assert_eq!(app.captures.list.len(), 2);
}

#[test]
fn a_blocked_button_is_disabled_and_says_why() {
    let mut ui = Ui::new();
    let mut app = App::new(Some(write_capture("blocked")));
    app.captures = Captures::new("x", Home::Live);
    for _ in 0..ps3dk_prof_viewer::capture::MAX_SNAPSHOTS {
        app.capture_frame();
    }
    let why = app.capture_blocked().unwrap();
    assert!(why.contains("is the limit"), "{why}");
    assert!(!click_button(&mut ui, &mut app), "a disabled button does not click");
}

#[test]
fn the_premise_is_frozen_and_re_resolving_makes_a_new_snapshot() {
    let mut ui = Ui::new();
    let mut app = App::new(Some(write_capture("premise")));
    app.timeline.as_mut().unwrap().w32 = true;
    assert!(click_button(&mut ui, &mut app));
    let first = app.shown.clone().unwrap();
    assert!(first.premise.w32);
    let premise_text = ui.frame(vec![], |p| {
        let mut d = Drawn::default();
        capture_ui::page(p, &first, &mut d);
        d.premise
    });
    assert!(premise_text.contains("premise (frozen): SPU W32"), "{premise_text}");
    // the Timeline's toggle changing later does not reinterpret the snapshot
    app.timeline.as_mut().unwrap().w32 = false;
    assert!(app.shown.as_ref().unwrap().premise.w32);
    // Re-resolve
    let act = page_click(&mut ui, &mut app, |d| d.reresolve);
    assert_eq!(act, Some(Action::Reresolve(Premise { w32: false, show_extrapolated: true })));
    let second = app.shown.clone().unwrap();
    assert_ne!(first.id, second.id);
    assert!(!second.premise.w32 && first.premise.w32, "the first snapshot is unchanged");
    assert!(Arc::ptr_eq(&first.snap, &second.snap), "same bytes, charged once");
    assert_eq!(app.captures.list.len(), 2);
}

#[test]
fn switching_between_snapshots_and_the_live_view() {
    let mut ui = Ui::new();
    let mut app = App::new(Some(write_capture("switch")));
    assert!(click_button(&mut ui, &mut app));
    let id = app.shown.as_ref().unwrap().id;
    let act = page_click(&mut ui, &mut app, |d| d.back);
    assert_eq!(act, Some(Action::Back));
    assert_eq!((app.page, app.shown.is_none()), (Page::Frames, true));
    sidebar_click(&mut ui, &mut app, Action::Show(id));
    assert_eq!((app.page, app.shown.as_ref().map(|s| s.id)), (Page::Snapshot, Some(id)));
}

#[test]
fn deleting_the_displayed_snapshot_refunds_it() {
    let mut ui = Ui::new();
    let mut app = App::new(Some(write_capture("delete")));
    let base = app.captures.ledger.used;
    assert!(click_button(&mut ui, &mut app));
    assert!(app.captures.ledger.used > base);
    let held = app.captures.ledger.used;
    let own = { let s = app.shown.as_ref().unwrap(); s.charge + s.snap.charge() };
    assert_eq!(app.captures.marks.len(), 1, "an opened file's snapshot is bookmarked at once");
    let act = page_click(&mut ui, &mut app, |d| d.delete);
    assert!(matches!(act, Some(Action::Delete(_))));
    assert!(app.shown.is_none() && app.captures.list.is_empty());
    assert_eq!(app.page, Page::Frames);
    // the snapshot's own charges are refunded; its bookmark record (metadata) stays with the bookmark
    assert_eq!((app.captures.ledger.used, app.captures.ledger.retained_snapshots()), (held - own, 0));
}

#[test]
fn a_new_file_clears_the_session_and_its_bookmarks_reopen_as_re_resolutions() {
    let mut ui = Ui::new();
    let path = write_capture("reopen");
    let mut app = App::new(Some(path.clone()));
    assert!(click_button(&mut ui, &mut app));
    assert_eq!(app.captures.message.as_ref().map(|m| m.0), Some(false), "{:?}", app.captures.message);
    assert!(path.with_file_name("ui.frames.txt").exists(), "bookmarked next to the open file");
    let other = write_capture("reopen-other");
    app.open(other);
    assert!(app.captures.list.is_empty() && app.shown.is_none());
    assert_eq!(app.captures.ledger.retained_snapshots(), 0);
    // reopen the first file: its bookmark is listed, and opening it is a re-resolution
    app.open(path);
    assert_eq!(app.captures.marks.len(), 1);
    sidebar_click(&mut ui, &mut app, Action::OpenMark(0));
    let s = app.shown.clone().unwrap();
    assert!(matches!(&s.origin, Origin::Reresolved { .. }));
    let strip = ui.frame(vec![], |p| {
        let mut d = Drawn::default();
        capture_ui::page(p, &s, &mut d);
        d.strip
    });
    assert!(strip.starts_with("RE-RESOLVED bookmark #1 against the whole saved file"), "{strip}");
    assert!(!strip.to_lowercase().contains("complete frame"));
    assert_eq!(s.lanes[0].status, Status::Observed);
}

#[test]
fn a_budget_refusal_is_shown_and_retains_nothing() {
    let mut ui = Ui::new();
    let mut app = App::new(Some(write_capture("budget")));
    app.captures = Captures::with_budget("file:b", Home::Live, 512);
    assert!(click_button(&mut ui, &mut app), "the button is enabled: the budget is checked on the click");
    let (bad, msg) = app.captures.message.clone().unwrap();
    assert!(bad && msg.starts_with("Not captured: snapshots would hold"), "{msg}");
    assert!(app.shown.is_none() && app.captures.list.is_empty());
    assert_eq!(app.captures.ledger.used, 0);
    assert_ne!(app.page, Page::Snapshot);
}

#[test]
fn hovering_a_bar_shows_its_membership() {
    let mut ui = Ui::new();
    let mut app = App::new(Some(write_capture("hover")));
    assert!(click_button(&mut ui, &mut app));
    let s = app.shown.clone().unwrap();
    let area = ui.frame(vec![], |p| {
        let mut d = Drawn::default();
        capture_ui::page(p, &s, &mut d);
        d.bars
    });
    let area = area.unwrap();
    // the one claimed zone 9100..9400 of frame [9000, 12000): 10%..13% of the axis right of the label column
    let x = area.left() + 150.0 + (area.width() - 150.0) * 0.12;
    let pos = Pos2::new(x, area.top() + 7.0);
    let hovered = ui.frame(vec![Event::PointerMoved(pos)], |p| {
        let mut d = Drawn::default();
        capture_ui::page(p, &s, &mut d);
        d.hovered
    });
    let i = hovered.expect("the bar is under the pointer");
    let lines = capture_ui::details(&s, &s.bars[i]);
    assert!(lines.iter().any(|l| l.contains("CLAIMED by this frame")), "{lines:?}");
}

#[test]
fn bookmarks_follow_the_live_clients_own_ending_classes() {
    use ps3dk_prof_viewer::app::bookmark_target;
    use ps3dk_prof_viewer::live::Published;
    use ps3dk_prof_viewer::live_client::Outcome;
    let p = |s: &str| Published { path: PathBuf::from(s), cleanup_warning: None };
    assert_eq!(bookmark_target(&Outcome::Published(p("a.prof"))), Some(PathBuf::from("a.prof")));
    let early = Outcome::PublishedEarly { published: p("e.prof"), cause: "user disconnect".into(), cut: 10, torn_dropped: 0 };
    assert_eq!(bookmark_target(&early), Some(PathBuf::from("e.prof")), "an accepted early ending is a normal file");
    let kept = Outcome::Incomplete { saved: Some(p("k.incomplete.prof")), reasons: vec!["link failed".into()] };
    assert_eq!(bookmark_target(&kept), Some(PathBuf::from("k.incomplete.prof")));
    assert_eq!(bookmark_target(&Outcome::Incomplete { saved: None, reasons: vec![] }), None);
}

// ---- live session transitions (a mock agent on localhost) ----------------------------------------------------------

mod live_mock {
    use super::*;
    use ps3dk_prof_viewer::app::ConnectForm;
    use ps3dk_prof_viewer::live::*;
    use std::io::{Read, Write};
    use std::net::TcpListener;
    use std::time::{Duration, Instant};

    fn wire(t: u16, payload: &[u8]) -> Vec<u8> {
        let mut f = t.to_le_bytes().to_vec();
        f.extend_from_slice(&0u16.to_le_bytes());
        f.extend_from_slice(&(payload.len() as u32).to_le_bytes());
        f.extend_from_slice(payload);
        f
    }

    fn hello_frame() -> Vec<u8> {
        let mut p = Vec::new();
        p.extend_from_slice(&VERSION.to_le_bytes());
        p.extend_from_slice(&1u16.to_le_bytes());
        p.extend_from_slice(&0u32.to_le_bytes());
        p.extend_from_slice(&0x42u64.to_le_bytes());
        for s in [&b"heterogeneous"[..], b"bid", b"heterogeneous-1.prof"] {
            p.extend_from_slice(&(s.len() as u16).to_le_bytes());
            p.extend_from_slice(s);
        }
        wire(T_HELLO, &p)
    }

    fn data_frame(cap: &[u8]) -> Vec<u8> {
        let mut p = 0u64.to_le_bytes().to_vec();
        p.extend_from_slice(cap);
        wire(T_DATA, &p)
    }

    fn end_frame(len: u64) -> Vec<u8> {
        let mut p = vec![1, 1, 0, 0];
        p.extend_from_slice(&0i32.to_le_bytes());
        p.extend_from_slice(&len.to_le_bytes());
        p.extend_from_slice(&len.to_le_bytes());
        wire(T_END, &p)
    }

    /// Serves one session: HELLO, the whole capture as one DATA frame, then END after `end_after` (or never: holds
    /// the connection until the client closes it).
    fn agent(cap: Vec<u8>, end_after: Option<Duration>) -> u16 {
        let l = TcpListener::bind("127.0.0.1:0").unwrap();
        let port = l.local_addr().unwrap().port();
        std::thread::spawn(move || {
            let (mut s, _) = l.accept().unwrap();
            let mut h = [0u8; 16];
            s.read_exact(&mut h).unwrap();
            let mut r = s.try_clone().unwrap();
            std::thread::spawn(move || {
                let mut f = [0u8; 256];
                while matches!(r.read(&mut f), Ok(n) if n > 0) {}
            });
            s.write_all(&hello_frame()).unwrap();
            s.write_all(&data_frame(&cap)).unwrap();
            match end_after {
                Some(d) => {
                    std::thread::sleep(d);
                    let _ = s.write_all(&end_frame(cap.len() as u64));
                    let _ = s.shutdown(std::net::Shutdown::Both);
                }
                None => std::thread::sleep(Duration::from_secs(20)),
            }
        });
        port
    }

    fn capture_bytes() -> Vec<u8> {
        let p = write_capture("live-src");
        std::fs::read(p).unwrap()
    }

    fn connect(app: &mut App, port: u16, dir: &std::path::Path) {
        let form = ConnectForm { host: "127.0.0.1".into(), port: port.to_string(), dir: dir.display().to_string(), error: None };
        app.start_live(&form).unwrap();
    }

    /// Polls the app (the update loop's poll, without egui) until `done` (a harness deadline, not a product timeout).
    fn poll_until(app: &mut App, what: &str, mut done: impl FnMut(&App) -> bool) {
        let t = Instant::now();
        while !done(app) {
            assert!(t.elapsed() < Duration::from_secs(15), "HARNESS-TIMEOUT waiting for {what}");
            app.poll();
            std::thread::sleep(Duration::from_millis(20));
        }
    }

    fn out_dir(name: &str) -> std::path::PathBuf {
        let d = PathBuf::from(env!("CARGO_TARGET_TMPDIR")).join(format!("capture-live-{name}-{}-{}", std::process::id(), Instant::now().elapsed().as_nanos()));
        std::fs::create_dir_all(&d).unwrap();
        d
    }

    fn frames_files(d: &std::path::Path) -> Vec<std::path::PathBuf> {
        std::fs::read_dir(d).unwrap().filter_map(|e| e.ok()).map(|e| e.path()).filter(|p| p.to_string_lossy().ends_with(".frames.txt")).collect()
    }

    #[test]
    fn a_snapshot_taken_after_the_session_ended_is_bookmarked_at_once() {
        let d = out_dir("after");
        let mut app = App::new(None);
        connect(&mut app, agent(capture_bytes(), Some(Duration::from_millis(50))), &d);
        poll_until(&mut app, "the outcome", |a| a.live.as_ref().is_some_and(|l| l.client.finished()));
        app.poll();
        assert!(frames_files(&d).is_empty(), "no snapshot yet, no bookmark file");
        app.capture_frame();
        assert!(app.shown.is_some(), "{:?}", app.captures.message);
        let f = frames_files(&d);
        assert_eq!(f.len(), 1, "persisted immediately: {:?}", app.captures.message);
        let p = ps3dk_prof_viewer::capture::parse(&std::fs::read(&f[0]).unwrap());
        assert_eq!((p.records.len(), p.refused.len()), (1, 0));
    }

    #[test]
    fn replacing_an_unfinished_session_keeps_its_snapshots_until_its_file_is_published() {
        let d = out_dir("retire");
        let mut app = App::new(None);
        connect(&mut app, agent(capture_bytes(), None), &d);
        poll_until(&mut app, "a snapshot with a measured frame", |a| a.capture_blocked().is_none());
        app.capture_frame();
        assert_eq!(app.captures.list.len(), 1, "{:?}", app.captures.message);
        // open a file while the session still runs: it is retired, not discarded
        app.open(write_capture("retire-other"));
        assert_eq!(app.retiring.len(), 1);
        assert!(app.captures.list.is_empty());
        // the retiring session and the new one draw on the app's one pool
        assert!(Arc::ptr_eq(app.retiring[0].1.ledger.pool(), &app.pool));
        assert!(Arc::ptr_eq(app.captures.ledger.pool(), &app.pool));
        assert_eq!(app.pool.held(), app.retiring[0].1.ledger.used + app.captures.ledger.used);
        poll_until(&mut app, "the retired session's outcome", |a| a.retiring.is_empty());
        let (bad, msg) = app.captures.message.clone().unwrap();
        assert!(!bad && msg.starts_with("Previous session: 1 snapshot bookmark(s) saved"), "{msg}");
        let f = frames_files(&d);
        assert_eq!(f.len(), 1);
        assert_eq!(ps3dk_prof_viewer::capture::parse(&std::fs::read(&f[0]).unwrap()).records.len(), 1);
    }
}
