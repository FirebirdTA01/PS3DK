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
    #[cfg(target_os = "linux")]
    Reset,
}

/// Arms SO_LINGER {on, 0}, so dropping the socket sends a reset.
#[cfg(target_os = "linux")]
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
    // Linux values; the reset case is Linux-only.
    const SOL_SOCKET: i32 = 1;
    const SO_LINGER: i32 = 13;
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
                #[cfg(target_os = "linux")]
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
        Outcome::PublishedEarly { published, .. } => panic!("unexpectedly published early {:?}", published.path),
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

#[cfg(target_os = "linux")]
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

// ---- a session that ends early after valid data (director 2026-11-04 19:30) ---------------------------------------

/// `n` lanes, one RECORDS chunk each, so a stream cut mid-capture can end after whole event chunks.
fn capture_lanes(n: u32) -> (Vec<u8>, Vec<usize>, Vec<usize>) {
    let lanes = (1..=n)
        .map(|inc| {
            let recs = seqd(
                0,
                [vec![frame(0, 1000, 0)], zone(0, 0, 0, 1100, 1, 0), vec![end(0, 0, 0, 1150, 1), frame(0, 1200, 1)]]
                    .concat(),
            );
            lane(inc, recs)
        })
        .collect();
    let f = scenario(lanes).file();
    let ends: Vec<usize> = (1..=f.chunks.len()).map(|k| File::new(f.chunks[..k].to_vec()).bytes().len()).collect();
    let rec_ends = f.chunks.iter().zip(&ends).filter(|(c, _)| c.ty == T_RECORDS).map(|(_, &e)| e).collect();
    (f.bytes(), ends, rec_ends)
}

fn early(o: &Outcome) -> (&Published, &str, u64, u64) {
    match o {
        Outcome::PublishedEarly { published, cause, cut, torn_dropped } => (published, cause, *cut, *torn_dropped),
        o => panic!("{o:?}"),
    }
}

#[test]
fn a_close_before_end_after_whole_event_chunks_is_published_early() {
    let (cap, _, rec) = capture_lanes(4);
    let sent = rec[2] - 5; // inside lane 3's RECORDS chunk: lanes 1 and 2 are whole
    let mut script = vec![Step::Send(hello_frame())];
    script.extend(data_frames(&cap[..sent], 256).into_iter().map(Step::Send));
    let (port, _) = mock(script);
    let dir = out_dir("early-publish");
    let c = start(cfg(port, &dir));
    wait(&c);
    let v = c.view();
    let (p, cause, cut, torn) = early(v.outcome.as_ref().unwrap());
    assert!(cause.contains("closed before END"), "{cause}");
    assert_eq!((cut as usize, torn as usize), (rec[1], sent - rec[1]));
    assert!(p.path.to_string_lossy().ends_with(".prof") && !p.path.to_string_lossy().ends_with(".incomplete.prof"));
    assert_eq!(std::fs::read(&p.path).unwrap(), &cap[..rec[1]], "the published bytes are the cut prefix");
    assert_eq!(complete_files(&dir).len(), 1, "{:?}", files(&dir));
    let doc = v.doc.unwrap();
    assert_eq!(doc.verdict().tone, VerdictTone::CompleteEarly);
    assert_eq!(doc.size, rec[1], "the screen shows the analysis of exactly the published bytes");
    let a = doc.analysis().unwrap();
    assert!(a.lane_valid(1) && a.lane_valid(2), "{:?}", a.lanes);
    // Reopening the published file gives the same acceptance.
    let re = ps3dk_prof_viewer::model::Document::from_bytes(p.path.clone(), &std::fs::read(&p.path).unwrap());
    assert_eq!(re.verdict().tone, VerdictTone::CompleteEarly);
    assert!(!re.verdict().text.contains("closed"), "reopen wording is cause-neutral: {}", re.verdict().text);
}

/// The app quit from the XMB: the agent sends END with `trailer_written` 0 (and a failed shutdown).  The bytes that
/// arrived are a valid prefix, so the session is published as a normal `.prof`, never `.incomplete.prof`.
/// END with every field chosen by the test.
fn end_fields(io_finalized: u8, trailer: u8, join_failed: u8, result: i32, accepted: u64, sent: u64) -> Vec<u8> {
    let mut p = vec![io_finalized, trailer, join_failed, 0];
    p.extend_from_slice(&result.to_le_bytes());
    p.extend_from_slice(&accepted.to_le_bytes());
    p.extend_from_slice(&sent.to_le_bytes());
    wire(T_END, &p)
}

/// A torn prefix of `capture_lanes(4)` (the first two RECORDS chunks whole, the third torn), streamed, then `end`
/// and `tail`.
fn trailerless_session(name: &str, end: Vec<u8>, tail: Vec<Step>, fault: Option<EarlyFault>) -> (LiveView, PathBuf) {
    let (cap, _, rec) = capture_lanes(4);
    let sent = rec[2] - 5;
    let mut script = vec![Step::Send(hello_frame())];
    script.extend(data_frames(&cap[..sent], 256).into_iter().map(Step::Send));
    script.push(Step::Send(end));
    script.extend(tail);
    let (port, _) = mock(script);
    let dir = out_dir(name);
    let mut k = cfg(port, &dir);
    k.early_fault = fault;
    let c = start(k);
    wait(&c);
    (c.view(), dir)
}

fn trailerless_sent() -> u64 {
    let (_, _, rec) = capture_lanes(4);
    (rec[2] - 5) as u64
}

/// A failure the missing trailer does not excuse: no normal `.prof`, the strict document on screen, and every
/// needle among the reported reasons.
fn assert_refused(v: &LiveView, dir: &Path, needles: &[&str]) {
    let why = reasons(v.outcome.as_ref().unwrap());
    for n in needles {
        assert!(why.contains(n), "missing '{n}' in: {why}");
    }
    assert!(complete_files(dir).is_empty(), "{:?}", files(dir));
    assert_eq!(v.doc.as_ref().unwrap().verdict().tone, VerdictTone::Incomplete, "strict document");
}

/// The app quit from the XMB: the runtime's END for a session without a trailer has `trailer_written` 0,
/// `io_finalized` 0 (the runtime sets it only with a trailer), the shutdown result `PS3DK_PROF_ESINKFAIL` (-10) and
/// no join failure.  Those three facts are the only ones forgiven: the valid prefix is published as a normal
/// `.prof`, never `.incomplete.prof`.
#[test]
fn an_end_without_a_trailer_after_valid_data_is_published_as_a_normal_prof() {
    let (cap, _, rec) = capture_lanes(4);
    let sent = trailerless_sent();
    let (v, dir) =
        trailerless_session("end-no-trailer", end_fields(0, 0, 0, SHUTDOWN_NO_TRAILER, sent, sent), vec![], None);
    let (p, cause, cut, torn) = early(v.outcome.as_ref().unwrap());
    assert!(cause.contains("without a trailer") && cause.contains("shutdown result -10"), "{cause}");
    assert_eq!((cut as usize, torn), (rec[1], sent - rec[1] as u64));
    let name = p.path.to_string_lossy().into_owned();
    assert!(name.ends_with(".prof") && !name.ends_with(".incomplete.prof"), "{name}");
    assert_eq!(std::fs::read(&p.path).unwrap(), &cap[..rec[1]]);
    assert!(!files(&dir).iter().any(|f| f.ends_with(".incomplete.prof")), "{:?}", files(&dir));
    let doc = v.doc.unwrap();
    assert_eq!(doc.verdict().tone, VerdictTone::CompleteEarly);
    assert!(doc.verdict().text.starts_with("COMPLETE (ended without a trailer"), "{}", doc.verdict().text);
}

/// A valid prefix, a whole local copy, END sent_bytes == received, but the I/O finalized with an accepted byte count
/// that is not what arrived: an integrity failure, never published.
#[test]
fn an_end_without_a_trailer_and_an_accepted_byte_mismatch_stays_incomplete() {
    let sent = trailerless_sent();
    let (v, dir) = trailerless_session(
        "end-no-trailer-accepted",
        end_fields(1, 0, 0, SHUTDOWN_NO_TRAILER, sent - 1, sent),
        vec![],
        None,
    );
    let mismatch = format!("the console wrote {} bytes, {sent} arrived", sent - 1);
    assert_refused(&v, &dir, &[&mismatch, "does not excuse"]);
}

#[test]
fn an_end_without_a_trailer_and_a_join_failure_stays_incomplete() {
    let sent = trailerless_sent();
    let (v, dir) =
        trailerless_session("end-no-trailer-join", end_fields(0, 0, 1, SHUTDOWN_NO_TRAILER, sent, sent), vec![], None);
    assert_refused(&v, &dir, &["drain thread did not join", "does not excuse"]);
}

/// Only `PS3DK_PROF_ESINKFAIL` is forgiven: 0 beside a missing trailer is inconsistent, and any other result
/// (`PS3DK_PROF_EPERM`, `PS3DK_PROF_ECLOSE`, `PS3DK_PROF_ETHREAD`) is a failure of its own.
#[test]
fn an_end_without_a_trailer_and_any_other_shutdown_result_stays_incomplete() {
    let sent = trailerless_sent();
    for result in [0, -1, -11, -12] {
        let (v, dir) = trailerless_session(
            &format!("end-no-trailer-result{result}"),
            end_fields(0, 0, 0, result, sent, sent),
            vec![],
            None,
        );
        let why = format!("shutdown result {result} is not the runtime's no-trailer result -10");
        assert_refused(&v, &dir, &[&why]);
    }
}

/// END without a trailer, then the agent keeps the link open past `close_wait`: an unclean close refuses.
#[test]
fn an_end_without_a_trailer_and_no_close_stays_incomplete() {
    let sent = trailerless_sent();
    let (v, dir) = trailerless_session(
        "end-no-trailer-no-close",
        end_fields(0, 0, 0, SHUTDOWN_NO_TRAILER, sent, sent),
        vec![Step::Hold(1500)], // close_wait is 300 ms
        None,
    );
    assert_refused(&v, &dir, &["did not close the link after END"]);
}

/// END without a trailer, then a TCP reset instead of a clean FIN.
#[cfg(target_os = "linux")]
#[test]
fn an_end_without_a_trailer_and_a_reset_stays_incomplete() {
    let sent = trailerless_sent();
    let (v, dir) = trailerless_session(
        "end-no-trailer-reset",
        end_fields(0, 0, 0, SHUTDOWN_NO_TRAILER, sent, sent),
        vec![Step::Sleep(50), Step::Reset],
        None,
    );
    assert_refused(&v, &dir, &["connection error"]);
}

/// END without a trailer, but the local copy's first append failed (the DATA may arrive in a single read, so the
/// first append is the one that always happens): the local copy does not hold every received byte, which no
/// missing trailer excuses.
#[test]
fn an_end_without_a_trailer_and_a_local_copy_mismatch_stays_incomplete() {
    let sent = trailerless_sent();
    let n = Arc::new(AtomicU64::new(0));
    let fault = EarlyFault(Arc::new(move |s| {
        (s == EarlyStep::LocalAppend && n.fetch_add(1, Ordering::SeqCst) == 0).then(|| std::io::Error::other("injected"))
    }));
    let (v, dir) = trailerless_session(
        "end-no-trailer-local",
        end_fields(0, 0, 0, SHUTDOWN_NO_TRAILER, sent, sent),
        vec![],
        Some(fault),
    );
    assert_refused(&v, &dir, &["the local copy could not be written", &format!("the local copy holds 0 of {sent} bytes")]);
    assert_eq!(v.local_error.as_deref(), Some("local write failed: injected"));
}

#[test]
fn an_end_without_a_trailer_before_any_event_chunk_stays_incomplete() {
    let (cap, _, rec) = capture_lanes(2);
    let sent = rec[0] - 3;
    let mut script = vec![Step::Send(hello_frame())];
    script.extend(data_frames(&cap[..sent], 256).into_iter().map(Step::Send));
    script.push(Step::Send(end_fields(0, 0, 0, SHUTDOWN_NO_TRAILER, sent as u64, sent as u64)));
    let (port, _) = mock(script);
    let dir = out_dir("end-no-trailer-no-events");
    let c = start(cfg(port, &dir));
    wait(&c);
    let why = reasons(c.view().outcome.as_ref().unwrap());
    assert!(why.contains("none with events"), "{why}");
    assert!(complete_files(&dir).is_empty(), "{:?}", files(&dir));
}

#[test]
fn disconnect_after_valid_data_is_published_early() {
    let (cap, _, rec) = capture_lanes(4);
    let mut script = vec![Step::Send(hello_frame())];
    script.extend(data_frames(&cap[..rec[1]], 256).into_iter().map(Step::Send));
    script.push(Step::Hold(3000)); // the user leaves while the console is quiet
    let (port, _) = mock(script);
    let dir = out_dir("disconnect-early");
    let c = start(cfg(port, &dir));
    let t0 = Instant::now();
    while c.view().received < rec[1] as u64 {
        assert!(t0.elapsed() < Duration::from_secs(5), "data never arrived");
        std::thread::sleep(Duration::from_millis(5));
    }
    c.disconnect();
    wait(&c);
    let v = c.view();
    let (p, cause, cut, torn) = early(v.outcome.as_ref().unwrap());
    assert_eq!((cause, cut as usize, torn), ("disconnected by the user", rec[1], 0));
    assert_eq!(std::fs::read(&p.path).unwrap(), &cap[..rec[1]]);
    assert_eq!(v.doc.unwrap().verdict().tone, VerdictTone::CompleteEarly);
}

#[test]
fn a_close_before_any_whole_event_chunk_stays_incomplete_and_strict() {
    let (cap, _, rec) = capture_lanes(2);
    let sent = rec[0] - 3;
    let mut script = vec![Step::Send(hello_frame())];
    script.extend(data_frames(&cap[..sent], 256).into_iter().map(Step::Send));
    let (port, _) = mock(script);
    let dir = out_dir("early-no-events");
    let c = start(cfg(port, &dir));
    wait(&c);
    let v = c.view();
    let why = reasons(v.outcome.as_ref().unwrap());
    assert!(why.contains("not a capture that ended early") && why.contains("none with events"), "{why}");
    assert!(complete_files(&dir).is_empty(), "{:?}", files(&dir));
    assert_eq!(v.doc.unwrap().verdict().tone, VerdictTone::Incomplete);
}

#[test]
fn a_protocol_failure_after_valid_data_is_never_published_early() {
    let (cap, _, rec) = capture_lanes(4);
    let mut script = vec![Step::Send(hello_frame())];
    script.extend(data_frames(&cap[..rec[1]], 256).into_iter().map(Step::Send));
    // DATA at the wrong offset: a Gap, a protocol failure
    let mut bad = ((rec[1] + 100) as u64).to_le_bytes().to_vec();
    bad.extend_from_slice(&cap[rec[1]..rec[1] + 16]);
    script.push(Step::Send(wire(T_DATA, &bad)));
    script.push(Step::Hold(500));
    let (port, _) = mock(script);
    let dir = out_dir("early-protocol");
    let c = start(cfg(port, &dir));
    wait(&c);
    let v = c.view();
    let why = reasons(v.outcome.as_ref().unwrap());
    assert!(why.contains("expected"), "{why}");
    assert!(complete_files(&dir).is_empty(), "{:?}", files(&dir));
    let doc = v.doc.unwrap();
    assert_eq!(doc.verdict().tone, VerdictTone::Incomplete, "a failed session keeps the strict analysis");
    // Its kept copy is never promoted on reopen.
    let kept = files(&dir).into_iter().find(|f| f.ends_with(".incomplete.prof")).expect("kept");
    let path = dir.join(kept);
    let re = ps3dk_prof_viewer::model::Document::from_bytes(path.clone(), &std::fs::read(&path).unwrap());
    assert_eq!(re.verdict().tone, VerdictTone::Incomplete);
}

#[test]
fn a_disconnect_racing_a_valid_end_keeps_the_end_rules() {
    let (cap, _, _) = capture_lanes(2);
    let mut script = vec![Step::Send(hello_frame())];
    script.extend(data_frames(&cap, 256).into_iter().map(Step::Send));
    script.push(Step::Send(end_frame(cap.len() as u64)));
    script.push(Step::Hold(2000)); // no close: the user disconnects inside close_wait
    let (port, _) = mock(script);
    let dir = out_dir("end-race");
    let mut k = cfg(port, &dir);
    k.close_wait = Duration::from_millis(1500);
    let c = start(k);
    let t0 = Instant::now();
    while !matches!(c.view().link, LinkState::Ended(_)) {
        assert!(t0.elapsed() < Duration::from_secs(5), "END never arrived");
        std::thread::sleep(Duration::from_millis(5));
    }
    c.disconnect();
    wait(&c);
    let v = c.view();
    assert!(matches!(v.outcome, Some(Outcome::Incomplete { .. })), "{:?}", v.outcome);
    assert!(complete_files(&dir).is_empty(), "{:?}", files(&dir));
}

#[test]
fn reopen_acceptance_follows_the_file_and_its_name() {
    let (cap, _, rec) = capture_lanes(2);
    let doc = |name: &str, b: &[u8]| ps3dk_prof_viewer::model::Document::from_bytes(PathBuf::from(name), b);
    assert_eq!(doc("a.prof", &cap).verdict().tone, VerdictTone::Complete, "ordinary COMPLETE unchanged");
    assert_eq!(doc("a.prof", &cap[..rec[1]]).verdict().tone, VerdictTone::CompleteEarly);
    assert_eq!(doc("a.incomplete.prof", &cap[..rec[1]]).verdict().tone, VerdictTone::Incomplete);
    assert_eq!(doc("a.prof", &cap[..rec[0] - 3]).verdict().tone, VerdictTone::Incomplete, "no whole event chunk");
    let early = doc("a.prof", &cap[..rec[1]]);
    let a = early.analysis().unwrap();
    assert!(a.prefix.is_some() && a.verdict == ps3dk_prof::Verdict::Complete, "a file without a trailer is COMPLETE");
    assert!(matches!(ps3dk_prof::load(&cap[..rec[1]]), ps3dk_prof::Outcome::Analysed(s) if s.verdict != ps3dk_prof::Verdict::Complete), "the strict transport verdict is unchanged");
    assert!(early.frames.is_some() && early.hot.is_some(), "every page is built from the prefix analysis");
}

#[test]
fn every_failed_step_of_an_early_publish_keeps_the_session_incomplete_and_strict() {
    let (cap, _, rec) = capture_lanes(4);
    let sent = rec[2] - 5; // a torn tail, so every step runs
    for step in [EarlyStep::Open, EarlyStep::SetLen, EarlyStep::Sync, EarlyStep::Close, EarlyStep::Publish] {
        let mut script = vec![Step::Send(hello_frame())];
        script.extend(data_frames(&cap[..sent], 256).into_iter().map(Step::Send));
        let (port, _) = mock(script);
        let dir = out_dir(&format!("early-fault-{step:?}"));
        let mut k = cfg(port, &dir);
        k.early_fault = Some(EarlyFault(Arc::new(move |s| (s == step).then(|| std::io::Error::other("injected")))));
        let c = start(k);
        wait(&c);
        let v = c.view();
        let why = reasons(v.outcome.as_ref().unwrap());
        assert!(why.contains(&format!("({step:?})")) && why.contains("injected"), "{step:?}: {why}");
        assert!(complete_files(&dir).is_empty(), "{step:?}: {:?}", files(&dir));
        assert_eq!(v.doc.unwrap().verdict().tone, VerdictTone::Incomplete, "{step:?}: strict document");
    }
}
