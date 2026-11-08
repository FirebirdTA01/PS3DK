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
    // all bytes received, and a parsed (not empty) INCOMPLETE document.
    let mut provisional = false;
    while !client.finished() {
        assert!(t0.elapsed() < Duration::from_secs(20));
        let v = client.view();
        if let (LinkState::Streaming, Some(d)) = (&v.link, &v.doc) {
            let partial = v.received > 0 && v.received < cap.len() as u64;
            let parsed = d.analysis().is_some_and(|a| a.capture.header.is_some());
            provisional |= partial && parsed && d.verdict().tone == VerdictTone::Incomplete;
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
    let mut provisional = false;
    while !client.finished() {
        assert!(t0.elapsed() < Duration::from_secs(20));
        let v = client.view();
        if let (LinkState::Streaming, Some(d)) = (&v.link, &v.doc) {
            let parsed = d.analysis().is_some_and(|a| a.capture.header.is_some());
            provisional |= v.received > 0 && parsed && d.verdict().tone == VerdictTone::Incomplete;
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
