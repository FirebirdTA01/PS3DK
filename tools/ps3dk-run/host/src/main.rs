//! ps3dk-run host tool, protocol 2.  Trusted LAN only: HMAC gives
//! authentication and integrity, NOT confidentiality - file contents and output cross the LAN in clear.
//!
//! ps3dk-run [--key <path>] <console-ip> <self> [options] [-- child args...]      upload and run one SELF
//!   --file <local>=<remote>   side file, staged at <remote> relative to the SELF's run directory (repeatable)
//!   --callback-port <p>       port this host listens on for the child and the loader (default 4300)
//!   --callback-host <ipv4>   address the child and loader call back to (default: this host's end of the upload
//!                             connection; the console's view of us is wrong when a NAT router sits between)
//!   --timeout <s>             child watchdog timeout (0 = none; default 0)
//!   --repeat <n>              run the same upload n times, each after the loader's READY
//!   --outcome-json <path>     write the outcome records (JSON array)
//!   --disconnect-after-ms <n> close the child's connection n ms after HELLO (host-disconnect test)
//! ps3dk-run keygen <out-dir>          new pairing key: <out-dir>/PS3DKRUN.KEY for the console + the host copy
//! ps3dk-run unpair                    delete the host copy
//! ps3dk-run [--key <path>] ls <console> [dir]                      file service, root /dev_hdd0/tmp
//! ps3dk-run [--key <path>] get <console> <remote> <local>
//! ps3dk-run [--key <path>] put <console> <local> <remote> [--replace]
//! ps3dk-run [--key <path>] rm <console> <remote>
//! ps3dk-run [--key <path>] files-off <console>
//!
//! Every connection to the loader is authenticated (CHALLENGE/AUTH/AUTH_OK, mutual) and every later frame carries
//! an HMAC tag with a direction and sequence number.  The child's callback, the watchdog's TIMEOUT and the loader's
//! READY are tagged under the run key Kr = HMAC(Ks, "ps3dk-run/2 run" | run id); untagged or wrongly tagged
//! callback frames are dropped and their connection closed.
//!
//! Shell status: run: EXITED(c) c & 255, TIMEOUT 124, NO_HOOK 125, START_FAILED 127; everything: 3 REJECTED /
//! FILE_ERR, 4 BUSY, 5 authentication failure, 126 other failures, 2 usage.  Scripts that need the outcome read the
//! JSON record.
//!
//! START_FAILED: the child's hook sent HELLO (and was answered) but could not start - its watchdog or output sender
//! thread could not be created, or it did not accept HELLO_OK - and relaunched the loader instead of running the
//! child.  The detail names the reason.  A hook that fails before HELLO (run key, connect) can only relaunch: that
//! run still reports NO_HOOK, or DISCONNECTED when the loader's READY arrives first.

mod hmac;
mod keys;
mod proto;
mod sha256;

use proto::*;
use std::io::{self, Write};
use std::net::{Shutdown, TcpListener, TcpStream};
use std::path::PathBuf;
use std::sync::mpsc::{self, Receiver, RecvTimeoutError, Sender};
use std::thread;
use std::time::{Duration, Instant};

struct Opts {
    console: String,
    self_path: String,
    files: Vec<(String, String)>,
    cb_port: u16,
    cb_host: Option<std::net::Ipv4Addr>,
    timeout: u32,
    repeat: u32,
    outcome_json: Option<String>,
    disconnect_after_ms: Option<u64>,
    args: Vec<String>,
}

fn usage() -> ! {
    eprintln!("usage: ps3dk-run [--key path] <console-ip> <self> [--file local=remote]... [--callback-port p] [--callback-host ipv4]");
    eprintln!("                 [--timeout s] [--repeat n] [--outcome-json path] [--disconnect-after-ms n] [-- args...]");
    eprintln!("       ps3dk-run keygen <out-dir> | unpair");
    eprintln!("       ps3dk-run [--key path] ls <console> [dir] | get <console> <remote> <local> |");
    eprintln!("                 put <console> <local> <remote> [--replace] | rm <console> <remote> | files-off <console>");
    std::process::exit(2);
}

fn die(status: i32, msg: String) -> ! {
    eprintln!("ps3dk-run: {msg}");
    std::process::exit(status);
}

fn open_err_status(e: &OpenErr) -> i32 {
    match e {
        OpenErr::Busy => 4,
        OpenErr::Rejected(_) => 3,
        OpenErr::Auth(_) => 5,
        OpenErr::Io(_) => 126,
    }
}

fn parse_run(argv: Vec<String>) -> Opts {
    let mut it = argv.into_iter();
    let mut pos = Vec::new();
    let mut o = Opts {
        console: String::new(),
        self_path: String::new(),
        files: vec![],
        cb_port: 4300,
        cb_host: None,
        timeout: 0,
        repeat: 1,
        outcome_json: None,
        disconnect_after_ms: None,
        args: vec![],
    };
    while let Some(a) = it.next() {
        let mut val = || it.next().unwrap_or_else(|| usage());
        match a.as_str() {
            "--file" => {
                let v = val();
                let (l, r) = v.split_once('=').unwrap_or_else(|| usage());
                o.files.push((l.to_string(), r.to_string()));
            }
            "--callback-port" => o.cb_port = val().parse().unwrap_or_else(|_| usage()),
            "--callback-host" => o.cb_host = Some(val().parse().unwrap_or_else(|_| usage())),
            "--timeout" => o.timeout = val().parse().unwrap_or_else(|_| usage()),
            "--repeat" => o.repeat = val().parse().unwrap_or_else(|_| usage()),
            "--outcome-json" => o.outcome_json = Some(val()),
            "--disconnect-after-ms" => o.disconnect_after_ms = Some(val().parse().unwrap_or_else(|_| usage())),
            "--" => {
                o.args = it.by_ref().collect();
                break;
            }
            _ if a.starts_with("--") => usage(),
            _ => pos.push(a),
        }
    }
    if pos.len() != 2 {
        usage();
    }
    o.console = pos[0].clone();
    o.self_path = pos[1].clone();
    o
}

/// Frames from callback connections, each connection on its own thread; tags are checked by the run loop, which
/// alone knows the run key.  `at` is taken on the connection's thread when the event happens (monotonic clock).
enum Ev {
    Frame { conn: u32, t: u8, p: Vec<u8>, tag: [u8; TAG], at: Instant },
    Closed { conn: u32, at: Instant },
    Stream { conn: u32, s: TcpStream, peer: String, at: Instant },
}

fn listener(l: TcpListener, tx: Sender<Ev>) {
    let mut n = 0u32;
    for s in l.incoming().flatten() {
        let at = Instant::now();
        n += 1;
        let conn = n;
        let tx = tx.clone();
        thread::spawn(move || {
            let mut s = s;
            let peer = s.peer_addr().map(|a| a.to_string()).unwrap_or_else(|_| "?".into());
            if let Ok(c) = s.try_clone() {
                let _ = tx.send(Ev::Stream { conn, s: c, peer, at });
            }
            loop {
                match get_tagged(&mut s) {
                    Ok((t, p, tag)) => {
                        let _ = tx.send(Ev::Frame { conn, t, p, tag, at: Instant::now() });
                    }
                    Err(_) => {
                        let _ = tx.send(Ev::Closed { conn, at: Instant::now() });
                        return;
                    }
                }
            }
        });
    }
}

/// One callback connection seen during a run (launch-reliability instrument I1): every accept is recorded, with its
/// first frame or none and its close.  Times are milliseconds after SPAWNING (negative: accepted before it).
struct ConnLog {
    conn: u32,
    peer: String,
    accepted_ms: i64,
    first: Option<(u8, i64)>,
    closed_ms: Option<i64>,
}

fn ms_after(at: Instant, start: Instant) -> i64 {
    if at >= start { (at - start).as_millis() as i64 } else { -((start - at).as_millis() as i64) }
}

fn conns_json(conns: &[ConnLog]) -> String {
    let opt = |v: Option<i64>| v.map_or("null".to_string(), |v| v.to_string());
    let items: Vec<String> = conns
        .iter()
        .map(|c| {
            format!(
                "{{\"conn\":{},\"peer\":{},\"accepted_ms\":{},\"first_frame\":{},\"first_frame_ms\":{},\"closed_ms\":{}}}",
                c.conn,
                json_str(&c.peer),
                c.accepted_ms,
                c.first.map_or("null".to_string(), |(t, _)| t.to_string()),
                opt(c.first.map(|(_, m)| m)),
                opt(c.closed_ms)
            )
        })
        .collect();
    format!("[{}]", items.join(","))
}

struct Outcome {
    kind: String,
    detail: String,
    run: u32,
    dropped: u64,
    ready: bool,
    status: i32,
    conns: Vec<ConnLog>,
}

fn json_str(s: &str) -> String {
    let mut o = String::from("\"");
    for c in s.chars() {
        match c {
            '"' => o.push_str("\\\""),
            '\\' => o.push_str("\\\\"),
            c if (c as u32) < 0x20 => o.push_str(&format!("\\u{:04x}", c as u32)),
            c => o.push(c),
        }
    }
    o.push('"');
    o
}

/// One upload on a fresh authenticated session; returns the run id and the run key.
fn upload(o: &Opts, key: &[u8; 32], payload_files: &[(String, Vec<u8>)]) -> Result<(u32, [u8; 32]), Outcome> {
    let fail = |kind: &str, detail: String, status: i32| Outcome { kind: kind.into(), detail, run: 0, dropped: 0, ready: false, status, conns: Vec::new() };
    let mut c = proto::open(&o.console, key).map_err(|e| {
        let st = open_err_status(&e);
        match e {
            OpenErr::Busy => fail("BUSY", "another session is active".into(), st),
            OpenErr::Rejected(r) => fail("REJECTED", r, st),
            OpenErr::Auth(r) => fail("AUTH_FAILED", r, st),
            OpenErr::Io(r) => fail("DISCONNECTED", r, st),
        }
    })?;
    let mut h = Vec::new();
    h.extend_from_slice(&(payload_files.len() as u32).to_be_bytes());
    h.extend_from_slice(&(o.args.len() as u32).to_be_bytes());
    h.extend_from_slice(&o.cb_port.to_be_bytes());
    h.extend_from_slice(&o.timeout.to_be_bytes());
    // the callback address: this host's end of the upload route (the loader's peer address is a router's behind NAT)
    let (cb_ip, how): (u32, &str) = match (&o.cb_host, c.s.local_addr()) {
        (Some(ip), _) => (u32::from(*ip), "explicit"),
        (None, Ok(std::net::SocketAddr::V4(a))) => (u32::from(*a.ip()), "local end of the upload route; not proof the console can reach it"),
        _ => (0, "the loader's peer address"),
    };
    eprintln!("ps3dk-run: callback {}:{} ({how})", std::net::Ipv4Addr::from(cb_ip), o.cb_port);
    h.extend_from_slice(&cb_ip.to_be_bytes());
    for a in &o.args {
        h.extend_from_slice(&(a.len() as u16).to_be_bytes());
        h.extend_from_slice(a.as_bytes());
    }
    for (remote, data) in payload_files {
        h.extend_from_slice(&(remote.len() as u16).to_be_bytes());
        h.extend_from_slice(remote.as_bytes());
        h.extend_from_slice(&(data.len() as u64).to_be_bytes());
        h.extend_from_slice(&sha256::digest(data));
    }
    let io = |e: io::Error| fail("DISCONNECTED", format!("upload: {e}"), 126);
    let rejected = |p: &[u8]| fail("REJECTED", String::from_utf8_lossy(p).into(), 3);
    c.send(T_HEADER_CB, &h).map_err(io)?;
    for (i, (_, data)) in payload_files.iter().enumerate() {
        for chunk in data.chunks(64 * 1024) {
            let mut f = (i as u32).to_be_bytes().to_vec();
            f.extend_from_slice(chunk);
            c.send(T_FILE, &f).map_err(io)?;
        }
    }
    c.send(T_COMMIT, &[]).map_err(io)?;
    let run = match c.recv().map_err(io)? {
        (T_ACCEPTED, p) if p.len() >= 4 => be32(&p),
        (T_REJECTED, p) => return Err(rejected(&p)),
        (t, _) => return Err(fail("REJECTED", format!("unexpected frame {t}"), 3)),
    };
    let kr = run_key(c.session_key(), run);
    eprintln!("ps3dk-run: upload accepted, run {run}");
    match c.recv() {
        Ok((T_SPAWNING, _)) => eprintln!("ps3dk-run: run {run}: spawning"),
        Ok((T_REJECTED, p)) => return Err(Outcome { run, ..rejected(&p) }),
        Ok((t, _)) => return Err(Outcome { run, ..fail("REJECTED", format!("expected SPAWNING, got {t}"), 3) }),
        Err(e) => return Err(Outcome { run, ..fail("DISCONNECTED", format!("before SPAWNING: {e}"), 126) }),
    }
    // The loader is replaced by the child on success (the connection just closes); on failure it reports.
    c.s.set_read_timeout(Some(Duration::from_secs(15))).ok();
    if let Ok((T_SPAWN_FAILED, p)) = c.recv() {
        let err = if p.len() >= 4 { be32(&p) as i32 } else { -1 };
        return Err(Outcome { run, ..fail("SPAWN_FAILED", format!("error {err}"), 126) });
    }
    Ok((run, kr))
}

fn run_once(o: &Opts, key: &[u8; 32], files: &[(String, Vec<u8>)], rx: &Receiver<Ev>) -> Outcome {
    let (run, kr) = match upload(o, key, files) {
        Ok(r) => r,
        Err(out) => return out,
    };
    let mut out = Outcome { kind: String::new(), detail: String::new(), run, dropped: 0, ready: false, status: 126, conns: Vec::new() };
    let mut child: Option<(u32, TcpStream)> = None;
    let mut child_seq = 0u32; // next expected 'C' sequence number on the child connection
    let mut streams: Vec<(u32, TcpStream)> = Vec::new();
    let mut hello_at: Option<Instant> = None;
    let start = Instant::now();
    let mut terminal_at: Option<Instant> = None;
    let drop_conn = |streams: &mut Vec<(u32, TcpStream)>, conn: u32, why: &str| {
        eprintln!("ps3dk-run: run {run}: callback connection {conn} dropped: {why}");
        if let Some(i) = streams.iter().position(|(c, _)| *c == conn) {
            let _ = streams[i].1.shutdown(Shutdown::Both);
            streams.remove(i);
        }
    };
    loop {
        let wait = match (terminal_at, hello_at) {
            (Some(t), _) => Duration::from_secs(60).saturating_sub(t.elapsed()),
            (None, None) => Duration::from_secs(60).saturating_sub(start.elapsed()),
            (None, Some(_)) => Duration::from_secs(3600),
        };
        if let (Some(ms), Some(h), Some((_, s))) = (o.disconnect_after_ms, hello_at, child.as_ref()) {
            if terminal_at.is_none() && h.elapsed() >= Duration::from_millis(ms) {
                eprintln!("ps3dk-run: run {run}: closing the child connection (host-disconnect test)");
                let _ = s.shutdown(Shutdown::Both);
                out.kind = "DISCONNECTED".into();
                out.detail = "host closed the child connection (test)".into();
                terminal_at = Some(Instant::now());
            }
        }
        let tick = if o.disconnect_after_ms.is_some() && terminal_at.is_none() { wait.min(Duration::from_millis(50)) } else { wait };
        match rx.recv_timeout(tick) {
            Ok(Ev::Stream { conn, s, peer, at }) => {
                let m = ms_after(at, start);
                eprintln!("ps3dk-run: run {run}: callback connection {conn} accepted from {peer} at {m} ms after spawning");
                out.conns.push(ConnLog { conn, peer, accepted_ms: m, first: None, closed_ms: None });
                streams.push((conn, s))
            }
            Ok(Ev::Frame { conn, t, p, tag, at }) => {
                if let Some(c) = out.conns.iter_mut().find(|c| c.conn == conn && c.first.is_none()) {
                    let m = ms_after(at, start);
                    eprintln!("ps3dk-run: run {run}: callback connection {conn} first frame type {t} at {m} ms");
                    c.first = Some((t, m));
                }
                let is_child = child.as_ref().map(|(c, _)| *c) == Some(conn);
                if is_child {
                    if !tag_ok(&kr, DIR_CHILD, child_seq, t, &p, &tag) {
                        if let Some((_, s)) = child.take() {
                            let _ = s.shutdown(Shutdown::Both);
                        }
                        if terminal_at.is_none() {
                            out.kind = "DISCONNECTED".into();
                            out.detail = "child frame failed authentication (bad tag or out of sequence)".into();
                            terminal_at = Some(Instant::now());
                        }
                        continue;
                    }
                    child_seq += 1;
                    match t {
                        T_STDOUT => {
                            let _ = io::stdout().write_all(&p);
                            let _ = io::stdout().flush();
                        }
                        T_STDERR => {
                            let _ = io::stderr().write_all(&p);
                        }
                        T_START_FAILED if p.len() >= 4 && terminal_at.is_none() => {
                            let reason = be32(&p);
                            out.kind = "START_FAILED".into();
                            out.detail = match reason {
                                1 => "hook did not accept HELLO_OK".into(),
                                2 => "hook could not create its watchdog thread".into(),
                                3 => "hook could not create its output sender thread".into(),
                                r => format!("hook start failed, reason {r}"),
                            };
                            out.status = 127;
                            terminal_at = Some(Instant::now());
                        }
                        T_EXIT if p.len() >= 12 && terminal_at.is_none() => {
                            let code = be32(&p) as i32;
                            out.kind = "EXITED".into();
                            out.detail = code.to_string();
                            out.dropped = be64(&p[4..]);
                            out.status = code & 0xff;
                            terminal_at = Some(Instant::now());
                        }
                        _ => {}
                    }
                    continue;
                }
                // a new connection: its first frame says what it is, and must be tagged under this run's key
                match t {
                    T_HELLO if p.len() >= 4 && child.is_none() && tag_ok(&kr, DIR_CHILD, 0, t, &p, &tag) => {
                        if be32(&p) != run {
                            drop_conn(&mut streams, conn, "HELLO for another run");
                            continue;
                        }
                        let Some(i) = streams.iter().position(|(c, _)| *c == conn) else { continue };
                        let (c, mut s) = streams.remove(i);
                        let ok = run.to_be_bytes();
                        let mut f = frame_bytes(T_HELLO_OK, &ok);
                        f.extend_from_slice(&proto::tag(&kr, DIR_HOST_C, 0, T_HELLO_OK, &ok));
                        if s.write_all(&f).is_err() {
                            continue;
                        }
                        eprintln!("ps3dk-run: run {run}: child connected");
                        hello_at = Some(Instant::now());
                        child_seq = 1;
                        child = Some((c, s));
                    }
                    T_TIMEOUT if p.len() >= 12 && tag_ok(&kr, DIR_WDOG, 0, t, &p, &tag) => {
                        if be32(&p) == run && terminal_at.is_none() {
                            out.kind = "TIMEOUT".into();
                            out.dropped = be64(&p[4..]);
                            out.status = 124;
                            terminal_at = Some(Instant::now());
                        }
                    }
                    T_READY if p.len() >= 4 && tag_ok(&kr, DIR_READY, 0, t, &p, &tag) => {
                        if be32(&p) == run {
                            out.ready = true;
                            if out.kind.is_empty() {
                                out.kind = "DISCONNECTED".into();
                                out.detail = "loader READY without a terminal outcome".into();
                            }
                            return out;
                        }
                    }
                    _ => drop_conn(&mut streams, conn, "first frame not authenticated for this run"),
                }
            }
            Ok(Ev::Closed { conn, at }) => {
                if let Some(c) = out.conns.iter_mut().find(|c| c.conn == conn) {
                    let m = ms_after(at, start);
                    eprintln!(
                        "ps3dk-run: run {run}: callback connection {conn} closed at {m} ms{}",
                        if c.first.is_none() { " (no frame received)" } else { "" }
                    );
                    c.closed_ms = Some(m);
                }
                if child.as_ref().map(|(c, _)| *c) == Some(conn) && terminal_at.is_none() {
                    out.kind = "DISCONNECTED".into();
                    out.detail = "child connection closed without EXIT".into();
                    terminal_at = Some(Instant::now());
                }
                streams.retain(|(c, _)| *c != conn);
            }
            Err(RecvTimeoutError::Timeout) => {
                if terminal_at.map_or(false, |t| t.elapsed() >= Duration::from_secs(60)) {
                    out.detail = format!("{}{}no READY from the loader", out.detail, if out.detail.is_empty() { "" } else { "; " });
                    return out;
                }
                if hello_at.is_none() && terminal_at.is_none() && start.elapsed() >= Duration::from_secs(60) {
                    out.kind = "NO_HOOK".into();
                    out.detail = "no HELLO within 60 s of spawning".into();
                    out.status = 125;
                    return out;
                }
            }
            Err(RecvTimeoutError::Disconnected) => {
                out.kind = "DISCONNECTED".into();
                out.detail = "callback listener stopped".into();
                return out;
            }
        }
    }
}

fn cmd_run(o: Opts, key: [u8; 32]) -> ! {
    let self_data = std::fs::read(&o.self_path).unwrap_or_else(|e| die(2, format!("{}: {e}", o.self_path)));
    let self_name = std::path::Path::new(&o.self_path).file_name().unwrap().to_string_lossy().to_string();
    let mut files = vec![(self_name, self_data)];
    for (l, r) in &o.files {
        let d = std::fs::read(l).unwrap_or_else(|e| die(2, format!("{l}: {e}")));
        files.push((r.clone(), d));
    }
    let self_hash = sha256::hex(&sha256::digest(&files[0].1));
    let l = TcpListener::bind(("0.0.0.0", o.cb_port)).unwrap_or_else(|e| die(126, format!("cannot listen on callback port {}: {e}", o.cb_port)));
    eprintln!("ps3dk-run: listening on callback port {} before upload; SELF sha256 {self_hash}", o.cb_port);
    let (tx, rx) = mpsc::channel();
    thread::spawn(move || listener(l, tx));

    let mut records = Vec::new();
    let mut status = 0;
    for i in 0..o.repeat {
        let out = run_once(&o, &key, &files, &rx);
        eprintln!(
            "ps3dk-run: run {} outcome {}{} dropped {} ready {}",
            out.run,
            out.kind,
            if out.detail.is_empty() { String::new() } else { format!(" ({})", out.detail) },
            out.dropped,
            out.ready
        );
        records.push(format!(
            "{{\"index\":{},\"run_id\":{},\"outcome\":{},\"detail\":{},\"self_sha256\":\"{}\",\"dropped_bytes\":{},\"loader_ready\":{},\"protocol\":2,\"connections\":{}}}",
            i, out.run, json_str(&out.kind), json_str(&out.detail), self_hash, out.dropped, out.ready, conns_json(&out.conns)
        ));
        status = out.status;
        if !out.ready {
            if i + 1 < o.repeat {
                eprintln!("ps3dk-run: stopping the run list: no READY from the loader");
            }
            break;
        }
    }
    if let Some(p) = &o.outcome_json {
        if let Err(e) = std::fs::write(p, format!("[{}]\n", records.join(",\n"))) {
            eprintln!("ps3dk-run: {p}: {e}");
        }
    }
    std::process::exit(status);
}

// ---- file service (root 1 = /dev_hdd0/tmp) ----

const ROOT_TMP: u8 = 1;

fn path_req(req: u32, path: &str) -> Vec<u8> {
    let mut b = req.to_be_bytes().to_vec();
    b.push(ROOT_TMP);
    b.extend_from_slice(&(path.len() as u16).to_be_bytes());
    b.extend_from_slice(path.as_bytes());
    b
}

fn file_session(console: &str, key: &[u8; 32]) -> Chan {
    proto::open(console, key).unwrap_or_else(|e| die(open_err_status(&e), e.to_string()))
}

/// The reply to request `req`: FILE_OK -> Ok(None), the given type -> Ok(Some(payload after the req id)).
fn file_reply(c: &mut Chan, req: u32, want: u8) -> Result<Option<Vec<u8>>, (i32, String)> {
    loop {
        let (t, p) = c.recv().map_err(|e| (126, e.to_string()))?;
        if p.len() < 4 {
            return Err((126, format!("short reply frame {t}")));
        }
        if be32(&p) != req {
            continue;
        }
        return match t {
            T_FILE_OK => Ok(None),
            T_FILE_ERR => Err((3, format!("FILE_ERR: {}", String::from_utf8_lossy(&p[4..])))),
            _ if t == want => Ok(Some(p[4..].to_vec())),
            _ => Err((126, format!("unexpected reply frame {t}"))),
        };
    }
}

fn finish(c: Chan, r: Result<(), (i32, String)>) -> ! {
    c.bye();
    match r {
        Ok(()) => std::process::exit(0),
        Err((st, m)) => die(st, m),
    }
}

fn cmd_ls(console: &str, dir: &str, key: [u8; 32]) -> ! {
    let mut c = file_session(console, &key);
    let r = (|| {
        c.send(T_LIST, &path_req(1, dir)).map_err(|e| (126, e.to_string()))?;
        let p = file_reply(&mut c, 1, T_LIST_RESULT)?.ok_or((126, "LIST answered FILE_OK".to_string()))?;
        if p.len() < 6 {
            return Err((126, "short LIST_RESULT".into()));
        }
        let (truncated, unstable, n) = (p[0], p[1], be32(&p[2..]));
        let mut at = 6usize;
        for _ in 0..n {
            if at + 11 > p.len() {
                return Err((126, "LIST_RESULT entry truncated".into()));
            }
            let (ty, size, nl) = (p[at], be64(&p[at + 1..]), u16::from_be_bytes([p[at + 9], p[at + 10]]) as usize);
            let name = String::from_utf8_lossy(p.get(at + 11..at + 11 + nl).ok_or((126, "LIST_RESULT name truncated".to_string()))?);
            println!("{} {:>12} {}", match ty { b'd' => 'd', b'f' => '-', _ => '?' }, size, name);
            at += 11 + nl;
        }
        if truncated != 0 {
            eprintln!("ps3dk-run: listing truncated by the loader");
        }
        if unstable != 0 {
            eprintln!("ps3dk-run: the directory changed while it was listed");
        }
        Ok(())
    })();
    finish(c, r)
}

fn cmd_get(console: &str, remote: &str, local: &str, key: [u8; 32]) -> ! {
    let mut c = file_session(console, &key);
    let r = (|| {
        c.send(T_GET, &path_req(1, remote)).map_err(|e| (126, e.to_string()))?;
        let mut data = Vec::new();
        loop {
            let (t, p) = c.recv().map_err(|e| (126, e.to_string()))?;
            if p.len() < 4 || be32(&p) != 1 {
                continue;
            }
            match t {
                T_GET_DATA => data.extend_from_slice(&p[4..]),
                T_GET_END if p.len() >= 4 + 8 + 32 => {
                    let (n, sha) = (be64(&p[4..]), &p[12..44]);
                    if n != data.len() as u64 || sha != sha256::digest(&data) {
                        return Err((126, "GET size or sha256 mismatch: nothing written".into()));
                    }
                    let tmp = format!("{local}.partial");
                    std::fs::write(&tmp, &data).and_then(|_| std::fs::rename(&tmp, local)).map_err(|e| (126, format!("{local}: {e}")))?;
                    eprintln!("ps3dk-run: {remote} -> {local}: {n} bytes, sha256 {}", sha256::hex(sha));
                    return Ok(());
                }
                T_FILE_ERR => return Err((3, format!("FILE_ERR: {}", String::from_utf8_lossy(&p[4..])))),
                _ => return Err((126, format!("unexpected frame {t} during GET"))),
            }
        }
    })();
    finish(c, r)
}

fn cmd_put(console: &str, local: &str, remote: &str, replace: bool, key: [u8; 32]) -> ! {
    let data = std::fs::read(local).unwrap_or_else(|e| die(2, format!("{local}: {e}")));
    let mut c = file_session(console, &key);
    let r = (|| {
        let io = |e: io::Error| (126, e.to_string());
        let mut b = path_req(1, remote);
        b.extend_from_slice(&(data.len() as u64).to_be_bytes());
        b.extend_from_slice(&sha256::digest(&data));
        b.push(replace as u8);
        c.send(T_PUT, &b).map_err(io)?;
        for chunk in data.chunks(64 * 1024) {
            let mut f = 1u32.to_be_bytes().to_vec();
            f.extend_from_slice(chunk);
            c.send(T_PUT_DATA, &f).map_err(io)?;
        }
        c.send(T_PUT_COMMIT, &1u32.to_be_bytes()).map_err(io)?;
        file_reply(&mut c, 1, T_FILE_OK)?;
        eprintln!("ps3dk-run: {local} -> {remote}: {} bytes published", data.len());
        Ok(())
    })();
    finish(c, r)
}

fn cmd_simple(console: &str, t: u8, payload: Vec<u8>, what: &str, key: [u8; 32]) -> ! {
    let mut c = file_session(console, &key);
    let r = (|| {
        c.send(t, &payload).map_err(|e| (126, e.to_string()))?;
        file_reply(&mut c, 1, T_FILE_OK)?;
        eprintln!("ps3dk-run: {what}: done");
        Ok(())
    })();
    finish(c, r)
}

fn main() {
    let mut argv: Vec<String> = std::env::args().skip(1).collect();
    if matches!(argv.first().map(|s| s.as_str()), Some("--version" | "-V")) {
        println!("ps3dk-run {}", ps3dk_version::VERSION);
        return;
    }
    let mut key_path: Option<PathBuf> = None;
    if argv.first().map(|s| s.as_str()) == Some("--key") {
        if argv.len() < 2 {
            usage();
        }
        key_path = Some(PathBuf::from(argv.remove(1)));
        argv.remove(0);
    }
    let host_key = || key_path.clone().or_else(keys::default_path).unwrap_or_else(|| die(2, "no profile directory for the key; use --key".into()));
    let load = || {
        let p = host_key();
        let k = keys::load(&p).unwrap_or_else(|e| die(5, e));
        eprintln!("ps3dk-run: key {} (fingerprint {})", p.display(), keys::fingerprint(&k));
        k
    };
    let a: Vec<&str> = argv.iter().map(|s| s.as_str()).collect();
    match a.as_slice() {
        ["keygen", dir] => keys::keygen(dir, &host_key()).map(|_| std::process::exit(0)).unwrap_or_else(|e| die(126, e)),
        ["unpair"] => keys::unpair(&host_key()).map(|_| std::process::exit(0)).unwrap_or_else(|e| die(126, e)),
        ["ls", console] => cmd_ls(console, "", load()),
        ["ls", console, dir] => cmd_ls(console, dir, load()),
        ["get", console, remote, local] => cmd_get(console, remote, local, load()),
        ["put", console, local, remote] => cmd_put(console, local, remote, false, load()),
        ["put", console, local, remote, "--replace"] => cmd_put(console, local, remote, true, load()),
        ["rm", console, remote] => cmd_simple(console, T_DELETE, path_req(1, remote), &format!("delete {remote}"), load()),
        ["files-off", console] => cmd_simple(console, T_FILES_OFF, 1u32.to_be_bytes().to_vec(), "file service OFF", load()),
        [cmd, ..] if ["keygen", "unpair", "ls", "get", "put", "rm", "files-off"].contains(cmd) => usage(),
        _ => {
            let o = parse_run(argv);
            cmd_run(o, load())
        }
    }
}
