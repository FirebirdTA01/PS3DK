//! Peak memory of a live session, in its own test binary so the process
//! high-water mark belongs to this test alone. Linux only (VmHWM).

#[path = "../../ps3dk-prof/tests/common/mod.rs"]
#[rustfmt::skip]
#[allow(clippy::all)]
mod common;

#[cfg(target_os = "linux")]
#[test]
fn peak_memory_of_a_live_session_is_reported() {
    use common::*;
    use ps3dk_prof_viewer::live::*;
    use ps3dk_prof_viewer::live_client::*;
    use std::io::{Read, Write};
    use std::net::TcpListener;
    use std::time::{Duration, Instant};

    fn kib(field: &str) -> u64 {
        let s = std::fs::read_to_string("/proc/self/status").unwrap();
        let line = s.lines().find(|l| l.starts_with(field)).unwrap();
        line.split_whitespace().nth(1).unwrap().parse().unwrap()
    }
    fn wire(t: u16, payload: &[u8]) -> Vec<u8> {
        let mut f = t.to_le_bytes().to_vec();
        f.extend_from_slice(&0u16.to_le_bytes());
        f.extend_from_slice(&(payload.len() as u32).to_le_bytes());
        f.extend_from_slice(payload);
        f
    }

    // A capture of 48,000 zone instances (several MiB) on 8 lanes (one
    // writer chunk per lane stays under the reader's chunk cap), streamed as
    // 1 KiB DATA frames.
    let mut lanes = Vec::new();
    for inc in 1..=8u32 {
        let mut recs = vec![frame(0, 1000, 0)];
        let mut seq = 1;
        for i in 0..6_000u64 {
            let tb = 1100 + i * 100;
            recs.extend(zone(seq, 0, 0, tb, i + 1, 0));
            recs.push(end(seq + 2, 0, 0, tb + 50, i + 1));
            seq += 3;
        }
        lanes.push(lane(inc, recs));
    }
    let cap = scenario(lanes).bytes();
    match ps3dk_prof::load(&cap) {
        ps3dk_prof::Outcome::Analysed(a) => assert!(
            matches!(a.verdict, ps3dk_prof::Verdict::Complete),
            "the test capture itself is not COMPLETE: {}",
            ps3dk_prof::report::verdict_line(&a.verdict)
        ),
        ps3dk_prof::Outcome::Malformed(m) => panic!("test capture malformed: {m}"),
    }
    let mut stream = Vec::new();
    let mut p = Vec::new();
    p.extend_from_slice(&VERSION.to_le_bytes());
    p.extend_from_slice(&1u16.to_le_bytes());
    p.extend_from_slice(&0u32.to_le_bytes());
    p.extend_from_slice(&7u64.to_le_bytes());
    for s in [&b"mem"[..], b"", b"mem.prof"] {
        p.extend_from_slice(&(s.len() as u16).to_le_bytes());
        p.extend_from_slice(s);
    }
    stream.extend(wire(T_HELLO, &p));
    for (i, c) in cap.chunks(1024).enumerate() {
        let mut d = ((i * 1024) as u64).to_le_bytes().to_vec();
        d.extend_from_slice(c);
        stream.extend(wire(T_DATA, &d));
    }
    let mut e = vec![1, 1, 0, 0];
    e.extend_from_slice(&0i32.to_le_bytes());
    e.extend_from_slice(&(cap.len() as u64).to_le_bytes());
    e.extend_from_slice(&(cap.len() as u64).to_le_bytes());
    stream.extend(wire(T_END, &e));

    let l = TcpListener::bind("127.0.0.1:0").unwrap();
    let port = l.local_addr().unwrap().port();
    std::thread::spawn(move || {
        let (mut s, _) = l.accept().unwrap();
        let mut h = [0u8; 16];
        s.read_exact(&mut h).unwrap();
        for c in stream.chunks(64 * 1024) {
            s.write_all(c).unwrap();
            std::thread::sleep(Duration::from_millis(2));
        }
    });
    let dir = std::env::temp_dir().join(format!("ps3dk-live-mem-{}", std::process::id()));
    let _ = std::fs::remove_dir_all(&dir);
    std::fs::create_dir_all(&dir).unwrap();

    let rss_before = kib("VmRSS:");
    let hwm_before = kib("VmHWM:");
    let mut c = ClientConfig::new("127.0.0.1", port, dir.clone());
    c.parse_every = Duration::from_millis(50);
    let client = LiveClient::start(c).expect("a valid config");
    let t0 = Instant::now();
    while !client.finished() {
        assert!(t0.elapsed() < Duration::from_secs(60));
        std::thread::sleep(Duration::from_millis(10));
    }
    let v = client.join();
    let hwm_after = kib("VmHWM:");
    assert!(
        matches!(v.outcome, Some(Outcome::Published(_))),
        "{:?}",
        v.outcome
    );
    let len = cap.len() as u64;
    println!(
        "MEASURE capture {} KiB, {} parses; client byte-buffer ESTIMATE {} KiB (max over parses of received + copied tail + flattened parse buffer; not a measured concurrent peak; EXCLUDES the reader's analysis, the 64 KiB read buffer and allocator slack); process RSS before {} KiB, VmHWM {} -> {} KiB (+{} KiB: everything, incl. the analysis and this test's own copies)",
        len / 1024,
        v.parses,
        v.snapshot_buffers_estimate / 1024,
        rss_before,
        hwm_before,
        hwm_after,
        hwm_after.saturating_sub(hwm_before)
    );
    // A sanity bound on the real process peak (client buffers, the reader's
    // analysis, this test's stream and capture copies): far below what a
    // per-frame or per-parse leak would cost.
    assert!(hwm_after.saturating_sub(hwm_before) * 1024 < 20 * len);
    let _ = std::fs::remove_dir_all(&dir);
}
