//! Live capture triggers (protocol 2), viewer side: the command and ACK wire layouts, the session's checks, the
//! button state machine, and the client against scripted agents of both versions on loopback.

#[path = "../../ps3dk-prof/tests/common/mod.rs"]
#[rustfmt::skip]
#[allow(clippy::all)]
mod common;

use common::*;
use ps3dk_prof_viewer::live::*;
use ps3dk_prof_viewer::live_client::*;
use ps3dk_prof_viewer::live_triggers::*;
use std::io::{Read, Write};
use std::net::{TcpListener, TcpStream};
use std::sync::{Arc, Mutex};
use std::time::{Duration, Instant};

#[test]
fn successful_windows_remain_savable_after_end_and_disconnect() {
    let mut t=Triggers::default();t.availability=Availability::Ready;
    let r=t.start(Command::Begin{name:"first".into()}).unwrap();
    t.on_ack(Ack{request:r,result:0,window_id:7,command:T_CAPTURE_BEGIN});
    let r=t.start(Command::End).unwrap();
    t.on_ack(Ack{request:r,result:0,window_id:7,command:T_CAPTURE_END});
    assert_eq!(t.windows.len(),1);
    assert_eq!((t.windows[0].id,t.windows[0].name.as_str()),(7,"first"));
    assert!(t.windows[0].end_acknowledged);
    let r=t.start(Command::Frames{name:"next".into(),frames:3}).unwrap();
    t.on_ack(Ack{request:r,result:0,window_id:8,command:T_CAPTURE_FRAMES});
    assert!(!t.windows[1].end_acknowledged,"ACK is not a WINDOW_END record");
    t.on_link(&LinkState::Failed(LinkError::Closed),None);
    assert_eq!(t.windows.len(),2);
    assert!(t.blocked().is_some());
    assert!(Triggers::default().windows.is_empty(),"new session never inherits ids");
}

#[test]
fn refused_commands_do_not_create_save_entries_and_history_is_bounded() {
    let mut t=Triggers::default();t.availability=Availability::Ready;
    let r=t.start(Command::Begin{name:"refused".into()}).unwrap();
    t.on_ack(Ack{request:r,result:-1,window_id:0,command:T_CAPTURE_BEGIN});
    assert!(t.windows.is_empty());
    for id in 1..=70 {
        let r=t.start(Command::Frames{name:format!("window{id}"),frames:1}).unwrap();
        t.on_ack(Ack{request:r,result:0,window_id:id,command:T_CAPTURE_FRAMES});
    }
    assert_eq!(t.windows.len(),64);
    assert_eq!(t.windows.first().unwrap().id,7);
    assert_eq!(t.windows.last().unwrap().id,70);
}

fn wire(t: u16, payload: &[u8]) -> Vec<u8> {
    let mut f = t.to_le_bytes().to_vec();
    f.extend_from_slice(&0u16.to_le_bytes());
    f.extend_from_slice(&(payload.len() as u32).to_le_bytes());
    f.extend_from_slice(payload);
    f
}

fn hello_payload(version: u16, caps: u32) -> Vec<u8> {
    let mut p = Vec::new();
    p.extend_from_slice(&version.to_le_bytes());
    p.extend_from_slice(&version.to_le_bytes());
    p.extend_from_slice(&caps.to_le_bytes());
    p.extend_from_slice(&0x42u64.to_le_bytes());
    for s in [&b"trig"[..], b"bid", b"trig-1.prof"] {
        p.extend_from_slice(&(s.len() as u16).to_le_bytes());
        p.extend_from_slice(s);
    }
    p
}

fn hello(version: u16, caps: u32) -> Vec<u8> {
    wire(T_HELLO, &hello_payload(version, caps))
}

fn ack(request: u32, result: i32, window: u32, command: u16) -> Vec<u8> {
    let mut p = request.to_le_bytes().to_vec();
    p.extend_from_slice(&result.to_le_bytes());
    p.extend_from_slice(&window.to_le_bytes());
    p.extend_from_slice(&command.to_le_bytes());
    p.extend_from_slice(&0u16.to_le_bytes());
    wire(T_CAPTURE_ACK, &p)
}

fn capture() -> Vec<u8> {
    let mut recs = vec![frame(0, 1000, 0)];
    let mut seq = 1;
    for i in 0..50u64 {
        let tb = 1100 + i * 100;
        recs.extend(zone(seq, 0, 0, tb, i + 1, 0));
        recs.push(end(seq + 2, 0, 0, tb + 50, i + 1));
        seq += 3;
    }
    scenario(vec![lane(1, recs)]).bytes()
}

fn data_and_end(cap: &[u8]) -> Vec<u8> {
    let mut v = Vec::new();
    let mut p = 0u64.to_le_bytes().to_vec();
    p.extend_from_slice(cap);
    v.extend(wire(T_DATA, &p));
    let mut e = vec![1, 1, 0, 0];
    e.extend_from_slice(&0i32.to_le_bytes());
    e.extend_from_slice(&(cap.len() as u64).to_le_bytes());
    e.extend_from_slice(&(cap.len() as u64).to_le_bytes());
    v.extend(wire(T_END, &e));
    v
}

fn streaming_session(version: u16, caps: u32) -> Session {
    let mut s = Session::asking(MAX_SESSION_BYTES, version);
    assert!(s.feed(&hello(version, caps)));
    assert!(matches!(s.state, LinkState::Streaming));
    s
}

// ---- wire layouts -----------------------------------------------------------------------------------------------

#[test]
fn command_frames_have_the_schema_layouts() {
    let b = Command::Begin { name: "pc".into() }.encode(7).unwrap();
    assert_eq!(b, wire(T_CAPTURE_BEGIN, &[7, 0, 0, 0, 2, 0, 0, 0, b'p', b'c']));
    let e = Command::End.encode(8).unwrap();
    assert_eq!(e, wire(T_CAPTURE_END, &[8, 0, 0, 0]));
    let f = Command::Frames { name: "n".into(), frames: 300 }.encode(9).unwrap();
    assert_eq!(f, wire(T_CAPTURE_FRAMES, &[9, 0, 0, 0, 44, 1, 0, 0, 1, 0, 0, 0, b'n']));
    let longest = "x".repeat(MAX_WINDOW_NAME);
    let l = Command::Frames { name: longest.clone(), frames: MAX_WINDOW_FRAMES }.encode(1).unwrap();
    assert_eq!(l.len(), HDR_BYTES + 12 + MAX_WINDOW_NAME);
    assert!(Command::Begin { name: longest }.encode(1).is_ok());
    assert_eq!(client_hello_version(1)[12..14], [1, 0]);
    assert_eq!(client_hello()[12..14], VERSION.to_le_bytes());
}

#[test]
fn commands_outside_the_bounds_are_never_encoded() {
    for bad in [
        Command::Begin { name: String::new() },
        Command::Begin { name: "x".repeat(MAX_WINDOW_NAME + 1) },
        Command::Begin { name: "a\0b".into() },
        Command::Frames { name: "x".into(), frames: 0 },
        Command::Frames { name: "x".into(), frames: MAX_WINDOW_FRAMES + 1 },
        Command::Frames { name: String::new(), frames: 1 },
    ] {
        assert!(bad.encode(1).is_err(), "{bad:?}");
    }
    assert!(Command::End.encode(0).is_err(), "request id 0");
    // a multi-byte name is bounded in bytes, not characters
    assert!(Command::Begin { name: "é".repeat(32) }.check().is_err());
    assert!(Command::Begin { name: "é".repeat(31) }.check().is_ok());
}

// ---- session ----------------------------------------------------------------------------------------------------

#[test]
fn a_v2_session_accepts_exactly_the_ack_of_its_outstanding_command() {
    let mut s = streaming_session(2, CAP_TRIGGERS);
    assert_eq!(s.protocol(), Some(2));
    assert_eq!(s.hello.as_ref().unwrap().capabilities, CAP_TRIGGERS);
    s.expect_ack(5, T_CAPTURE_BEGIN).unwrap();
    assert!(s.expect_ack(6, T_CAPTURE_END).is_err(), "one command at a time");
    assert!(s.feed(&ack(5, 0, 3, T_CAPTURE_BEGIN)));
    assert_eq!(s.take_acks(), vec![Ack { request: 5, result: 0, window_id: 3, command: T_CAPTURE_BEGIN }]);
    assert_eq!(s.outstanding(), None);
    // a refusal
    s.expect_ack(6, T_CAPTURE_END).unwrap();
    assert!(s.feed(&ack(6, -1, 0, T_CAPTURE_END)));
    assert_eq!(s.take_acks()[0].result, -1);
    assert!(s.take_acks().is_empty());
}

#[test]
fn an_ack_that_does_not_match_fails_the_link() {
    let cases: Vec<(Option<(u32, u16)>, Vec<u8>)> = vec![
        (None, ack(1, 0, 1, T_CAPTURE_BEGIN)),                     // nothing outstanding
        (Some((1, T_CAPTURE_BEGIN)), ack(2, 0, 1, T_CAPTURE_BEGIN)), // another request id
        (Some((1, T_CAPTURE_BEGIN)), ack(1, 0, 1, T_CAPTURE_END)),   // another command type
        (Some((1, T_CAPTURE_BEGIN)), ack(1, -2, 4, T_CAPTURE_BEGIN)), // a refusal naming a window
        (Some((1, T_CAPTURE_BEGIN)), wire(T_CAPTURE_ACK, &[1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 8, 0, 0])), // 15 bytes
        (Some((1, T_CAPTURE_BEGIN)), wire(T_CAPTURE_ACK, &[1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 8, 0, 1, 0])), // reserved
    ];
    for (i, (out, bytes)) in cases.into_iter().enumerate() {
        let mut s = streaming_session(2, CAP_TRIGGERS);
        if let Some((r, t)) = out {
            s.expect_ack(r, t).unwrap();
        }
        assert!(!s.feed(&bytes), "case {i}");
        assert!(matches!(s.state, LinkState::Failed(LinkError::BadFrame(_))), "case {i}: {:?}", s.state);
        assert!(s.take_acks().is_empty());
    }
}

#[test]
fn a_v1_session_has_no_commands_and_no_acks() {
    let mut s = streaming_session(1, 0);
    assert_eq!(s.protocol(), Some(1));
    let e = s.expect_ack(1, T_CAPTURE_BEGIN).unwrap_err();
    assert!(e.contains("protocol 1"), "{e}");
    assert!(!s.feed(&ack(1, 0, 1, T_CAPTURE_BEGIN)), "an ACK is an unexpected type on v1");
    // v1 keeps its reserved field: a nonzero one is still refused
    let mut s = Session::asking(MAX_SESSION_BYTES, 1);
    assert!(!s.feed(&hello(1, 1)));
}

#[test]
fn hello_must_name_a_version_the_viewer_asked_for() {
    let mut s = Session::asking(MAX_SESSION_BYTES, 1);
    assert!(!s.feed(&hello(2, CAP_TRIGGERS)), "a v2 HELLO after a v1 CLIENT_HELLO");
    let mut s = Session::asking(MAX_SESSION_BYTES, 2);
    assert!(!s.feed(&hello(3, 0)));
    let mut s = Session::asking(MAX_SESSION_BYTES, 2);
    assert!(!s.feed(&hello(0, 0)));
    // v2 without the capability: streams, but no command may be sent
    let mut s = streaming_session(2, 0);
    assert!(s.expect_ack(1, T_CAPTURE_END).is_err());
    // unknown capability bits are ignored
    let mut s = streaming_session(2, CAP_TRIGGERS | 0x8000_0000);
    assert!(s.expect_ack(1, T_CAPTURE_END).is_ok());
}

#[test]
fn no_command_before_hello_or_after_end() {
    let mut s = Session::new();
    assert!(s.expect_ack(1, T_CAPTURE_END).is_err());
    let mut s = streaming_session(2, CAP_TRIGGERS);
    let cap = capture();
    assert!(s.feed(&data_and_end(&cap)));
    assert!(matches!(s.state, LinkState::Ended(_)));
    assert!(s.expect_ack(1, T_CAPTURE_END).is_err());
}

// ---- the button state machine -------------------------------------------------------------------------------------

fn hello_struct(protocol: u16, caps: u32) -> Hello {
    Hello { protocol, capabilities: caps, agent_version: protocol, session_id: 1, app: "a".into(), build_id: vec![], file_name: "a.prof".into() }
}

#[test]
fn buttons_follow_availability_pending_and_answers() {
    let form = Form::default();
    let mut t = Triggers::default();
    let b = buttons(&t, &form);
    assert!(b.begin.is_err() && b.end.is_err() && b.frames.is_err(), "connecting");

    t.on_link(&LinkState::Streaming, Some(&hello_struct(1, 0)));
    assert_eq!(t.availability, Availability::V1Agent);
    let b = buttons(&t, &form);
    assert!(b.begin.as_ref().unwrap_err().contains("protocol 1"), "{:?}", b.begin);
    assert!(b.end.is_err() && b.frames.is_err());
    assert!(t.start(Command::End).is_err());

    t.on_link(&LinkState::Streaming, Some(&hello_struct(2, 0)));
    assert_eq!(t.availability, Availability::NoCapability);
    assert!(buttons(&t, &form).end.is_err());

    t.on_link(&LinkState::Streaming, Some(&hello_struct(2, CAP_TRIGGERS)));
    assert_eq!(t.availability, Availability::Ready);
    let b = buttons(&t, &form);
    assert_eq!(b.begin, Ok(Command::Begin { name: "viewer".into() }));
    assert_eq!(b.end, Ok(Command::End));
    assert_eq!(b.frames, Ok(Command::Frames { name: "viewer".into(), frames: 60 }));

    // pending: every button disabled until the answer
    let r = t.start(Command::Begin { name: "viewer".into() }).unwrap();
    assert!(matches!(t.status, Status::Pending { request, .. } if request == r));
    let b = buttons(&t, &form);
    assert!(b.begin.is_err() && b.end.is_err() && b.frames.is_err());
    assert!(t.start(Command::End).is_err());
    // an ACK for another request changes nothing
    t.on_ack(Ack { request: r + 1, result: 0, window_id: 9, command: T_CAPTURE_BEGIN });
    assert!(matches!(t.status, Status::Pending { .. }));
    t.on_ack(Ack { request: r, result: 0, window_id: 3, command: T_CAPTURE_BEGIN });
    assert_eq!(t.status, Status::Acked { command: Command::Begin { name: "viewer".into() }, window_id: 3 });
    assert!(t.status_line().unwrap().1.contains("window 3"));
    assert!(buttons(&t, &form).end.is_ok());

    // refused: the reason, and the buttons come back
    let r2 = t.start(Command::Begin { name: "again".into() }).unwrap();
    assert_ne!(r2, r);
    t.on_ack(Ack { request: r2, result: -2, window_id: 0, command: T_CAPTURE_BEGIN });
    match &t.status {
        Status::Refused { code, text, .. } => assert!(*code == -2 && text.contains("EBUSY"), "{text}"),
        s => panic!("{s:?}"),
    }
    let (bad, line) = t.status_line().unwrap();
    assert!(bad && line.contains("refused"));
    assert!(buttons(&t, &form).begin.is_ok());

    // the link ends with a command pending: unanswered, and everything disabled
    t.start(Command::End).unwrap();
    t.on_link(&LinkState::Failed(LinkError::Closed), Some(&hello_struct(2, CAP_TRIGGERS)));
    assert_eq!(t.availability, Availability::Ended);
    assert_eq!(t.status, Status::Unanswered { command: Command::End });
    assert!(buttons(&t, &form).end.is_err());
}

#[test]
fn a_bad_form_disables_only_the_buttons_that_use_it() {
    let mut t = Triggers::default();
    t.on_link(&LinkState::Streaming, Some(&hello_struct(2, CAP_TRIGGERS)));
    let b = buttons(&t, &Form { name: "  ".into(), frames: 60 });
    assert!(b.begin.is_err() && b.frames.is_err() && b.end.is_ok(), "an empty name");
    let b = buttons(&t, &Form { name: "x".repeat(64), frames: 60 });
    assert!(b.begin.is_err() && b.end.is_ok());
    let b = buttons(&t, &Form { name: "ok".into(), frames: 0 });
    assert!(b.begin.is_ok() && b.frames.is_err());
    // the name is trimmed
    let b = buttons(&t, &Form { name: "  pc ".into(), frames: 2 });
    assert_eq!(b.frames, Ok(Command::Frames { name: "pc".into(), frames: 2 }));
}

#[test]
fn every_refusal_code_has_a_reason() {
    for code in [-1, -2, -5, -12, -14, -15, -16, -17, -19] {
        let t = result_text(code);
        assert!(t.starts_with('E'), "{code}: {t}");
    }
    assert!(result_text(-99).contains("-99"));
}

// ---- the client against scripted agents ---------------------------------------------------------------------------

type Seen = Arc<Mutex<Vec<(u16, Vec<u8>)>>>;
type Conn = Box<dyn FnOnce(TcpStream, Seen) + Send>;

/// Accepts one connection per entry, in order; each frame the viewer sends is recorded (type, payload).
fn agent(conns: Vec<Conn>) -> (u16, Seen) {
    let l = TcpListener::bind("127.0.0.1:0").unwrap();
    let port = l.local_addr().unwrap().port();
    let seen = Arc::new(Mutex::new(Vec::new()));
    let s2 = seen.clone();
    std::thread::spawn(move || {
        for c in conns {
            let (s, _) = l.accept().unwrap();
            c(s, s2.clone());
        }
    });
    (port, seen)
}

fn read_frame(s: &mut TcpStream) -> Option<(u16, Vec<u8>)> {
    let mut h = [0u8; 8];
    s.read_exact(&mut h).ok()?;
    let len = u32::from_le_bytes(h[4..8].try_into().unwrap()) as usize;
    let mut p = vec![0u8; len];
    s.read_exact(&mut p).ok()?;
    Some((u16::from_le_bytes([h[0], h[1]]), p))
}

fn cfg(port: u16, name: &str) -> (ClientConfig, std::path::PathBuf) {
    let d = std::env::temp_dir().join(format!("ps3dk-live-trig-{}-{name}", std::process::id()));
    let _ = std::fs::remove_dir_all(&d);
    std::fs::create_dir_all(&d).unwrap();
    let mut c = ClientConfig::new("127.0.0.1", port, d.clone());
    c.parse_every = Duration::from_millis(30);
    c.close_wait = Duration::from_millis(300);
    c.ping_every = Duration::from_millis(250);
    c.hello_timeout = Duration::from_millis(800);
    c.liveness_timeout = Duration::from_millis(1500);
    (c, d)
}

fn wait_until(what: &str, f: impl Fn() -> bool) {
    let t0 = Instant::now();
    while !f() {
        assert!(t0.elapsed() < Duration::from_secs(10), "{what}");
        std::thread::sleep(Duration::from_millis(5));
    }
}

/// A v1 agent (as shipped before protocol 2): it closes a v2 CLIENT_HELLO at once without a byte, and serves a v1
/// one.  The viewer reconnects once asking for v1, streams the whole capture, keeps its triggers off with the
/// reason, and never sends the v1 agent a frame it does not know.
#[test]
fn a_v1_agent_is_detected_and_never_sent_a_command() {
    let cap = capture();
    let stream = data_and_end(&cap);
    let (port, seen) = agent(vec![
        Box::new(|mut s: TcpStream, seen: Seen| {
            let (t, p) = read_frame(&mut s).unwrap();
            assert_eq!((t, &p[4..6]), (T_CLIENT_HELLO, &2u16.to_le_bytes()[..]));
            seen.lock().unwrap().push((t, p));
            // version != 1: BAD HELLO, close
        }),
        Box::new(move |mut s: TcpStream, seen: Seen| {
            let (t, p) = read_frame(&mut s).unwrap();
            assert_eq!((t, &p[4..6]), (T_CLIENT_HELLO, &1u16.to_le_bytes()[..]));
            seen.lock().unwrap().push((t, p));
            s.write_all(&hello(1, 0)).unwrap();
            // hold the stream open while the test tries the buttons, recording whatever the viewer sends
            let mut r = s.try_clone().unwrap();
            let rec = seen.clone();
            let reader = std::thread::spawn(move || {
                while let Some(f) = read_frame(&mut r) {
                    rec.lock().unwrap().push(f);
                }
            });
            std::thread::sleep(Duration::from_millis(600));
            s.write_all(&stream).unwrap();
            std::thread::sleep(Duration::from_millis(50));
            let _ = s.shutdown(std::net::Shutdown::Both);
            let _ = reader.join();
        }),
    ]);
    let (c, dir) = cfg(port, "v1");
    let client = LiveClient::start(c).unwrap();
    wait_until("v1 HELLO", || client.view().hello.is_some());
    let v = client.view();
    assert!(v.v1_fallback);
    assert_eq!(v.hello.as_ref().unwrap().protocol, 1);
    assert_eq!(v.triggers.availability, Availability::V1Agent);
    let why = client.send(Command::Begin { name: "x".into() }).unwrap_err();
    assert!(why.contains("protocol 1"), "{why}");
    assert!(client.send(Command::End).is_err());
    wait_until("finished", || client.finished());
    let v = client.join();
    assert!(matches!(v.outcome, Some(Outcome::Published(_))), "{:?}", v.outcome);
    let types: Vec<u16> = seen.lock().unwrap().iter().map(|f| f.0).collect();
    assert_eq!(types[..2], [T_CLIENT_HELLO, T_CLIENT_HELLO]);
    assert!(types[2..].iter().all(|t| *t == T_PING), "only PINGs on the v1 session: {types:?}");
    let _ = std::fs::remove_dir_all(&dir);
}

/// A v2 agent: the viewer sends each command once, waits for its ACK, shows acks and refusals, and the stream ends
/// normally.  A v2 agent never sees a second CLIENT_HELLO.
#[test]
fn a_v2_agent_gets_one_command_at_a_time_and_its_answers_are_shown() {
    let cap = capture();
    let stream = data_and_end(&cap);
    let (port, seen) = agent(vec![Box::new(move |mut s: TcpStream, seen: Seen| {
        let (t, p) = read_frame(&mut s).unwrap();
        assert_eq!((t, &p[4..6]), (T_CLIENT_HELLO, &2u16.to_le_bytes()[..]));
        seen.lock().unwrap().push((t, p));
        s.write_all(&hello(2, CAP_TRIGGERS)).unwrap();
        let mut answered = 0;
        while answered < 3 {
            let (t, p) = read_frame(&mut s).unwrap();
            seen.lock().unwrap().push((t, p.clone()));
            if t == T_PING {
                s.write_all(&wire(T_PONG, &[])).unwrap();
                continue;
            }
            let req = u32::from_le_bytes(p[0..4].try_into().unwrap());
            std::thread::sleep(Duration::from_millis(150)); // the next frame mark
            let a = match (t, answered) {
                (T_CAPTURE_BEGIN, 0) => ack(req, 0, 1, t),
                (T_CAPTURE_BEGIN, 1) => ack(req, -2, 0, t),
                (T_CAPTURE_FRAMES, 2) => ack(req, 0, 2, t),
                other => panic!("unexpected {other:?}"),
            };
            s.write_all(&a).unwrap();
            answered += 1;
        }
        s.write_all(&stream).unwrap();
        std::thread::sleep(Duration::from_millis(50));
        let _ = s.shutdown(std::net::Shutdown::Both);
    })]);
    let (c, dir) = cfg(port, "v2");
    let client = LiveClient::start(c).unwrap();
    wait_until("ready", || client.view().triggers.availability == Availability::Ready);
    assert!(!client.view().v1_fallback);
    let answer = |what: &str| -> Status {
        wait_until(what, || !matches!(client.view().triggers.status, Status::Pending { .. }));
        client.view().triggers.status
    };
    let r1 = client.send(Command::Begin { name: "pc-window".into() }).unwrap();
    assert!(client.send(Command::End).is_err(), "pending: no second command");
    assert_eq!(answer("begin"), Status::Acked { command: Command::Begin { name: "pc-window".into() }, window_id: 1 });
    let r2 = client.send(Command::Begin { name: "again".into() }).unwrap();
    assert_ne!(r1, r2);
    assert!(matches!(answer("busy"), Status::Refused { code: -2, .. }));
    client.send(Command::Frames { name: "pc-frames".into(), frames: 5 }).unwrap();
    assert!(matches!(answer("frames"), Status::Acked { window_id: 2, .. }));
    wait_until("finished", || client.finished());
    let v = client.join();
    assert!(matches!(v.outcome, Some(Outcome::Published(_))), "{:?}", v.outcome);
    assert_eq!(v.triggers.availability, Availability::Ended);
    let cmds: Vec<(u16, Vec<u8>)> = seen.lock().unwrap().iter().filter(|f| f.0 != T_PING).cloned().collect();
    assert_eq!(cmds.len(), 4);
    assert_eq!(cmds[1], (T_CAPTURE_BEGIN, Command::Begin { name: "pc-window".into() }.encode(r1).unwrap()[8..].to_vec()));
    assert_eq!(cmds[3].0, T_CAPTURE_FRAMES);
    let _ = std::fs::remove_dir_all(&dir);
}

/// The link ends while a command waits for its answer: the viewer says so instead of claiming either outcome.
#[test]
fn a_command_left_unanswered_by_the_link_end_is_reported() {
    let cap = capture();
    let stream = data_and_end(&cap);
    let (port, _) = agent(vec![Box::new(move |mut s: TcpStream, _: Seen| {
        let _ = read_frame(&mut s).unwrap();
        s.write_all(&hello(2, CAP_TRIGGERS)).unwrap();
        loop {
            let (t, _) = read_frame(&mut s).unwrap();
            if t == T_CAPTURE_END {
                break;
            }
        }
        s.write_all(&stream).unwrap(); // END, no ACK (the agent never follows END with an ACK)
        std::thread::sleep(Duration::from_millis(50));
        let _ = s.shutdown(std::net::Shutdown::Both);
    })]);
    let (c, dir) = cfg(port, "unanswered");
    let client = LiveClient::start(c).unwrap();
    wait_until("ready", || client.view().triggers.availability == Availability::Ready);
    client.send(Command::End).unwrap();
    wait_until("finished", || client.finished());
    let v = client.join();
    assert_eq!(v.triggers.status, Status::Unanswered { command: Command::End });
    assert!(v.triggers.status_line().unwrap().0);
    assert!(matches!(v.outcome, Some(Outcome::Published(_))));
    let _ = std::fs::remove_dir_all(&dir);
}

/// An agent that answers with an ACK nobody asked for fails the link (as any malformed frame does).
#[test]
fn an_unsolicited_ack_fails_the_link() {
    let (port, _) = agent(vec![Box::new(move |mut s: TcpStream, _: Seen| {
        let _ = read_frame(&mut s).unwrap();
        s.write_all(&hello(2, CAP_TRIGGERS)).unwrap();
        s.write_all(&ack(1, 0, 1, T_CAPTURE_BEGIN)).unwrap();
        std::thread::sleep(Duration::from_millis(300));
    })]);
    let (c, dir) = cfg(port, "unsolicited");
    let client = LiveClient::start(c).unwrap();
    wait_until("finished", || client.finished());
    let v = client.join();
    assert!(matches!(&v.link, LinkState::Failed(LinkError::BadFrame(m)) if m.contains("not outstanding")), "{:?}", v.link);
    assert!(matches!(v.outcome, Some(Outcome::Incomplete { .. })));
    let _ = std::fs::remove_dir_all(&dir);
}

/// A connection that closes during the handshake after sending bytes is not a v1 agent: no reconnect.
#[test]
fn only_a_silent_close_after_the_v2_hello_falls_back() {
    let (port, seen) = agent(vec![Box::new(move |mut s: TcpStream, seen: Seen| {
        let f = read_frame(&mut s).unwrap();
        seen.lock().unwrap().push(f);
        let mut busy = 1u32.to_le_bytes().to_vec();
        busy.truncate(4);
        s.write_all(&wire(T_BUSY, &busy)).unwrap();
    })]);
    let (c, dir) = cfg(port, "busy");
    let client = LiveClient::start(c).unwrap();
    wait_until("finished", || client.finished());
    let v = client.join();
    assert!(matches!(v.link, LinkState::Failed(LinkError::Busy(1))), "{:?}", v.link);
    assert!(!v.v1_fallback);
    assert_eq!(seen.lock().unwrap().len(), 1);
    let _ = std::fs::remove_dir_all(&dir);
}
