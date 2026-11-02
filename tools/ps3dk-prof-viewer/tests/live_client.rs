//! The live client against a scripted mock agent on loopback: publication
//! order, failures, cancellation, pings and provisional parses.

#[path = "../../ps3dk-prof/tests/common/mod.rs"]
#[rustfmt::skip]
#[allow(clippy::all)]
mod common;

use common::*;
use ps3dk_prof_viewer::live::*;
use ps3dk_prof_viewer::live_client::*;
use ps3dk_prof_viewer::model::VerdictTone;
use std::io::{Read, Write};
use std::net::{TcpListener, TcpStream};
use std::path::{Path, PathBuf};
use std::sync::atomic::{AtomicU64, Ordering};
use std::sync::Arc;
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

fn data_frames(cap: &[u8], chunk: usize) -> Vec<Vec<u8>> {
    cap.chunks(chunk)
        .enumerate()
        .map(|(i, c)| {
            let mut p = ((i * chunk) as u64).to_le_bytes().to_vec();
            p.extend_from_slice(c);
            wire(T_DATA, &p)
        })
        .collect()
}

fn end_frame(len: u64) -> Vec<u8> {
    let mut p = vec![1, 1, 0, 0];
    p.extend_from_slice(&0i32.to_le_bytes());
    p.extend_from_slice(&len.to_le_bytes());
    p.extend_from_slice(&len.to_le_bytes());
    wire(T_END, &p)
}

/// A valid capture with `zones` zone instances.
fn capture(zones: u64) -> Vec<u8> {
    let mut recs = vec![frame(0, 1000, 0)];
    let mut seq = 1;
    for i in 0..zones {
        let tb = 1100 + i * 100;
        recs.extend(zone(seq, 0, 0, tb, i + 1, 0));
        recs.push(end(seq + 2, 0, 0, tb + 50, i + 1));
        seq += 3;
    }
    scenario(vec![lane(1, recs)]).bytes()
}

enum Step {
    Send(Vec<u8>),
    Sleep(u64),
    /// Keep the connection open this long, then close.
    Hold(u64),
    /// Close with a TCP reset instead of a clean FIN.
    #[cfg(unix)]
    Reset,
}

/// Arms SO_LINGER {on, 0}, so dropping the socket sends a reset.
#[cfg(unix)]
fn arm_reset(s: &TcpStream) {
    use std::os::fd::AsRawFd;
    #[repr(C)]
    struct Linger {
        on: i32,
        secs: i32,
    }
    extern "C" {
        fn setsockopt(fd: i32, level: i32, name: i32, val: *const Linger, len: u32) -> i32;
    }
    const SOL_SOCKET: i32 = 1; // Linux
    const SO_LINGER: i32 = 13; // Linux
    let l = Linger { on: 1, secs: 0 };
    // SAFETY: a valid socket fd and a correctly sized struct.
    let rc = unsafe {
        setsockopt(
            s.as_raw_fd(),
            SOL_SOCKET,
            SO_LINGER,
            &l,
            std::mem::size_of::<Linger>() as u32,
        )
    };
    assert_eq!(rc, 0);
}

/// Serves one connection: checks CLIENT_HELLO, runs the script, closes.
/// Returns the port and a counter of PINGs received.
fn mock(script: Vec<Step>) -> (u16, Arc<AtomicU64>) {
    let l = TcpListener::bind("127.0.0.1:0").unwrap();
    let port = l.local_addr().unwrap().port();
    let pings = Arc::new(AtomicU64::new(0));
    let p = pings.clone();
    std::thread::spawn(move || {
        let (mut s, _) = l.accept().unwrap();
        let mut h = [0u8; 16];
        s.read_exact(&mut h).unwrap();
        assert_eq!(h.to_vec(), client_hello());
        // Count PINGs in the background.
        let mut r = s.try_clone().unwrap();
        std::thread::spawn(move || {
            let mut f = [0u8; 8];
            while r.read_exact(&mut f).is_ok() {
                if u16::from_le_bytes([f[0], f[1]]) == T_PING {
                    p.fetch_add(1, Ordering::SeqCst);
                }
            }
        });
        for step in script {
            match step {
                Step::Send(b) => {
                    if s.write_all(&b).is_err() {
                        return;
                    }
                }
                Step::Sleep(ms) => std::thread::sleep(Duration::from_millis(ms)),
                Step::Hold(ms) => std::thread::sleep(Duration::from_millis(ms)),
                #[cfg(unix)]
                Step::Reset => {
                    arm_reset(&s);
                    // End the PING counter so its cloned handle is closed;
                    // then dropping `s` closes the last handle: a reset.
                    let _ = s.shutdown(std::net::Shutdown::Read);
                    std::thread::sleep(Duration::from_millis(30));
                    return;
                }
            }
        }
        let _ = s.shutdown(std::net::Shutdown::Both);
    });
    (port, pings)
}

fn out_dir(name: &str) -> PathBuf {
    let d = std::env::temp_dir().join(format!("ps3dk-live-client-{}-{name}", std::process::id()));
    let _ = std::fs::remove_dir_all(&d);
    std::fs::create_dir_all(&d).unwrap();
    d
}

/// Compliant with the agent: at most 4 PINGs a second.
fn cfg(port: u16, dir: &Path) -> ClientConfig {
    let mut c = ClientConfig::new("127.0.0.1", port, dir.to_path_buf());
    c.parse_every = Duration::from_millis(30);
    c.close_wait = Duration::from_millis(300);
    c.ping_every = Duration::from_millis(250);
    c.hello_timeout = Duration::from_millis(800);
    c.liveness_timeout = Duration::from_millis(1500);
    c
}

fn start(c: ClientConfig) -> LiveClient {
    LiveClient::start(c).expect("a valid config")
}

/// Polls until the client has finished (or panics after 10 s), returning
/// every verdict tone seen along the way.
fn wait(c: &LiveClient) -> Vec<VerdictTone> {
    let t0 = Instant::now();
    let mut tones = Vec::new();
    let mut last_gen = 0;
    while !c.finished() {
        assert!(
            t0.elapsed() < Duration::from_secs(10),
            "client did not finish"
        );
        let v = c.view();
        if v.generation != last_gen {
            last_gen = v.generation;
            if let Some(d) = &v.doc {
                tones.push(d.verdict().tone);
            }
        }
        std::thread::sleep(Duration::from_millis(5));
    }
    tones
}

fn files(dir: &Path) -> Vec<String> {
    let mut v: Vec<String> = std::fs::read_dir(dir)
        .unwrap()
        .map(|e| e.unwrap().file_name().to_string_lossy().into_owned())
        .collect();
    v.sort();
    v
}

fn complete_files(dir: &Path) -> Vec<String> {
    files(dir)
        .into_iter()
        .filter(|f| f.ends_with(".prof") && !f.ends_with(".incomplete.prof"))
        .collect()
}

fn reasons(o: &Outcome) -> String {
    match o {
        Outcome::Incomplete { reasons, .. } => reasons.join("; "),
        Outcome::Published(p) => panic!("unexpectedly published {:?}", p.path),
    }
}

#[test]
fn a_clean_stream_is_parsed_provisionally_then_published() {
    let cap = capture(400);
    let mut script = vec![Step::Send(hello_frame())];
    let frames = data_frames(&cap, 97);
    let n_frames = frames.len();
    for f in frames {
        script.push(Step::Send(f));
        script.push(Step::Sleep(2));
    }
    script.push(Step::Send(end_frame(cap.len() as u64)));
    let (port, _) = mock(script);
    let dir = out_dir("clean");
    let c = start(cfg(port, &dir));
    let tones = wait(&c);
    let v = c.view();
    match v.outcome.as_ref().unwrap() {
        Outcome::Published(p) => {
            assert!(p.cleanup_warning.is_none());
            assert_eq!(std::fs::read(&p.path).unwrap(), cap);
            assert_eq!(
                p.path.file_name().unwrap(),
                "0000000000000042-heterogeneous-1.prof"
            );
        }
        o => panic!("{o:?}"),
    }
    assert!(matches!(v.link, LinkState::Ended(_)));
    // Provisional (INCOMPLETE) parses while streaming, COMPLETE at the end,
    // and far fewer parses than DATA frames (coalesced).
    assert!(tones.contains(&VerdictTone::Incomplete), "{tones:?}");
    assert_eq!(
        v.doc.as_ref().unwrap().verdict().tone,
        VerdictTone::Complete
    );
    assert!(
        v.parses >= 2 && (v.parses as usize) < n_frames / 4,
        "{} parses for {n_frames} frames",
        v.parses
    );
    assert_eq!(
        files(&dir),
        vec!["0000000000000042-heterogeneous-1.prof".to_string()]
    );
}

#[test]
fn a_byte_after_end_in_a_later_read_is_never_published() {
    let cap = capture(20);
    let mut script = vec![Step::Send(hello_frame())];
    script.extend(data_frames(&cap, 512).into_iter().map(Step::Send));
    script.push(Step::Send(end_frame(cap.len() as u64)));
    script.push(Step::Sleep(150)); // a separate read on the client
    script.push(Step::Send(vec![0]));
    let (port, _) = mock(script);
    let dir = out_dir("after-end");
    let c = start(cfg(port, &dir));
    wait(&c);
    let v = c.view();
    let why = reasons(v.outcome.as_ref().unwrap());
    assert!(why.contains("after END"), "{why}");
    assert!(complete_files(&dir).is_empty(), "{:?}", files(&dir));
    assert_eq!(
        files(&dir),
        vec!["0000000000000042-heterogeneous-1.incomplete.prof".to_string()]
    );
}

#[test]
fn an_agent_that_does_not_close_after_end_is_not_published() {
    let cap = capture(20);
    let mut script = vec![Step::Send(hello_frame())];
    script.extend(data_frames(&cap, 512).into_iter().map(Step::Send));
    script.push(Step::Send(end_frame(cap.len() as u64)));
    script.push(Step::Hold(1500)); // close_wait is 300 ms
    let (port, _) = mock(script);
    let dir = out_dir("no-close");
    let c = start(cfg(port, &dir));
    wait(&c);
    let why = reasons(c.view().outcome.as_ref().unwrap());
    assert!(why.contains("did not close"), "{why}");
    assert!(complete_files(&dir).is_empty());
}

#[test]
fn a_close_before_end_keeps_an_incomplete_copy() {
    let cap = capture(50);
    let frames = data_frames(&cap, 256);
    let half = frames.len() / 2;
    let mut script = vec![Step::Send(hello_frame())];
    script.extend(frames.into_iter().take(half).map(Step::Send));
    let (port, _) = mock(script);
    let dir = out_dir("early-close");
    let c = start(cfg(port, &dir));
    wait(&c);
    let v = c.view();
    match v.outcome.as_ref().unwrap() {
        Outcome::Incomplete {
            saved: Some(p),
            reasons,
        } => {
            assert!(p.path.to_string_lossy().ends_with(".incomplete.prof"));
            assert_eq!(std::fs::read(&p.path).unwrap(), &cap[..half * 256]);
            assert!(
                reasons.iter().any(|r| r.contains("closed before END")),
                "{reasons:?}"
            );
        }
        o => panic!("{o:?}"),
    }
    assert!(complete_files(&dir).is_empty());
    // The provisional view says INCOMPLETE, never COMPLETE.
    assert_ne!(v.doc.unwrap().verdict().tone, VerdictTone::Complete);
}

#[test]
fn busy_saves_nothing_and_says_why() {
    let (port, _) = mock(vec![Step::Send(wire(T_BUSY, &1u32.to_le_bytes()))]);
    let dir = out_dir("busy");
    let c = start(cfg(port, &dir));
    wait(&c);
    match c.view().outcome.unwrap() {
        Outcome::Incomplete {
            saved: None,
            reasons,
        } => {
            assert!(reasons.join(" ").contains("BUSY"), "{reasons:?}");
        }
        o => panic!("{o:?}"),
    }
    assert!(files(&dir).is_empty());
}

#[test]
fn disconnect_ends_promptly_and_keeps_what_arrived() {
    let cap = capture(50);
    let mut script = vec![Step::Send(hello_frame())];
    for f in data_frames(&cap, 64) {
        script.push(Step::Send(f));
        script.push(Step::Sleep(50)); // slow: the user leaves first
    }
    let (port, _) = mock(script);
    let dir = out_dir("disconnect");
    let c = start(cfg(port, &dir));
    std::thread::sleep(Duration::from_millis(300));
    let t0 = Instant::now();
    c.disconnect();
    wait(&c);
    assert!(t0.elapsed() < Duration::from_secs(1), "{:?}", t0.elapsed());
    let why = reasons(c.view().outcome.as_ref().unwrap());
    assert!(why.contains("disconnected by the user"), "{why}");
    assert!(complete_files(&dir).is_empty());
}

#[test]
fn the_client_pings_within_the_agents_limit() {
    let cap = capture(5);
    let mut script = vec![Step::Send(hello_frame()), Step::Sleep(1100)];
    script.extend(data_frames(&cap, 512).into_iter().map(Step::Send));
    script.push(Step::Send(end_frame(cap.len() as u64)));
    let (port, pings) = mock(script);
    let dir = out_dir("ping");
    let c = start(cfg(port, &dir));
    wait(&c);
    assert!(matches!(c.view().outcome, Some(Outcome::Published(_))));
    let n = pings.load(Ordering::SeqCst);
    // ~1.1 s at 250 ms: 4 or 5 PINGs, never more than the agent allows.
    assert!((3..=5).contains(&n), "{n} pings in ~1.1 s at 250 ms");
}

#[test]
fn a_refused_connection_is_reported() {
    let port = {
        let l = TcpListener::bind("127.0.0.1:0").unwrap();
        l.local_addr().unwrap().port()
    }; // closed again: nothing listens
    let dir = out_dir("refused");
    let c = start(cfg(port, &dir));
    wait(&c);
    let why = reasons(c.view().outcome.as_ref().unwrap());
    assert!(why.contains("could not connect"), "{why}");
    assert!(files(&dir).is_empty());
}

#[cfg(unix)]
#[test]
fn a_reset_after_end_is_never_published() {
    let cap = capture(20);
    let mut script = vec![Step::Send(hello_frame())];
    script.extend(data_frames(&cap, 512).into_iter().map(Step::Send));
    script.push(Step::Send(end_frame(cap.len() as u64)));
    script.push(Step::Sleep(50));
    script.push(Step::Reset);
    let (port, _) = mock(script);
    let dir = out_dir("reset");
    let c = start(cfg(port, &dir));
    wait(&c);
    let why = reasons(c.view().outcome.as_ref().unwrap());
    assert!(why.contains("connection error"), "{why}");
    assert!(complete_files(&dir).is_empty(), "{:?}", files(&dir));
}

#[test]
fn a_local_file_that_cannot_be_created_is_latched_for_the_session() {
    let cap = capture(50);
    let frames = data_frames(&cap, 256);
    let half = frames.len() / 2;
    let mut script = vec![Step::Send(hello_frame())];
    let mut it = frames.into_iter();
    script.extend(it.by_ref().take(half).map(Step::Send));
    script.push(Step::Sleep(400)); // the test removes the obstacle here
    script.extend(it.map(Step::Send));
    script.push(Step::Send(end_frame(cap.len() as u64)));
    let dir = out_dir("create-fail");
    // An existing .partial makes the exclusive create fail.
    let obstacle = dir.join("0000000000000042-heterogeneous-1.partial");
    std::fs::write(&obstacle, b"someone else's file").unwrap();
    let (port, _) = mock(script);
    let c = start(cfg(port, &dir));
    let t0 = Instant::now();
    while c.view().local_error.is_none() {
        assert!(t0.elapsed() < Duration::from_secs(5));
        std::thread::sleep(Duration::from_millis(5));
    }
    std::fs::remove_file(&obstacle).unwrap(); // the environment recovers
    wait(&c);
    match c.view().outcome.unwrap() {
        Outcome::Incomplete {
            saved: None,
            reasons,
        } => {
            assert!(
                reasons.iter().any(|r| r.contains("could not create")),
                "{reasons:?}"
            );
        }
        o => panic!("{o:?}"),
    }
    // No retry wrote a truncated file later.
    assert!(files(&dir).is_empty(), "{:?}", files(&dir));
}

#[test]
fn no_hello_in_time_ends_the_session() {
    let (port, _) = mock(vec![Step::Hold(3000)]);
    let dir = out_dir("no-hello");
    let t0 = Instant::now();
    let c = start(cfg(port, &dir));
    wait(&c);
    assert!(
        t0.elapsed() < Duration::from_millis(2000),
        "{:?}",
        t0.elapsed()
    );
    let why = reasons(c.view().outcome.as_ref().unwrap());
    assert!(why.contains("no HELLO"), "{why}");
}

#[test]
fn a_silent_streaming_peer_ends_the_session() {
    // HELLO, then nothing: no data and no PONG answers.
    let (port, pings) = mock(vec![Step::Send(hello_frame()), Step::Hold(4000)]);
    let dir = out_dir("silent");
    let t0 = Instant::now();
    let c = start(cfg(port, &dir));
    wait(&c);
    assert!(
        t0.elapsed() < Duration::from_millis(3000),
        "{:?}",
        t0.elapsed()
    );
    let why = reasons(c.view().outcome.as_ref().unwrap());
    assert!(why.contains("stopped responding"), "{why}");
    assert!(pings.load(Ordering::SeqCst) >= 3);
}

#[test]
fn disconnect_during_the_handshake_is_prompt() {
    let (port, _) = mock(vec![Step::Hold(3000)]);
    let dir = out_dir("cancel-handshake");
    let c = start(cfg(port, &dir));
    std::thread::sleep(Duration::from_millis(100));
    let t0 = Instant::now();
    c.disconnect();
    wait(&c);
    assert!(
        t0.elapsed() < Duration::from_millis(500),
        "{:?}",
        t0.elapsed()
    );
    let why = reasons(c.view().outcome.as_ref().unwrap());
    assert!(why.contains("disconnected by the user"), "{why}");
}

#[test]
fn dropping_a_client_does_not_block() {
    let (port, _) = mock(vec![Step::Send(hello_frame()), Step::Hold(3000)]);
    let dir = out_dir("drop");
    let c = start(cfg(port, &dir));
    std::thread::sleep(Duration::from_millis(100));
    let t0 = Instant::now();
    drop(c);
    assert!(
        t0.elapsed() < Duration::from_millis(50),
        "{:?}",
        t0.elapsed()
    );
}

#[test]
fn configs_outside_the_protocol_are_refused() {
    let dir = out_dir("config");
    let ok = cfg(1, &dir);
    assert!(ok.validate().is_ok());
    let mut bad = Vec::new();
    let mut c = ok.clone();
    c.ping_every = Duration::from_millis(200); // 5 a second: over the agent's 4
    bad.push(c);
    let mut c = ok.clone();
    c.host = "localhost".into(); // a name would need an unbounded lookup
    bad.push(c);
    let mut c = ok.clone();
    c.cap = 0;
    bad.push(c);
    let mut c = ok.clone();
    c.cap = MAX_SESSION_BYTES + 1;
    bad.push(c);
    let mut c = ok.clone();
    c.liveness_timeout = c.ping_every;
    bad.push(c);
    let mut c = ok.clone();
    c.connect_timeout = Duration::ZERO;
    bad.push(c);
    let mut c = ok.clone();
    c.port = 0;
    bad.push(c);
    for c in bad {
        assert!(LiveClient::start(c.clone()).is_err(), "{c:?}");
    }
    // The client's PING limit is the agent's.
    let h = std::fs::read_to_string(concat!(
        env!("CARGO_MANIFEST_DIR"),
        "/../../sdk/libps3dkprof/src/live/live_proto.h"
    ))
    .unwrap();
    assert!(h.contains(&format!("#define LIVE_MAX_PINGS_PER_S {MAX_PINGS_PER_S}u")));
}
