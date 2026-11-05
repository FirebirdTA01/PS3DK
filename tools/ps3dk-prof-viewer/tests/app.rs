//! App-level transitions without a window: the Connect dialog's checks,
//! what a failed or successful start does to the open document, the save
//! folder rules, and the live outcome line.

#[path = "../../ps3dk-prof/tests/common/mod.rs"]
#[rustfmt::skip]
#[allow(clippy::all)]
mod common;

use common::*;
use ps3dk_prof_viewer::app::*;
use ps3dk_prof_viewer::live::{LinkState, Published};
use ps3dk_prof_viewer::live_client::{LiveView, Outcome};
use std::path::{Path, PathBuf};
use std::sync::Arc;

fn scratch(name: &str) -> PathBuf {
    let d = std::env::temp_dir().join(format!("ps3dk-app-{}-{name}", std::process::id()));
    let _ = std::fs::remove_dir_all(&d);
    std::fs::create_dir_all(&d).unwrap();
    d
}

/// An App with a capture open and a zone selected.
fn app_with_capture(dir: &Path) -> App {
    let mut recs = vec![frame(0, 1000, 0)];
    recs.extend(zone(1, 0, 0, 1100, 1, 0));
    recs.push(end(3, 0, 0, 1200, 1));
    let path = dir.join("open.prof");
    std::fs::write(&path, scenario(vec![lane(1, recs)]).bytes()).unwrap();
    let mut app = App::new(Some(path));
    assert!(app.doc.is_some());
    app.selected_zone = Some(("zoneA".into(), String::new()));
    app
}

/// A port nothing listens on (a refused connection is fine for these tests).
fn closed_port() -> String {
    let l = std::net::TcpListener::bind("127.0.0.1:0").unwrap();
    l.local_addr().unwrap().port().to_string()
}

fn form(host: &str, port: &str, dir: &str) -> ConnectForm {
    ConnectForm {
        host: host.into(),
        port: port.into(),
        dir: dir.into(),
        error: None,
    }
}

#[test]
fn an_invalid_address_leaves_the_open_capture_alone() {
    let d = scratch("invalid");
    let mut app = app_with_capture(&d);
    let doc = Arc::as_ptr(app.doc.as_ref().unwrap());
    let out = d.join("captures").display().to_string();
    for bad in [
        form("localhost", "31750", &out),
        form("", "31750", &out),
        form("192.168.1.41", "0", &out),
        form("192.168.1.41", "x", &out),
    ] {
        let e = app.start_live(&bad).unwrap_err();
        assert!(!e.is_empty());
        assert_eq!(Arc::as_ptr(app.doc.as_ref().unwrap()), doc, "document kept");
        assert_eq!(app.selected_zone, Some(("zoneA".into(), String::new())));
        assert!(app.live.is_none() && app.timeline.is_some());
    }
    assert!(
        !d.join("captures").exists(),
        "nothing created before validation"
    );
}

#[test]
fn an_invalid_address_leaves_a_running_session_alone() {
    let d = scratch("keep-session");
    let mut app = app_with_capture(&d);
    let out = d.join("captures").display().to_string();
    app.start_live(&form("127.0.0.1", &closed_port(), &out))
        .unwrap();
    let target = app.live.as_ref().unwrap().target.clone();
    assert!(app.start_live(&form("not-an-ip", "31750", &out)).is_err());
    assert_eq!(
        app.live.as_ref().unwrap().target,
        target,
        "the session was not replaced"
    );
}

#[test]
fn a_valid_start_replaces_the_view_only_after_it_starts() {
    let d = scratch("valid");
    let mut app = app_with_capture(&d);
    let out = d.join("captures");
    app.start_live(&form(
        "127.0.0.1",
        &closed_port(),
        &out.display().to_string(),
    ))
    .unwrap();
    assert!(app.live.is_some());
    assert!(app.doc.is_none() && app.timeline.is_none() && app.selected_zone.is_none());
    assert!(out.is_dir(), "the save folder is created on a valid start");
}

#[test]
fn the_save_folder_must_be_a_nonempty_absolute_path() {
    for dir in ["", "   ", "captures", "./captures", "..\\captures"] {
        let e = form_config(&form("127.0.0.1", "31750", dir)).unwrap_err();
        assert!(e.contains("folder"), "{dir:?}: {e}");
    }
    let abs = std::env::temp_dir().join("x").display().to_string();
    let cfg = form_config(&form(" 127.0.0.1 ", "31750", &abs)).unwrap();
    assert_eq!(cfg.port, 31750);
}

#[test]
fn the_default_folder_never_falls_back_to_the_launch_directory() {
    assert_eq!(default_capture_dir(None), None, "no home: the user chooses");
    let home = scratch("home");
    assert_eq!(
        default_capture_dir(Some(home.clone())).unwrap(),
        home.join("PS3DK Captures").display().to_string()
    );
    std::fs::create_dir_all(home.join("Documents")).unwrap();
    assert_eq!(
        default_capture_dir(Some(home.clone())).unwrap(),
        home.join("Documents")
            .join("PS3DK Captures")
            .display()
            .to_string()
    );
}

fn view(outcome: Outcome) -> LiveView {
    LiveView {
        link: LinkState::Handshake,
        hello: None,
        received: 0,
        pongs: 0,
        doc: None,
        generation: 0,
        parses: 0,
        local_error: None,
        outcome: Some(outcome),
        snapshot_buffers_estimate: 0,
    }
}

#[test]
fn the_outcome_line_keeps_cleanup_warnings_for_both_outcomes() {
    let saved = |w: Option<&str>| Published {
        path: PathBuf::from("/c/s.incomplete.prof"),
        cleanup_warning: w.map(str::to_string),
    };
    let (_, t) = live_line(&view(Outcome::Incomplete {
        saved: Some(saved(Some("published, but /c/s.partial was not removed"))),
        reasons: vec!["the console stopped responding".into()],
    }));
    assert!(t.contains("the console stopped responding"), "{t}");
    assert!(t.contains("kept as"), "{t}");
    assert!(t.contains("was not removed"), "{t}");
    let (_, t) = live_line(&view(Outcome::Published(Published {
        path: PathBuf::from("/c/s.prof"),
        cleanup_warning: Some("published, but /c/s.partial was not removed".into()),
    })));
    assert!(t.contains("saved") && t.contains("was not removed"), "{t}");
    let (tone, t) = live_line(&view(Outcome::PublishedEarly {
        published: Published { path: PathBuf::from("/c/s.prof"), cleanup_warning: None },
        cause: "disconnected by the user".into(),
        cut: 1000,
        torn_dropped: 7,
    }));
    assert_eq!(tone, ps3dk_ui::Tone::Success);
    assert!(t.contains("ended early (disconnected by the user)") && t.contains("byte 1000") && t.contains("7 bytes"), "{t}");
    let (_, t) = live_line(&view(Outcome::Incomplete {
        saved: None,
        reasons: vec!["nothing was saved".into()],
    }));
    assert!(
        t.contains("nothing was saved") && !t.contains("kept as"),
        "{t}"
    );
}

#[test]
fn an_empty_userprofile_does_not_hide_home() {
    use std::ffi::OsString;
    let h = |s: &str| Some(OsString::from(s));
    assert_eq!(
        home_from(h(""), h("/home/u")),
        Some(PathBuf::from("/home/u"))
    );
    assert_eq!(
        home_from(h("C:/Users/u"), h("/home/u")),
        Some(PathBuf::from("C:/Users/u"))
    );
    assert_eq!(
        home_from(None, h("/home/u")),
        Some(PathBuf::from("/home/u"))
    );
    assert_eq!(home_from(h(""), h("")), None);
    assert_eq!(home_from(None, None), None);
}
