//! The viewer's live client against the REAL C agent (libps3dkprof's
//! live_agent.c, host build). Needs the serve program built from
//! tests/host/ps3dk-prof-runtime/live_agent_serve.c:
//! `LIVE_AGENT_SERVE=/path/to/live-agent-serve cargo test --test live_e2e -- --ignored`
//! (tests/host/ps3dk-prof-runtime/live-e2e-test.py does both).

#[path = "../../ps3dk-prof/tests/common/mod.rs"]
#[rustfmt::skip]
#[allow(clippy::all)]
mod common;

use common::*;
use ps3dk_prof_viewer::live::LinkState;
use ps3dk_prof_viewer::live_client::*;
use ps3dk_prof_viewer::model::VerdictTone;
use std::io::{BufRead, BufReader};
use std::process::{Child, Command, Stdio};
use std::sync::mpsc;
use std::time::{Duration, Instant};

/// Owns the serve process: killed and reaped on drop (timeout or panic).
struct Serve(Child);

impl Drop for Serve {
    fn drop(&mut self) {
        if let Ok(None) = self.0.try_wait() {
            let _ = self.0.kill();
        }
        let _ = self.0.wait();
    }
}

#[test]
#[ignore]
fn the_client_and_the_real_agent_interoperate() {
    let serve = std::env::var("LIVE_AGENT_SERVE").expect("set LIVE_AGENT_SERVE");
    let dir = std::env::temp_dir().join(format!("ps3dk-live-e2e-{}", std::process::id()));
    let _ = std::fs::remove_dir_all(&dir);
    let agent_dir = dir.join("agent");
    let out = dir.join("out");
    std::fs::create_dir_all(&agent_dir).unwrap();
    std::fs::create_dir_all(&out).unwrap();

    let mut recs = vec![frame(0, 1000, 0)];
    let mut seq = 1;
    for i in 0..3000u64 {
        let tb = 1100 + i * 100;
        recs.extend(zone(seq, 0, 0, tb, i + 1, 0));
        recs.push(end(seq + 2, 0, 0, tb + 50, i + 1));
        seq += 3;
    }
    let cap = scenario(vec![lane(1, recs)]).bytes();
    let cap_path = dir.join("capture.prof");
    std::fs::write(&cap_path, &cap).unwrap();

    let mut child = Command::new(&serve)
        .arg(&cap_path)
        .arg(&agent_dir)
        .stdout(Stdio::piped())
        .spawn()
        .expect("start the serve program");
    // stdout is read on its own thread, so no read here can block past a deadline.
    let (tx, rx) = mpsc::channel();
    let stdout = child.stdout.take().unwrap();
    let mut serve_proc = Serve(child);
    std::thread::spawn(move || {
        for line in BufReader::new(stdout).lines().map_while(Result::ok) {
            if tx.send(line).is_err() {
                break;
            }
        }
    });
    let first = rx
        .recv_timeout(Duration::from_secs(10))
        .expect("the serve program printed its port");
    let port: u16 = first.strip_prefix("PORT ").unwrap().parse().unwrap();

    let mut c = ClientConfig::new("127.0.0.1", port, out.clone());
    c.parse_every = Duration::from_millis(50);
    c.ping_every = Duration::from_millis(250);
    let client = LiveClient::start(c).unwrap();
    let t0 = Instant::now();
    // A provisional parse of a growing capture: still streaming, some but not
    // all bytes received, and a parsed header. This fixture has one large
    // RECORDS chunk: while it is torn, metadata alone is still INCOMPLETE.
    let mut provisional = false;
    while !client.finished() {
        assert!(t0.elapsed() < Duration::from_secs(20));
        let v = client.view();
        if let (LinkState::Streaming, Some(d)) = (&v.link, &v.doc) {
            let partial = v.received > 0 && v.received < cap.len() as u64;
            let metadata = d.analysis().is_some_and(|a| {
                a.capture.header.is_some() && a.capture.trailer.is_none() && a.prefix.is_none()
            });
            provisional |= partial && metadata && d.verdict().tone == VerdictTone::Incomplete;
        }
        std::thread::sleep(Duration::from_millis(5));
    }
    let v = client.join();
    // The agent exits after the stream completes; reap it within a deadline.
    let t1 = Instant::now();
    let status = loop {
        if let Some(s) = serve_proc.0.try_wait().unwrap() {
            break s;
        }
        assert!(
            t1.elapsed() < Duration::from_secs(10),
            "the serve program did not exit"
        );
        std::thread::sleep(Duration::from_millis(10));
    };
    let served: Vec<String> = rx.try_iter().collect();
    println!(
        "E2E served: {served:?}, status {status}, client parses {}, pongs {}",
        v.parses, v.pongs
    );
    match v.outcome.as_ref().unwrap() {
        Outcome::Published(p) => assert_eq!(std::fs::read(&p.path).unwrap(), cap),
        o => panic!("{o:?}"),
    }
    assert!(
        matches!(v.link, LinkState::Ended(e) if e.io_finalized && e.sent_bytes == cap.len() as u64)
    );
    assert_eq!(v.hello.as_ref().unwrap().app, "e2e");
    assert!(provisional, "a provisional parse of the growing capture");
    assert!(v.pongs > 0, "the real agent answered PINGs");
    assert!(
        status.success(),
        "the agent saw the stream complete and stopped cleanly"
    );
    let _ = std::fs::remove_dir_all(&dir);
}

/// The client against the real runtime through the PUBLIC live API
/// (ps3dk_prof_live_start/stop): the runtime writes its own capture while the
/// viewer watches, then publishes its finalization record at shutdown. Needs
/// the serve program built from tests/host/ps3dk-prof-runtime/live_api_serve.c:
/// `LIVE_API_SERVE=/path/to/live-api-serve cargo test --test live_e2e -- --ignored`.
#[test]
#[ignore]
fn the_client_and_the_runtime_live_api_interoperate() {
    let serve = std::env::var("LIVE_API_SERVE").expect("set LIVE_API_SERVE");
    let dir = std::env::temp_dir().join(format!("ps3dk-live-api-e2e-{}", std::process::id()));
    let _ = std::fs::remove_dir_all(&dir);
    let agent_dir = dir.join("agent");
    let out = dir.join("out");
    std::fs::create_dir_all(&agent_dir).unwrap();
    std::fs::create_dir_all(&out).unwrap();

    let mut child = Command::new(&serve)
        .arg(&agent_dir)
        .stdout(Stdio::piped())
        .spawn()
        .expect("start the serve program");
    let (tx, rx) = mpsc::channel();
    let stdout = child.stdout.take().unwrap();
    let mut serve_proc = Serve(child);
    std::thread::spawn(move || {
        for line in BufReader::new(stdout).lines().map_while(Result::ok) {
            if tx.send(line).is_err() {
                break;
            }
        }
    });
    let first = rx
        .recv_timeout(Duration::from_secs(10))
        .expect("the serve program printed its port");
    let port: u16 = first.strip_prefix("PORT ").unwrap().parse().unwrap();

    let mut c = ClientConfig::new("127.0.0.1", port, out.clone());
    c.parse_every = Duration::from_millis(50);
    c.ping_every = Duration::from_millis(250);
    let client = LiveClient::start(c).unwrap();
    let t0 = Instant::now();
    // Unlike the single-chunk fixture above, the runtime flushes whole event
    // chunks as it goes: those bytes form an accepted prefix before END. The
    // link remains Streaming; final publication is checked separately below.
    let mut provisional = false;
    while !client.finished() {
        assert!(t0.elapsed() < Duration::from_secs(20));
        let v = client.view();
        if let (LinkState::Streaming, Some(d)) = (&v.link, &v.doc) {
            let prefix = d.analysis().is_some_and(|a| {
                a.capture.header.is_some() && a.capture.trailer.is_none() && a.prefix.is_some()
            });
            provisional |= v.received > 0 && prefix && d.verdict().tone == VerdictTone::CompleteEarly;
        }
        std::thread::sleep(Duration::from_millis(5));
    }
    let v = client.join();
    let t1 = Instant::now();
    let status = loop {
        if let Some(s) = serve_proc.0.try_wait().unwrap() {
            break s;
        }
        assert!(
            t1.elapsed() < Duration::from_secs(10),
            "the serve program did not exit"
        );
        std::thread::sleep(Duration::from_millis(10));
    };
    let served: Vec<String> = rx.try_iter().collect();
    println!(
        "E2E-API served: {served:?}, status {status}, client parses {}, pongs {}",
        v.parses, v.pongs
    );
    let capture = served
        .iter()
        .find_map(|l| l.strip_prefix("CAPTURE "))
        .expect("the serve program named its capture");
    let written = std::fs::read(capture).unwrap();
    assert!(!written.is_empty());
    match v.outcome.as_ref().unwrap() {
        Outcome::Published(p) => assert_eq!(std::fs::read(&p.path).unwrap(), written),
        o => panic!("{o:?}"),
    }
    assert!(matches!(v.link, LinkState::Ended(e)
        if e.io_finalized && e.shutdown_result == 0 && e.accepted_bytes == written.len() as u64
            && e.sent_bytes == written.len() as u64));
    assert_eq!(v.hello.as_ref().unwrap().app, "e2e-api");
    assert!(provisional, "a provisional parse of the growing capture");
    assert!(v.pongs > 0, "the real agent answered PINGs");
    assert!(
        status.success(),
        "the runtime shut down, END was sent and the live link stopped cleanly"
    );
    let _ = std::fs::remove_dir_all(&dir);
}

/// Capture triggers end to end: the viewer's live client against the real runtime through the public live API
/// (protocol 2).  The app asks for no window; the viewer begins one, is refused a second (EBUSY), ends the first,
/// then asks for a 5-frame window, and both appear in the published capture, named as sent: the first closed by END,
/// the second by COUNT.  Needs the serve program built from tests/host/ps3dk-prof-runtime/live_trigger_serve.c:
/// `LIVE_TRIGGER_SERVE=/path/to/live-trigger-serve cargo test --test live_e2e -- --ignored`.
#[test]
#[ignore]
fn the_client_triggers_capture_windows_in_the_real_runtime() {
    use ps3dk_prof::{WindowKind, WindowReason};
    use ps3dk_prof_viewer::live::Command;
    use ps3dk_prof_viewer::live_triggers::{Availability, Status};

    let serve = std::env::var("LIVE_TRIGGER_SERVE").expect("set LIVE_TRIGGER_SERVE");
    let dir = std::env::temp_dir().join(format!("ps3dk-live-trigger-e2e-{}", std::process::id()));
    let _ = std::fs::remove_dir_all(&dir);
    let agent_dir = dir.join("agent");
    let out = dir.join("out");
    std::fs::create_dir_all(&agent_dir).unwrap();
    std::fs::create_dir_all(&out).unwrap();

    let mut child = std::process::Command::new(&serve)
        .arg(&agent_dir)
        .arg("4")
        .stdout(Stdio::piped())
        .spawn()
        .expect("start the serve program");
    let (tx, rx) = mpsc::channel();
    let stdout = child.stdout.take().unwrap();
    let mut serve_proc = Serve(child);
    std::thread::spawn(move || {
        for line in BufReader::new(stdout).lines().map_while(Result::ok) {
            if tx.send(line).is_err() {
                break;
            }
        }
    });
    let first = rx.recv_timeout(Duration::from_secs(10)).expect("the serve program printed its port");
    let port: u16 = first.strip_prefix("PORT ").unwrap().parse().unwrap();

    let mut c = ClientConfig::new("127.0.0.1", port, out.clone());
    c.parse_every = Duration::from_millis(50);
    c.ping_every = Duration::from_millis(250);
    let client = LiveClient::start(c).unwrap();
    let t0 = Instant::now();
    while client.view().triggers.availability != Availability::Ready {
        assert!(t0.elapsed() < Duration::from_secs(10), "{:?}", client.view().triggers);
        std::thread::sleep(Duration::from_millis(5));
    }
    let await_answer = |what: &str| -> Status {
        let t = Instant::now();
        loop {
            let s = client.view().triggers.status;
            if !matches!(s, Status::Pending { .. }) {
                return s;
            }
            assert!(t.elapsed() < Duration::from_secs(10), "no answer to {what}");
            std::thread::sleep(Duration::from_millis(2));
        }
    };
    client.send(Command::Begin { name: "pc-window".into() }).unwrap();
    let w1 = match await_answer("Begin") {
        Status::Acked { window_id, .. } => window_id,
        s => panic!("{s:?}"),
    };
    client.send(Command::Begin { name: "again".into() }).unwrap();
    match await_answer("the second Begin") {
        Status::Refused { code, .. } => assert_eq!(code, -2, "EBUSY"),
        s => panic!("{s:?}"),
    }
    std::thread::sleep(Duration::from_millis(50)); // some frames inside the window
    client.send(Command::End).unwrap();
    match await_answer("End") {
        Status::Acked { window_id, command: Command::End } => assert_eq!(window_id, w1),
        s => panic!("{s:?}"),
    }
    client.send(Command::Frames { name: "pc-frames".into(), frames: 5 }).unwrap();
    let w2 = match await_answer("Frames") {
        Status::Acked { window_id, .. } => window_id,
        s => panic!("{s:?}"),
    };
    assert_eq!(w2, w1 + 1);
    while !client.finished() {
        assert!(t0.elapsed() < Duration::from_secs(40));
        std::thread::sleep(Duration::from_millis(5));
    }
    let v = client.join();
    let t1 = Instant::now();
    let status = loop {
        if let Some(s) = serve_proc.0.try_wait().unwrap() {
            break s;
        }
        assert!(t1.elapsed() < Duration::from_secs(10), "the serve program did not exit");
        std::thread::sleep(Duration::from_millis(10));
    };
    let served: Vec<String> = rx.try_iter().collect();
    let doc = v.doc.as_ref().expect("a final parse");
    let windows = doc.analysis().expect("analysed").capture.windows.clone();
    println!("E2E-TRIGGER served: {served:?}, status {status}, windows {windows:?}");
    let capture = served.iter().find_map(|l| l.strip_prefix("CAPTURE ")).expect("the serve program named its capture");
    let written = std::fs::read(capture).unwrap();
    match v.outcome.as_ref().unwrap() {
        Outcome::Published(p) => assert_eq!(std::fs::read(&p.path).unwrap(), written),
        o => panic!("{o:?}"),
    }
    assert!(status.success(), "every ACK was sent and the live link stopped cleanly");
    assert_eq!(windows.len(), 2, "only the windows the viewer opened");
    let a = &windows[0];
    assert_eq!((a.id, a.name.as_str(), a.kind.clone(), a.reason), (w1, "pc-window", WindowKind::Manual, WindowReason::End));
    assert!(!a.is_open && !a.incomplete && a.last_frame.unwrap() >= a.first_frame);
    let b = &windows[1];
    assert_eq!((b.id, b.name.as_str(), b.kind.clone(), b.reason), (w2, "pc-frames", WindowKind::Frames(5), WindowReason::Count));
    assert_eq!(b.last_frame.unwrap() - b.first_frame + 1, 5);
    assert!(b.first_frame > a.last_frame.unwrap());
    let _ = std::fs::remove_dir_all(&dir);
}
