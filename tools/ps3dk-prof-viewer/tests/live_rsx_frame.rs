//! Protocol r2: readiness comes from DATA; ACK/STATE are only request progress.
use ps3dk_prof_viewer::live::*;

fn wire(t: u16, p: &[u8]) -> Vec<u8> {
    let mut b = t.to_le_bytes().to_vec();
    b.extend(0u16.to_le_bytes());
    b.extend((p.len() as u32).to_le_bytes());
    b.extend(p);
    b
}
fn session(version: u16, caps: u32) -> Session {
    let mut s = Session::asking(MAX_SESSION_BYTES, version);
    let mut p = version.to_le_bytes().to_vec();
    p.extend(1u16.to_le_bytes());
    p.extend(caps.to_le_bytes());
    p.extend(42u64.to_le_bytes());
    for name in ["app", "build", "file.prof"] {
        p.extend((name.len() as u16).to_le_bytes());
        p.extend(name.as_bytes());
    }
    assert!(s.feed(&wire(T_HELLO, &p)));
    s
}
fn ack(request: u32, result: i32, capture: u32) -> Vec<u8> {
    let mut p = request.to_le_bytes().to_vec();
    p.extend(result.to_le_bytes());
    p.extend(capture.to_le_bytes());
    p.extend(0u32.to_le_bytes());
    wire(T_RSX_FRAME_ACK, &p)
}
fn state(kind: u8) -> Vec<u8> {
    let mut p = 7u32.to_le_bytes().to_vec();
    p.extend([kind, 0, 0, 0]);
    p.extend(10u32.to_le_bytes());
    p.extend(1u32.to_le_bytes());
    p.extend(9u64.to_le_bytes());
    p.extend(100u64.to_le_bytes());
    wire(T_RSX_FRAME_STATE, &p)
}
fn pending() -> Session {
    let mut s = session(3, CAP_TRIGGERS | CAP_RSX_FRAME);
    s.expect_rsx_frame(1).unwrap();
    s
}
fn accepted() -> Session {
    let mut s = pending();
    assert!(s.feed(&ack(1, 0, 7)));
    s.take_rsx_events();
    s
}

#[test]
fn newest_hello_asks_for_three_and_request_layout_is_exact() {
    assert_eq!(VERSION, 3);
    let r = RsxFrameRequest {
        name: "draws".into(),
    }
    .encode(12)
    .unwrap();
    assert_eq!(&r[..8], &[12, 0, 0, 0, 17, 0, 0, 0]);
    assert_eq!(
        &r[8..],
        &[12, 0, 0, 0, 0, 0, 1, 0, 5, 0, 0, 0, b'd', b'r', b'a', b'w', b's']
    );
    assert!(RsxFrameRequest {
        name: String::new()
    }
    .encode(1)
    .is_ok());
    for name in ["x".repeat(64), "a\0b".into(), "é".repeat(32)] {
        assert!(RsxFrameRequest { name }.encode(1).is_err());
    }
    assert!(RsxFrameRequest { name: "ok".into() }.encode(0).is_err());
}
#[test]
fn windows_and_rsx_have_independent_pending_slots() {
    let mut s = pending();
    s.expect_ack(1, T_CAPTURE_BEGIN).unwrap();
    assert!(s.expect_rsx_frame(2).is_err());
    assert!(s.feed(&ack(1, 0, 7)));
    assert_eq!(s.outstanding(), Some((1, T_CAPTURE_BEGIN)));
    let e = s.take_rsx_events();
    assert_eq!(
        e,
        vec![RsxFrameEvent::Ack(RsxFrameAck {
            request: 1,
            result: 0,
            capture: 7
        })]
    );
    for k in [1, 2, 3] {
        assert!(s.feed(&state(k)));
    }
    assert_eq!(s.take_rsx_events().len(), 3);
    assert!(!s.feed(&state(3)), "no state after terminal");
}
#[test]
fn progress_may_skip_intermediate_states_but_never_go_back() {
    // The runtime samples status at a poll, so ARMED/CLOSED can be skipped.
    let mut s = accepted();
    assert!(s.feed(&state(3)));
    let mut s = accepted();
    assert!(s.feed(&state(2)));
    assert!(!s.feed(&state(1)));
    let mut s = accepted();
    assert!(s.feed(&state(4)));
    assert!(!s.feed(&state(1)));
}
#[test]
fn bad_ack_layout_identity_and_results_refuse() {
    for (offset, value) in [(8, 2), (16, 0), (20, 1)] {
        let mut s = pending();
        let mut a = ack(1, 0, 7);
        a[offset] = value;
        assert!(!s.feed(&a), "offset {offset}");
    }
    let mut s = pending();
    assert!(!s.feed(&ack(1, -6, 7)));
    let mut s = pending();
    assert!(s.feed(&ack(1, -6, 0)));
    assert!(s.expect_rsx_frame(2).is_ok());
    let mut s = pending();
    assert!(!s.feed(&ack(1, 1, 0)));
    for bytes in [15, 17] {
        let mut s = pending();
        let a = ack(1, 0, 7);
        let mut p = a[8..].to_vec();
        p.resize(bytes, 0);
        assert!(!s.feed(&wire(T_RSX_FRAME_ACK, &p)));
    }
}
#[test]
fn unsolicited_foreign_or_malformed_state_refuses() {
    let mut s = pending();
    assert!(!s.feed(&state(1)), "before ack");
    for (offset, value) in [(8, 8), (12, 0), (12, 5), (14, 1), (20, 2)] {
        let mut s = accepted();
        let mut p = state(1);
        p[offset] = value;
        assert!(!s.feed(&p), "offset {offset}");
    }
    for bytes in [31, 33] {
        let mut s = accepted();
        let a = state(1);
        let mut p = a[8..].to_vec();
        p.resize(bytes, 0);
        assert!(!s.feed(&wire(T_RSX_FRAME_STATE, &p)));
    }
    for (v, caps) in [(1, 0), (2, CAP_TRIGGERS), (3, CAP_TRIGGERS)] {
        let mut s = session(v, caps);
        assert!(s.expect_rsx_frame(1).is_err());
        assert!(!s.feed(&ack(1, 0, 7)));
    }
}
#[test]
fn progress_cannot_follow_stream_end() {
    let mut s = accepted();
    assert!(s.feed(&wire(T_END, &[0; 24])));
    assert!(!s.feed(&state(3)));
}

#[test]
fn encoded_is_not_ready_and_a_fresh_tracker_does_not_inherit_requests() {
    use ps3dk_prof_viewer::live_rsx_frame::RequestTracker;
    let mut t = RequestTracker::default();
    let s = session(3, CAP_RSX_FRAME);
    t.on_link(&s.state, s.hello.as_ref());
    let (r, _) = t.start("one".into()).unwrap();
    t.event(RsxFrameEvent::Ack(RsxFrameAck {
        request: r,
        result: 0,
        capture: 7,
    }));
    t.event(RsxFrameEvent::State(RsxFrameState {
        request: r,
        capture: 7,
        state: 3,
        reason: 0,
        draws: 1,
        frame: 9,
        bytes: 200,
    }));
    assert!(!t.resolved);
    assert!(t.blocked().is_none(), "ENCODED releases admission, not timing readiness");
    assert!(t.status_line().unwrap().contains("Waiting"));
    t.on_link(&LinkState::Failed(LinkError::Closed), s.hello.as_ref());
    assert!(t.status_line().unwrap().contains("ended"));
    assert!(RequestTracker::default().request.is_none());
}

#[test]
fn encoded_pending_timing_has_counts_and_allows_the_next_request() {
    use ps3dk_prof_viewer::live_rsx_frame::RequestTracker;
    let mut t = RequestTracker::default();
    let s = session(3, CAP_RSX_FRAME);
    t.on_link(&s.state, s.hello.as_ref());
    let (r, _) = t.start("one".into()).unwrap();
    let bytes = include_bytes!("../../ps3dk-prof/tests/fixtures/rsx_frame/sample.prof");
    let mut frames = ps3dk_prof::rsx_frame::frames(&ps3dk_prof::reader::read(bytes).unwrap());
    let f = &mut frames[0];
    f.request_id = r;
    f.status = ps3dk_prof::rsx_frame::FrameStatus::PendingTiming { missing: 1 };
    t.event(RsxFrameEvent::Ack(RsxFrameAck { request:r, capture:f.id, result:0 }));
    t.event(RsxFrameEvent::State(RsxFrameState { request:r, capture:f.id, state:3, reason:0, draws:f.totals.draws, frame:f.frame, bytes:200 }));
    t.observe(&frames);
    assert_eq!(t.status_line().unwrap(), format!("Waiting for RSX ({}/{})", frames[0].totals.timed, frames[0].totals.draws));
    assert!(!t.resolved);
    assert!(t.start("two".into()).is_ok());
    // No mutation of the reader's retained first capture.
    assert!(matches!(frames[0].status, ps3dk_prof::rsx_frame::FrameStatus::PendingTiming { .. }));
    // The first capture's timing arriving later never marks the second request Ready.
    frames[0].status = ps3dk_prof::rsx_frame::FrameStatus::Complete;
    t.observe(&frames);
    assert!(!t.resolved);
    assert_eq!(t.status_line().unwrap(), "Requested: waiting for a frame");
    assert!(t.blocked().is_some(), "the second request is outstanding");
}

#[test]
fn abandoned_without_data_is_terminal_but_never_ready() {
    use ps3dk_prof_viewer::live_rsx_frame::RequestTracker;
    let mut t = RequestTracker::default();
    let s = session(3, CAP_RSX_FRAME);
    t.on_link(&s.state, s.hello.as_ref());
    let (id, _) = t.start("no window".into()).unwrap();
    t.event(RsxFrameEvent::Ack(RsxFrameAck {
        request: id,
        result: 0,
        capture: 7,
    }));
    t.event(RsxFrameEvent::State(RsxFrameState {
        capture: 7,
        request: id,
        state: 4,
        reason: 7,
        draws: 0,
        frame: 0,
        bytes: 0,
    }));
    assert!(!t.resolved);
    assert!(t.status_line().unwrap().contains("Abandoned"));
    assert!(t.blocked().is_none());
    assert!(t.start("again".into()).is_ok());
}
#[test]
fn socket_negotiates_each_version_once_and_v3_can_send_an_rsx_request() {
    use ps3dk_prof_viewer::live_client::{ClientConfig, LiveClient};
    use std::io::{Read, Write};
    use std::time::{Duration, Instant};
    fn read(s: &mut std::net::TcpStream) -> (u16, Vec<u8>) {
        let mut h = [0; 8];
        s.read_exact(&mut h).unwrap();
        let mut p = vec![0; u32::from_le_bytes(h[4..8].try_into().unwrap()) as usize];
        s.read_exact(&mut p).unwrap();
        (u16::from_le_bytes(h[..2].try_into().unwrap()), p)
    }
    for served in [1u16, 2, 3] {
        let l = std::net::TcpListener::bind("127.0.0.1:0").unwrap();
        let port = l.local_addr().unwrap().port();
        let worker = std::thread::spawn(move || {
            for version in (served..=3).rev() {
                let (mut s, _) = l.accept().unwrap();
                s.set_read_timeout(Some(Duration::from_secs(3))).unwrap();
                let (t, p) = read(&mut s);
                assert_eq!(t, T_CLIENT_HELLO);
                assert_eq!(u16::from_le_bytes(p[4..6].try_into().unwrap()), version);
                if version != served {
                    continue;
                }
                let mut p = version.to_le_bytes().to_vec();
                p.extend(1u16.to_le_bytes());
                p.extend(
                    (if served == 3 {
                        3u32
                    } else if served == 2 {
                        1
                    } else {
                        0
                    })
                    .to_le_bytes(),
                );
                p.extend(42u64.to_le_bytes());
                for n in ["app", "build", "rsxf.prof"] {
                    p.extend((n.len() as u16).to_le_bytes());
                    p.extend(n.as_bytes());
                }
                s.write_all(&wire(T_HELLO, &p)).unwrap();
                if served == 3 {
                    let (t, p) = read(&mut s);
                    assert_eq!(t, T_RSX_FRAME_REQ);
                    let id = u32::from_le_bytes(p[..4].try_into().unwrap());
                    s.write_all(&ack(id, -6, 0)).unwrap();
                }
                // A test channel, not a sleep, holds the link until the assertion.
                let (t, _) = read(&mut s);
                assert_eq!(t, T_PING);
                s.write_all(&wire(T_END, &[0; 24])).unwrap();
            }
        });
        let dir = std::env::temp_dir().join(format!("rsxf-v{}-{}", served, std::process::id()));
        std::fs::create_dir_all(&dir).unwrap();
        let mut cfg = ClientConfig::new("127.0.0.1", port, dir.clone());
        cfg.ping_every = Duration::from_millis(500);
        cfg.liveness_timeout = Duration::from_secs(2);
        let c = LiveClient::start(cfg).unwrap();
        let now = Instant::now();
        while c.view().hello.is_none() {
            assert!(now.elapsed() < Duration::from_secs(5));
            std::thread::sleep(Duration::from_millis(5));
        }
        assert_eq!(c.view().hello.unwrap().protocol, served);
        if served == 3 {
            c.request_rsx_frame("probe".into()).unwrap();
            while c.view().rsx_frame.error.is_none() {
                assert!(now.elapsed() < Duration::from_secs(5));
                std::thread::sleep(Duration::from_millis(5));
            }
            assert!(c.view().rsx_frame.error.unwrap().contains("ENOMEM"));
        } else {
            assert!(c.request_rsx_frame("probe".into()).is_err());
        }
        c.join();
        worker.join().unwrap();
        let _ = std::fs::remove_dir_all(dir);
    }
}
