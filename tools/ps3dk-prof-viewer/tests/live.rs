//! Live link, viewer side: recorded agent streams through the decoder and
//! session, publication rules, and the reader on every prefix of a capture.

#[path = "../../ps3dk-prof/tests/common/mod.rs"]
#[rustfmt::skip]
#[allow(clippy::all)]
mod common;

use common::*;
use ps3dk_prof::{load, Outcome};
use ps3dk_prof_viewer::live::*;

// ---- an agent-side encoder for recorded streams ---------------------------

fn frame(t: u16, flags: u16, payload: &[u8]) -> Vec<u8> {
    let mut f = Vec::new();
    f.extend_from_slice(&t.to_le_bytes());
    f.extend_from_slice(&flags.to_le_bytes());
    f.extend_from_slice(&(payload.len() as u32).to_le_bytes());
    f.extend_from_slice(payload);
    f
}

fn str16(v: &mut Vec<u8>, s: &[u8]) {
    v.extend_from_slice(&(s.len() as u16).to_le_bytes());
    v.extend_from_slice(s);
}

fn hello_payload(app: &[u8], build: &[u8], name: &[u8]) -> Vec<u8> {
    let mut p = Vec::new();
    p.extend_from_slice(&VERSION.to_le_bytes());
    p.extend_from_slice(&1u16.to_le_bytes());
    p.extend_from_slice(&0u32.to_le_bytes());
    p.extend_from_slice(&0x0123_4567_89ab_cdefu64.to_le_bytes());
    str16(&mut p, app);
    str16(&mut p, build);
    str16(&mut p, name);
    p
}

fn hello() -> Vec<u8> {
    frame(
        T_HELLO,
        0,
        &hello_payload(b"heterogeneous", b"bid", b"heterogeneous-1.prof"),
    )
}

fn data(off: u64, bytes: &[u8]) -> Vec<u8> {
    let mut p = off.to_le_bytes().to_vec();
    p.extend_from_slice(bytes);
    frame(T_DATA, 0, &p)
}

fn end(io: u8, result: i32, accepted: u64, sent: u64) -> Vec<u8> {
    let mut p = vec![io, io, 0, 0];
    p.extend_from_slice(&result.to_le_bytes());
    p.extend_from_slice(&accepted.to_le_bytes());
    p.extend_from_slice(&sent.to_le_bytes());
    frame(T_END, 0, &p)
}

/// HELLO, the capture in DATA frames of at most `chunk` bytes, END.
fn stream(capture: &[u8], chunk: usize) -> Vec<u8> {
    let mut s = hello();
    let mut off = 0usize;
    while off < capture.len() {
        let n = chunk.min(capture.len() - off);
        s.extend(data(off as u64, &capture[off..off + n]));
        off += n;
    }
    s.extend(end(1, 0, capture.len() as u64, capture.len() as u64));
    s
}

fn capture() -> Vec<u8> {
    let mut recs = vec![frame_rec(0, 1000, 0)];
    recs.extend(zone(1, 0, 0, 1100, 1, 0));
    recs.push(end_rec(3, 0, 0, 1200, 1));
    scenario(vec![lane(1, recs)]).bytes()
}

// The writer's FRAME/END record builders share names with the frame
// helpers above; alias them.
fn frame_rec(seq: u64, tb: u64, id: u64) -> Rec {
    common::frame(seq, tb, id)
}
fn end_rec(seq: u64, desc: u32, depth: u8, tb: u64, span: u64) -> Rec {
    common::end(seq, desc, depth, tb, span)
}

fn run(bytes: &[u8]) -> Session {
    let mut s = Session::new();
    s.feed(bytes);
    s
}

fn failed_with(s: &Session) -> &LinkError {
    match &s.state {
        LinkState::Failed(e) => e,
        other => panic!("expected a failure, got {other:?}"),
    }
}

// ---- schema ---------------------------------------------------------------

#[test]
fn constants_match_the_c_schema() {
    let h = std::fs::read_to_string(concat!(
        env!("CARGO_MANIFEST_DIR"),
        "/../../sdk/libps3dkprof/src/live/live_proto.h"
    ))
    .unwrap();
    let def = |name: &str| -> u64 {
        let line = h
            .lines()
            .find(|l| l.starts_with(&format!("#define {name} ")))
            .unwrap_or_else(|| panic!("{name} not in live_proto.h"));
        let v = line.split_whitespace().nth(2).unwrap();
        let v = v.trim_end_matches('u');
        if let Some(x) = v.strip_prefix("0x") {
            u64::from_str_radix(x, 16).unwrap()
        } else {
            v.parse().unwrap_or_else(|_| panic!("{name}: {v}"))
        }
    };
    assert_eq!(def("LIVE_HDR_BYTES"), HDR_BYTES as u64);
    assert_eq!(def("LIVE_MAX_PAYLOAD"), MAX_PAYLOAD as u64);
    assert_eq!(def("LIVE_T_CLIENT_HELLO"), T_CLIENT_HELLO as u64);
    assert_eq!(def("LIVE_MAGIC"), MAGIC as u64);
    assert_eq!(def("LIVE_VERSION"), VERSION as u64);
    assert_eq!(def("LIVE_T_HELLO"), T_HELLO as u64);
    assert_eq!(def("LIVE_MAX_APP"), MAX_APP as u64);
    assert_eq!(def("LIVE_MAX_BUILD_ID"), MAX_BUILD_ID as u64);
    assert_eq!(def("LIVE_MAX_NAME"), MAX_NAME as u64);
    assert_eq!(def("LIVE_T_DATA"), T_DATA as u64);
    assert_eq!(def("LIVE_T_END"), T_END as u64);
    assert_eq!(def("LIVE_END_BYTES"), END_BYTES as u64);
    assert_eq!(def("LIVE_T_PING"), T_PING as u64);
    assert_eq!(def("LIVE_T_PONG"), T_PONG as u64);
    assert_eq!(def("LIVE_T_BUSY"), T_BUSY as u64);
    assert!(h.contains("#define LIVE_MAX_DATA (LIVE_MAX_PAYLOAD - 8u)"));
    assert_eq!(MAX_DATA, MAX_PAYLOAD - 8);
    let ch = client_hello();
    assert_eq!(ch.len(), HDR_BYTES + 8);
    assert_eq!(&ch[8..12], &MAGIC.to_le_bytes());
}

// ---- valid streams ---------------------------------------------------------

#[test]
fn a_valid_stream_arrives_whole_however_it_is_split() {
    let cap = capture();
    let s = stream(&cap, 100);
    // Fed whole, a byte at a time, and in uneven pieces.
    let mut splits: Vec<Vec<usize>> = vec![vec![s.len()], vec![1; s.len()]];
    let mut x: u64 = 12345;
    let mut uneven = Vec::new();
    let mut left = s.len();
    while left > 0 {
        x = x
            .wrapping_mul(6364136223846793005)
            .wrapping_add(1442695040888963407);
        let n = ((x >> 33) as usize % 37 + 1).min(left);
        uneven.push(n);
        left -= n;
    }
    splits.push(uneven);
    for sizes in splits {
        let mut sess = Session::new();
        let mut at = 0;
        for n in sizes {
            assert!(sess.feed(&s[at..at + n]));
            at += n;
        }
        match sess.state {
            LinkState::Ended(e) => {
                assert!(e.io_finalized && e.sent_bytes == cap.len() as u64);
            }
            ref other => panic!("{other:?}"),
        }
        assert_eq!(sess.received.to_vec(), cap);
        let h = sess.hello.as_ref().unwrap();
        assert_eq!(
            (h.app.as_str(), h.session_id),
            ("heterogeneous", 0x0123_4567_89ab_cdef)
        );
        sess.closed();
        assert!(
            matches!(sess.state, LinkState::Ended(_)),
            "close after END is fine"
        );
    }
}

#[test]
fn snapshots_share_segments() {
    let cap = capture();
    let mut s = Session::new();
    s.feed(&stream(&cap, 64));
    let snap = s.received.snapshot();
    assert_eq!(snap.len(), cap.len() as u64);
    assert_eq!(snap.to_vec(), cap);
}

#[test]
fn pong_is_counted() {
    let mut s = hello();
    s.extend(frame(T_PONG, 0, &[]));
    assert_eq!(run(&s).pongs, 1);
}

// ---- failures ---------------------------------------------------------------

#[test]
fn data_before_hello_fails() {
    let s = run(&data(0, b"abc"));
    assert!(matches!(failed_with(&s), LinkError::Order(_)));
}

#[test]
fn gap_and_overlap_fail() {
    let mut g = hello();
    g.extend(data(0, b"abcd"));
    g.extend(data(5, b"e"));
    assert_eq!(
        *failed_with(&run(&g)),
        LinkError::Gap {
            expected: 4,
            got: 5
        }
    );
    let mut o = hello();
    o.extend(data(0, b"abcd"));
    o.extend(data(2, b"cd"));
    assert_eq!(
        *failed_with(&run(&o)),
        LinkError::Gap {
            expected: 4,
            got: 2
        }
    );
}

#[test]
fn bytes_past_the_cap_fail() {
    let mut s = hello();
    s.extend(data(0, &[7u8; 100]));
    let mut sess = Session::with_cap(99);
    sess.feed(&s);
    assert_eq!(*failed_with(&sess), LinkError::TooLarge);
    let mut sess = Session::with_cap(100);
    sess.feed(&s);
    assert!(!sess.failed());
}

#[test]
fn oversize_length_and_flags_fail_at_the_header() {
    let mut h = hello();
    h.extend_from_slice(&T_DATA.to_le_bytes());
    h.extend_from_slice(&0u16.to_le_bytes());
    h.extend_from_slice(&(MAX_PAYLOAD + 1).to_le_bytes());
    // No payload follows: the header alone must fail.
    assert!(matches!(failed_with(&run(&h)), LinkError::BadFrame(_)));
    let mut f = hello();
    f.extend(frame(T_PONG, 1, &[]));
    assert!(matches!(failed_with(&run(&f)), LinkError::BadFrame(_)));
}

#[test]
fn end_size_mismatch_and_bad_end_fail() {
    let mut s = hello();
    s.extend(data(0, b"abcd"));
    s.extend(end(1, 0, 4, 5));
    assert_eq!(
        *failed_with(&run(&s)),
        LinkError::EndMismatch {
            received: 4,
            sent: 5
        }
    );
    let mut b = hello();
    b.extend(data(0, b"abcd"));
    let mut e = end(1, 0, 4, 4);
    e[HDR_BYTES] = 2; // io_finalized must be 0 or 1
    b.extend(e);
    assert!(matches!(failed_with(&run(&b)), LinkError::BadFrame(_)));
}

#[test]
fn any_byte_after_end_fails_and_refuses_publication() {
    let base = stream(&capture(), 1000);
    let header_only = {
        let mut h = T_PONG.to_le_bytes().to_vec();
        h.extend_from_slice(&[0, 0, 0, 0, 0, 0]);
        h
    };
    let partial_payload = {
        let mut f = frame(T_DATA, 0, &[0u8; 20]);
        f.truncate(HDR_BYTES + 5);
        f
    };
    let suffixes: [&[u8]; 4] = [&[0], &[1, 2, 3, 4, 5, 6, 7], &header_only, &partial_payload];
    for suffix in suffixes {
        // In the same feed as END, and in a later feed.
        for same_feed in [true, false] {
            let mut sess = Session::new();
            if same_feed {
                let mut all = base.clone();
                all.extend_from_slice(suffix);
                sess.feed(&all);
            } else {
                assert!(sess.feed(&base));
                assert!(matches!(sess.state, LinkState::Ended(_)));
                sess.feed(suffix);
            }
            sess.closed();
            assert_eq!(
                *failed_with(&sess),
                LinkError::AfterEnd,
                "{suffix:?} {same_feed}"
            );
            let n = sess.received.len();
            assert!(matches!(
                publish_decision(&sess.state, n, true, true),
                Publish::Incomplete(_)
            ));
        }
    }
    // An empty feed and a clean close after END stay valid.
    let mut sess = Session::new();
    sess.feed(&base);
    assert!(sess.feed(&[]));
    sess.closed();
    assert!(matches!(sess.state, LinkState::Ended(_)));
}

#[test]
fn hello_reserved_bytes_must_be_zero() {
    for i in 4..8 {
        let mut p = hello_payload(b"a", b"", b"x.prof");
        p[i] = 1;
        let s = run(&frame(T_HELLO, 0, &p));
        assert!(
            matches!(failed_with(&s), LinkError::BadFrame(_)),
            "reserved byte {i}"
        );
    }
}

#[test]
fn tiny_data_frames_keep_storage_bounded_and_snapshots_immutable() {
    // 300,000 one-byte DATA frames: storage stays a few blocks.
    let mut s = hello();
    let n = 300_000usize;
    let byte = |i: usize| (i * 7 + 3) as u8;
    for i in 0..n {
        s.extend(data(i as u64, &[byte(i)]));
    }
    let mut sess = Session::new();
    sess.feed(&s);
    assert!(!sess.failed());
    assert_eq!(sess.received.len(), n as u64);
    assert_eq!(sess.received.block_count(), n.div_ceil(BLOCK_BYTES));
    let snap = sess.received.snapshot();
    let before = snap.to_vec();
    assert!(before.iter().enumerate().all(|(i, b)| *b == byte(i)));
    // More data after the snapshot does not change it.
    let mut more = Vec::new();
    for i in n..n + 2 * BLOCK_BYTES {
        more.extend(data(i as u64, &[byte(i)]));
    }
    sess.feed(&more);
    assert_eq!(snap.to_vec(), before);
    assert_eq!(sess.received.len(), (n + 2 * BLOCK_BYTES) as u64);
    assert_eq!(
        sess.received.block_count(),
        (n + 2 * BLOCK_BYTES).div_ceil(BLOCK_BYTES)
    );
}

#[test]
fn a_frame_after_end_fails() {
    let mut s = stream(&capture(), 1000);
    s.extend(frame(T_PONG, 0, &[]));
    assert_eq!(*failed_with(&run(&s)), LinkError::AfterEnd);
}

#[test]
fn busy_is_a_failure_with_its_reason() {
    let s = run(&frame(T_BUSY, 0, &1u32.to_le_bytes()));
    assert_eq!(*failed_with(&s), LinkError::Busy(1));
}

#[test]
fn a_close_mid_frame_or_before_end_fails() {
    let s = stream(&capture(), 64);
    let mut sess = Session::new();
    sess.feed(&s[..s.len() - 10]); // cut inside END
    assert!(!sess.failed());
    sess.closed();
    assert_eq!(*failed_with(&sess), LinkError::Closed);
}

#[test]
fn bad_hellos_fail() {
    let bad = [
        {
            let mut p = hello_payload(b"a", b"", b"x.prof");
            p[0] = 2; // protocol version 2
            p
        },
        hello_payload(&[b'a'; MAX_APP + 1], b"", b"x.prof"),
        hello_payload(b"", b"", b"x.prof"),
        hello_payload(b"a", b"", b""),
        hello_payload(b"a", b"", b"../x.prof"),
        {
            let mut p = hello_payload(b"a", b"", b"x.prof");
            p.push(0); // trailing byte
            p
        },
        hello_payload(b"a", b"", b"x.prof")[..20].to_vec(), // truncated
    ];
    for p in bad {
        let s = run(&frame(T_HELLO, 0, &p));
        assert!(matches!(failed_with(&s), LinkError::BadFrame(_)), "{p:?}");
    }
}

// ---- publication -----------------------------------------------------------

fn ended(io: bool, result: i32, accepted: u64, sent: u64) -> LinkState {
    LinkState::Ended(End {
        io_finalized: io,
        trailer_written: io,
        join_failed: false,
        shutdown_result: result,
        accepted_bytes: accepted,
        sent_bytes: sent,
    })
}

#[test]
fn publish_only_when_every_check_passes() {
    assert_eq!(
        publish_decision(&ended(true, 0, 100, 100), 100, true, true),
        Publish::Complete
    );
    let cases = [
        publish_decision(&ended(false, -10, 90, 100), 100, true, true),
        publish_decision(&ended(true, 0, 120, 100), 100, true, true),
        publish_decision(&ended(true, -9, 100, 100), 100, true, true),
        publish_decision(&ended(true, 0, 100, 100), 100, false, true),
        publish_decision(&ended(true, 0, 100, 100), 100, true, false),
        publish_decision(&LinkState::Failed(LinkError::Closed), 100, true, true),
        publish_decision(&LinkState::Streaming, 100, true, true),
    ];
    for c in cases {
        match c {
            Publish::Incomplete(why) => assert!(!why.is_empty()),
            Publish::Complete => panic!("published something that failed a check"),
        }
    }
    // A failed shutdown is reported as such, not as a final size.
    match publish_decision(&ended(false, -10, 90, 100), 100, true, true) {
        Publish::Incomplete(why) => {
            assert!(why.iter().any(|w| w.contains("did not finalize")));
            assert!(!why.iter().any(|w| w.contains("wrote 90")));
        }
        Publish::Complete => unreachable!(),
    }
}

#[test]
fn local_names_are_generated_and_sanitized() {
    let h = |name: &str| Hello {
        agent_version: 1,
        session_id: 0xab,
        app: "a".into(),
        build_id: vec![],
        file_name: name.into(),
    };
    assert_eq!(
        local_stem(&h("heterogeneous-1.prof")),
        "00000000000000ab-heterogeneous-1"
    );
    for bad in [
        "..",
        "../../etc/passwd",
        "a\\b:c*?.prof",
        ".hidden",
        "\u{e9}t\u{e9}",
    ] {
        let s = local_stem(&h(bad));
        assert!(s.starts_with("00000000000000ab-"), "{s}");
        assert!(
            !s.contains('/') && !s.contains('\\') && !s.contains(':') && !s.contains(".."),
            "{s}"
        );
        assert!(
            s.chars()
                .all(|c| c.is_ascii_alphanumeric() || "_-".contains(c)),
            "{s}"
        );
    }
    assert_eq!(local_stem(&h(&"x".repeat(300))).len(), 17 + 100);
}

fn scratch(name: &str) -> std::path::PathBuf {
    let d = std::env::temp_dir().join(format!("ps3dk-live-{}-{name}", std::process::id()));
    let _ = std::fs::remove_dir_all(&d);
    std::fs::create_dir_all(&d).unwrap();
    d
}

#[test]
fn publication_never_overwrites() {
    let d = scratch("publish");
    std::fs::write(d.join("s.prof"), b"older").unwrap();
    let partial = d.join("s.partial");
    let mut copy = LocalCopy::new(Box::new(FileSink::create(&partial).unwrap()));
    copy.append(b"new capture");
    assert!(copy.finish());
    let got = publish_file(&partial, &d, "s", SUFFIX_COMPLETE).unwrap();
    assert_eq!(got.cleanup_warning, None);
    let got = got.path;
    assert_eq!(got, d.join("s-1.prof"));
    assert_eq!(std::fs::read(d.join("s.prof")).unwrap(), b"older");
    assert_eq!(std::fs::read(&got).unwrap(), b"new capture");
    assert!(!partial.exists());
    // The partial file itself is created exclusively.
    std::fs::write(&partial, b"x").unwrap();
    assert!(FileSink::create(&partial).is_err());
    let _ = std::fs::remove_dir_all(&d);
}

/// A sink that fails chosen steps and records every call.
#[derive(Default)]
struct Spy {
    room: Option<usize>,
    fail_flush: bool,
    fail_sync: bool,
    fail_close: bool,
    calls: std::sync::Arc<std::sync::Mutex<Vec<&'static str>>>,
}

impl LocalSink for Spy {
    fn write_all(&mut self, bytes: &[u8]) -> std::io::Result<()> {
        self.calls.lock().unwrap().push("write");
        if let Some(room) = self.room.as_mut() {
            if bytes.len() > *room {
                return Err(std::io::Error::other("disk full"));
            }
            *room -= bytes.len();
        }
        Ok(())
    }
    fn flush(&mut self) -> std::io::Result<()> {
        self.calls.lock().unwrap().push("flush");
        if self.fail_flush {
            return Err(std::io::Error::other("flush failed"));
        }
        Ok(())
    }
    fn sync(&mut self) -> std::io::Result<()> {
        self.calls.lock().unwrap().push("sync");
        if self.fail_sync {
            return Err(std::io::Error::other("sync failed"));
        }
        Ok(())
    }
    fn close(&mut self) -> std::io::Result<()> {
        self.calls.lock().unwrap().push("close");
        if self.fail_close {
            return Err(std::io::Error::other("close failed"));
        }
        Ok(())
    }
}

fn spy_run(spy: Spy, writes: &[&[u8]]) -> (LocalCopy, Vec<&'static str>) {
    let calls = spy.calls.clone();
    let mut copy = LocalCopy::new(Box::new(spy));
    for w in writes {
        copy.append(w);
    }
    let first = copy.finish();
    let again = copy.finish(); // never erases a failure, never closes twice
    assert_eq!(first, again);
    let c = calls.lock().unwrap().clone();
    (copy, c)
}

#[test]
fn a_local_write_error_stops_writing_still_closes_once_and_is_kept() {
    let spy = Spy {
        room: Some(10),
        ..Default::default()
    };
    let (copy, calls) = spy_run(spy, &[b"0123456789", b"x", b"y"]);
    assert_eq!(copy.written, 10);
    assert!(copy.error.as_deref().unwrap().contains("disk full"));
    // The failed write stops further writes, skips flush and sync, and
    // still closes exactly once.
    assert_eq!(calls, vec!["write", "write", "close"]);
}

#[test]
fn every_finish_step_failure_is_reported_and_close_runs_once_last() {
    let ok = spy_run(Spy::default(), &[b"abc"]);
    assert!(ok.0.error.is_none());
    assert_eq!(ok.1, vec!["write", "flush", "sync", "close"]);
    let cases = [
        (
            Spy {
                fail_flush: true,
                ..Default::default()
            },
            "flush failed",
            vec!["write", "flush", "close"],
        ),
        (
            Spy {
                fail_sync: true,
                ..Default::default()
            },
            "sync failed",
            vec!["write", "flush", "sync", "close"],
        ),
        (
            Spy {
                fail_close: true,
                ..Default::default()
            },
            "close failed",
            vec!["write", "flush", "sync", "close"],
        ),
    ];
    for (spy, want, steps) in cases {
        let (copy, got) = spy_run(spy, &[b"abc"]);
        assert!(
            copy.error.as_deref().unwrap().contains(want),
            "{:?}",
            copy.error
        );
        assert_eq!(got, steps);
    }
}

#[test]
fn a_real_file_closes_with_a_checked_close() {
    let d = scratch("close");
    let partial = d.join("c.partial");
    let mut copy = LocalCopy::new(Box::new(FileSink::create(&partial).unwrap()));
    copy.append(b"data");
    assert!(copy.finish());
    assert!(copy.finish());
    assert_eq!(std::fs::read(&partial).unwrap(), b"data");
    let _ = std::fs::remove_dir_all(&d);
}

#[cfg(unix)]
#[test]
fn a_removal_failure_after_publication_is_a_warning_not_a_failure() {
    use std::os::unix::fs::PermissionsExt;
    let d = scratch("cleanup");
    let src = d.join("src");
    let out = d.join("out");
    std::fs::create_dir_all(&src).unwrap();
    std::fs::create_dir_all(&out).unwrap();
    let partial = src.join("p.partial");
    std::fs::write(&partial, b"capture").unwrap();
    // The partial's directory becomes read-only: the link into `out` works,
    // removing the partial does not.
    std::fs::set_permissions(&src, std::fs::Permissions::from_mode(0o555)).unwrap();
    // A privileged runner (root, CAP_DAC_OVERRIDE) can still write there,
    // so the fault would not happen: say so instead of passing silently.
    if std::fs::write(src.join("probe"), b"x").is_ok() {
        std::fs::set_permissions(&src, std::fs::Permissions::from_mode(0o755)).unwrap();
        let _ = std::fs::remove_dir_all(&d);
        eprintln!("SKIP removal-failure case: a read-only directory is writable for this user");
        return;
    }
    let r = publish_file(&partial, &out, "p", SUFFIX_COMPLETE);
    std::fs::set_permissions(&src, std::fs::Permissions::from_mode(0o755)).unwrap();
    let p = r.expect("published");
    assert_eq!(p.path, out.join("p.prof"));
    assert_eq!(std::fs::read(&p.path).unwrap(), b"capture");
    assert!(p
        .cleanup_warning
        .as_deref()
        .unwrap()
        .contains("was not removed"));
    assert!(partial.exists());
    let _ = std::fs::remove_dir_all(&d);
}

#[test]
fn publication_refuses_names_it_did_not_make() {
    let d = scratch("names");
    let partial = d.join("x.partial");
    std::fs::write(&partial, b"x").unwrap();
    for (stem, suffix) in [
        ("../x", SUFFIX_COMPLETE),
        ("a/b", SUFFIX_COMPLETE),
        ("", SUFFIX_COMPLETE),
        ("ok", ".exe"),
        ("ok", ""),
    ] {
        let e = publish_file(&partial, &d, stem, suffix).unwrap_err();
        assert_eq!(e.kind(), std::io::ErrorKind::InvalidInput, "{stem}{suffix}");
    }
    assert!(partial.exists());
    let _ = std::fs::remove_dir_all(&d);
}

// ---- the reader on partial data --------------------------------------------

fn verdict_is_malformed(bytes: &[u8]) -> bool {
    matches!(load(bytes), Outcome::Malformed(_))
}

#[test]
fn every_strict_prefix_of_a_valid_capture_is_incomplete_not_malformed() {
    let cap = capture();
    for n in 0..cap.len() {
        match load(&cap[..n]) {
            Outcome::Analysed(a) => assert!(
                !matches!(a.verdict, ps3dk_prof::Verdict::Complete),
                "prefix {n} claims COMPLETE"
            ),
            Outcome::Malformed(m) => panic!("prefix {n} of a valid capture is MALFORMED: {m}"),
        }
    }
    assert!(matches!(
        load(&cap),
        Outcome::Analysed(a) if matches!(a.verdict, ps3dk_prof::Verdict::Complete)
    ));
}

#[test]
fn corrupt_prefixes_stay_malformed() {
    // Wrong magic, major version or file flags: MALFORMED as soon as the
    // bad byte is present, never relabelled as waiting for data.
    let cap = capture();
    for (at, label) in [(0usize, "magic"), (8, "major"), (12, "file flags")] {
        let mut bad = cap.clone();
        bad[at] ^= 0x5a;
        for n in at + 1..=bad.len().min(64) {
            assert!(verdict_is_malformed(&bad[..n]), "{label}: prefix {n}");
        }
        for n in 0..=at {
            assert!(
                !verdict_is_malformed(&bad[..n]),
                "{label}: prefix {n} before the bad byte"
            );
        }
    }
}

#[test]
fn cut_file_checks_every_step_and_releases_the_handle() {
    use std::sync::Arc;
    let dir = std::env::temp_dir().join(format!("ps3dk-cut-file-{}", std::process::id()));
    let _ = std::fs::remove_dir_all(&dir);
    std::fs::create_dir_all(&dir).unwrap();
    let p = dir.join("c.partial");
    std::fs::write(&p, [7u8; 100]).unwrap();
    assert_eq!(cut_file(&p, 60, &None), Ok(()));
    assert_eq!(std::fs::metadata(&p).unwrap().len(), 60);
    for step in [EarlyStep::Open, EarlyStep::SetLen, EarlyStep::Sync, EarlyStep::Close] {
        std::fs::write(&p, [7u8; 100]).unwrap();
        let f = Some(EarlyFault(Arc::new(move |s| (s == step).then(|| std::io::Error::other("injected")))));
        let e = cut_file(&p, 60, &f).unwrap_err();
        assert!(e.contains(&format!("({step:?})")) && e.contains("injected"), "{e}");
        let len = std::fs::metadata(&p).unwrap().len();
        match step {
            EarlyStep::Open | EarlyStep::SetLen => assert_eq!(len, 100, "{step:?}: nothing cut"),
            _ => assert_eq!(len, 60, "{step:?}"),
        }
        // The handle was released: the file can be removed (Windows refuses an open file).
        std::fs::remove_file(&p).unwrap();
    }
    let missing = cut_file(&dir.join("missing.partial"), 1, &None).unwrap_err();
    assert!(missing.contains("(Open)"), "{missing}");
}

/// The missing-trailer exception forgives exactly `trailer_written` 0, `io_finalized` 0 and the shutdown result
/// `PS3DK_PROF_ESINKFAIL`; everything else an END reports still refuses, and an END with a trailer forgives nothing.
#[test]
fn the_missing_trailer_exception_forgives_only_the_enumerated_end_facts() {
    assert_eq!(NO_TRAILER_FORGIVEN_RESULTS, [-10]);
    let base = End {
        io_finalized: false,
        trailer_written: false,
        join_failed: false,
        shutdown_result: SHUTDOWN_NO_TRAILER,
        accepted_bytes: 100,
        sent_bytes: 100,
    };
    assert!(trailerless_end_refusals(&base, 100).is_empty());
    // io_finalized 0: the accepted count is not checked (the runtime acknowledges positively written bytes only).
    assert!(trailerless_end_refusals(&End { accepted_bytes: 7, ..base }, 100).is_empty());
    assert!(trailerless_end_refusals(&End { io_finalized: true, ..base }, 100).is_empty());
    let r = trailerless_end_refusals(&End { io_finalized: true, accepted_bytes: 99, ..base }, 100);
    assert_eq!(r, vec!["the console wrote 99 bytes, 100 arrived".to_string()]);
    let r = trailerless_end_refusals(&End { join_failed: true, ..base }, 100);
    assert_eq!(r, vec!["the profiler's drain thread did not join".to_string()]);
    for code in [0, -1, -2, -9, -11, -12, -13, i32::MIN, i32::MAX] {
        let r = trailerless_end_refusals(&End { shutdown_result: code, ..base }, 100);
        assert_eq!(r, vec![format!("shutdown result {code} is not the runtime's no-trailer result -10")], "{code}");
    }
    let all = trailerless_end_refusals(
        &End { io_finalized: true, join_failed: true, shutdown_result: -12, accepted_bytes: 1, ..base },
        100,
    );
    assert_eq!(all.len(), 3, "{all:?}");
    // With a trailer the exception does not apply: publish_decision alone decides.
    assert!(trailerless_end_refusals(&End { trailer_written: true, join_failed: true, ..base }, 100).is_empty());
}
