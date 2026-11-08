//! Live link client: one network thread and one parse worker per session.
//! No egui types; the UI polls `LiveClient::view`.
//!
//! - The network thread decodes frames into the session, appends accepted
//!   DATA to the local copy and pings. It publishes only after END AND a
//!   clean EOF from the agent within `close_wait`: any read error, a byte
//!   after END, or no close in time refuses complete publication.
//! - The parse worker is the only place the reader runs. It parses an
//!   immutable snapshot at most once per `parse_every`, and produces the
//!   parse of the final bytes, which the network thread waits for (outside
//!   the shared lock) before it decides publication.
//!
//! Waits: connecting is bounded by `connect_timeout`, HELLO must arrive
//! within `hello_timeout`, and while streaming some byte (data or PONG)
//! must arrive within `liveness_timeout`. These are SOCKET-wait bounds: a
//! disconnect request is seen within the 100 ms read wait, or after an
//! in-progress connect attempt or blocked write (bounded by
//! `connect_timeout` and the 2 s write timeout). File writes, parsing and
//! finalization are not bounded by them.

use crate::live::*;
use crate::model::{Document, VerdictTone};
use std::io::{ErrorKind, Read, Write};
use std::net::{IpAddr, Shutdown, SocketAddr, TcpStream};
use std::path::PathBuf;
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::{Arc, Condvar, Mutex};
use std::thread::JoinHandle;
use std::time::{Duration, Instant};

const READ_WAIT: Duration = Duration::from_millis(100);
const WRITE_TIMEOUT: Duration = Duration::from_secs(2);

/// The agent's PING limit (live_proto.h LIVE_MAX_PINGS_PER_S).
pub const MAX_PINGS_PER_S: u32 = 4;

#[derive(Debug, Clone)]
pub struct ClientConfig {
    /// The console's IP address (a literal: no name lookup, which could
    /// block without a deadline).
    pub host: String,
    pub port: u16,
    /// Where `.partial`, `.prof` and `.incomplete.prof` files go.
    pub out_dir: PathBuf,
    pub connect_timeout: Duration,
    /// After CLIENT_HELLO, HELLO must arrive within this.
    pub hello_timeout: Duration,
    /// While streaming, some byte must arrive within this (PINGs make the
    /// agent answer, so a healthy idle stream still sends PONGs).
    pub liveness_timeout: Duration,
    /// After END, how long the agent has to close the connection.
    pub close_wait: Duration,
    /// At least 250 ms: the agent accepts at most 4 PINGs a second.
    pub ping_every: Duration,
    pub parse_every: Duration,
    /// Capture bytes accepted at most.
    pub cap: u64,
    /// Tests only: fault injection for publishing a session that ended early.
    pub early_fault: Option<EarlyFault>,
}

impl ClientConfig {
    pub fn new(host: &str, port: u16, out_dir: PathBuf) -> ClientConfig {
        ClientConfig {
            host: host.to_string(),
            port,
            out_dir,
            connect_timeout: Duration::from_secs(3),
            hello_timeout: Duration::from_secs(3),
            liveness_timeout: Duration::from_secs(5),
            close_wait: Duration::from_secs(2),
            ping_every: Duration::from_secs(1),
            parse_every: Duration::from_millis(250),
            cap: MAX_SESSION_BYTES,
            early_fault: None,
        }
    }

    /// Checks the config against the protocol and the client's bounds.
    pub fn validate(&self) -> Result<SocketAddr, String> {
        let ip: IpAddr = self
            .host
            .trim()
            .parse()
            .map_err(|_| format!("'{}' is not an IP address", self.host))?;
        if self.port == 0 {
            return Err("the port must be 1..65535".into());
        }
        let within = |d: Duration, lo_ms: u64, hi_ms: u64| {
            d >= Duration::from_millis(lo_ms) && d <= Duration::from_millis(hi_ms)
        };
        let min_ping_ms = 1000 / u64::from(MAX_PINGS_PER_S);
        if !within(self.ping_every, min_ping_ms, 60_000) {
            return Err(format!(
                "ping_every must be {min_ping_ms} ms..60 s (the agent accepts at most {MAX_PINGS_PER_S} PINGs a second)"
            ));
        }
        for (name, d) in [
            ("connect_timeout", self.connect_timeout),
            ("hello_timeout", self.hello_timeout),
            ("close_wait", self.close_wait),
        ] {
            if !within(d, 100, 60_000) {
                return Err(format!("{name} must be 100 ms..60 s"));
            }
        }
        if self.liveness_timeout < 2 * self.ping_every
            || self.liveness_timeout > Duration::from_secs(300)
        {
            return Err("liveness_timeout must be 2 x ping_every..300 s".into());
        }
        if !within(self.parse_every, 20, 5_000) {
            return Err("parse_every must be 20 ms..5 s".into());
        }
        if self.cap == 0 || self.cap > MAX_SESSION_BYTES {
            return Err(format!("cap must be 1..{MAX_SESSION_BYTES} bytes"));
        }
        Ok(SocketAddr::new(ip, self.port))
    }
}

/// How a session ended.
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum Outcome {
    /// Every check passed; the capture was published as a `.prof`.
    Published(Published),
    /// The session ended early (`cause`) after valid data: the accepted prefix, cut at its last whole chunk, was
    /// published as a `.prof` (see `ps3dk_prof::load_prefix`).
    PublishedEarly {
        published: Published,
        cause: String,
        cut: u64,
        torn_dropped: u64,
    },
    /// Kept as `.incomplete.prof` (or not saved at all), with every reason.
    Incomplete {
        saved: Option<Published>,
        reasons: Vec<String>,
    },
}

/// What the UI shows. Link state, the capture verdict (inside `doc`) and
/// the outcome are separate fields.
#[derive(Clone)]
pub struct LiveView {
    pub link: LinkState,
    pub hello: Option<Hello>,
    pub received: u64,
    pub pongs: u64,
    /// The newest parsed snapshot; PROVISIONAL until `outcome` is set.
    pub doc: Option<Arc<Document>>,
    /// Increases with every new `doc`.
    pub generation: u64,
    pub parses: u64,
    pub local_error: Option<String>,
    pub outcome: Option<Outcome>,
    /// An ESTIMATE, per parse, of the client's byte buffers: received bytes,
    /// the snapshot's copied tail and the flattened parse buffer, maximised
    /// over parses. It is not a measured concurrent peak (bytes may arrive
    /// during a parse; sealing a block has transients) and it excludes the
    /// reader's analysis, the network read buffer and allocator slack. The
    /// memory test reports the process high-water mark (VmHWM) beside it.
    pub snapshot_buffers_estimate: u64,
}

struct Inner {
    session: Session,
    view: LiveView,
    dirty: bool,
    /// The network side is done: the received bytes are final.
    net_done: bool,
    /// The parse of the final bytes is in `view.doc`.
    final_parsed: bool,
}

struct Shared {
    inner: Mutex<Inner>,
    wake: Condvar,
    cancel: AtomicBool,
}

/// Dropping a client requests a disconnect and returns at once: its threads
/// finish in the background (they own what they use). `join` waits.
pub struct LiveClient {
    shared: Arc<Shared>,
    net: Option<JoinHandle<()>>,
    worker: Option<JoinHandle<()>>,
}

impl LiveClient {
    pub fn start(cfg: ClientConfig) -> Result<LiveClient, String> {
        let addr = cfg.validate()?;
        let shared = Arc::new(Shared {
            inner: Mutex::new(Inner {
                session: Session::with_cap(cfg.cap),
                view: LiveView {
                    link: LinkState::Handshake,
                    hello: None,
                    received: 0,
                    pongs: 0,
                    doc: None,
                    generation: 0,
                    parses: 0,
                    local_error: None,
                    outcome: None,
                    snapshot_buffers_estimate: 0,
                },
                dirty: false,
                net_done: false,
                final_parsed: false,
            }),
            wake: Condvar::new(),
            cancel: AtomicBool::new(false),
        });
        let s = shared.clone();
        let c = cfg.clone();
        let net = std::thread::spawn(move || run_net(&s, &c, addr));
        let s = shared.clone();
        let every = cfg.parse_every;
        let worker = std::thread::spawn(move || run_worker(&s, every));
        Ok(LiveClient {
            shared,
            net: Some(net),
            worker: Some(worker),
        })
    }

    pub fn view(&self) -> LiveView {
        self.shared.inner.lock().unwrap().view.clone()
    }

    /// The first `len` received bytes (a snapshot's bytes: received bytes only grow), or None when fewer arrived.
    pub fn received_prefix(&self, len: u64) -> Option<Vec<u8>> {
        // The snapshot shares the sealed blocks (copies at most the tail); only the prefix is copied out.
        let snap = self.shared.inner.lock().unwrap().session.received.snapshot();
        snap.prefix_vec(len)
    }

    /// Asks the session to end; valid data that arrived is published as a capture that ended early.
    pub fn disconnect(&self) {
        self.shared.cancel.store(true, Ordering::SeqCst);
        self.shared.wake.notify_all();
    }

    /// True once the outcome is decided (which needs the final parse).
    pub fn finished(&self) -> bool {
        self.shared.inner.lock().unwrap().view.outcome.is_some()
    }

    /// Waits for both threads (tests and shutdown; never on a UI frame).
    pub fn join(mut self) -> LiveView {
        if let Some(h) = self.net.take() {
            let _ = h.join();
        }
        if let Some(h) = self.worker.take() {
            let _ = h.join();
        }
        self.view()
    }
}

impl Drop for LiveClient {
    fn drop(&mut self) {
        // Nonblocking: the threads see the request and finish on their own.
        self.disconnect();
    }
}

fn run_worker(sh: &Shared, every: Duration) {
    loop {
        let (snap, is_final) = {
            let mut g = sh.inner.lock().unwrap();
            while !g.dirty && !g.net_done {
                g = sh.wake.wait(g).unwrap();
            }
            if !g.dirty {
                return;
            }
            g.dirty = false;
            // Once the network is done the received bytes never change, so
            // this snapshot is the final one.
            (g.session.received.snapshot(), g.net_done)
        };
        // Copy and parse outside the lock.
        let tail = snap.len() % BLOCK_BYTES as u64;
        let bytes = snap.to_vec();
        let mut doc = Document::from_bytes(PathBuf::from("live capture"), &bytes);
        // A snapshot that is still arriving places its SPU zones PROVISIONALLY from the order records received so
        // far; the final snapshot (no byte can follow) keeps the final placement rules.
        doc.foreign_tl.spu.options.provisional = !is_final;
        let doc = Arc::new(doc);
        let working = snap.len() + tail + bytes.len() as u64;
        drop(bytes);
        {
            let mut g = sh.inner.lock().unwrap();
            g.view.snapshot_buffers_estimate = g.view.snapshot_buffers_estimate.max(working);
            g.view.doc = Some(doc);
            g.view.generation += 1;
            g.view.parses += 1;
            if is_final {
                g.final_parsed = true;
            }
        }
        if is_final {
            sh.wake.notify_all();
            return;
        }
        // Coalesce: whatever arrives during the pause is parsed once.
        std::thread::sleep(every);
    }
}

fn set_link(sh: &Shared, g: &mut Inner) {
    g.view.link = g.session.state.clone();
    g.view.hello = g.session.hello.clone();
    g.view.received = g.session.received.len();
    g.view.pongs = g.session.pongs;
    g.dirty = true;
    sh.wake.notify_all();
}

/// The local copy, created once at HELLO. A creation failure is latched
/// for the session: no later retry could hold the bytes already accepted.
enum Local {
    NotYet,
    Open {
        copy: LocalCopy,
        partial: PathBuf,
        stem: String,
    },
    Failed(String),
}

/// What the network side established.
struct NetResult {
    local: Local,
    /// Reasons that refuse complete publication beyond the session state.
    extra: Vec<String>,
    /// EOF after a valid END: the only clean end.
    clean_eof: bool,
    /// Set only by an end that may publish an accepted prefix: the user's Disconnect, EOF or a read error before
    /// END, no response in time, a failed PING.  Never by a protocol failure or a local failure.
    early: Option<String>,
}

fn run_net(sh: &Shared, cfg: &ClientConfig, addr: SocketAddr) {
    let mut r = NetResult {
        local: Local::NotYet,
        extra: Vec::new(),
        clean_eof: false,
        early: None,
    };
    let connected = TcpStream::connect_timeout(&addr, cfg.connect_timeout).and_then(|s| {
        s.set_nodelay(true)?;
        s.set_read_timeout(Some(READ_WAIT))?;
        s.set_write_timeout(Some(WRITE_TIMEOUT))?;
        Ok(s)
    });
    match connected {
        Ok(mut stream) => {
            stream_loop(sh, cfg, &mut stream, &mut r);
            let _ = stream.shutdown(Shutdown::Both);
        }
        Err(e) => r.extra.push(format!("could not connect to {addr}: {e}")),
    }
    finish(sh, cfg, r);
}

fn stream_loop(sh: &Shared, cfg: &ClientConfig, stream: &mut TcpStream, r: &mut NetResult) {
    if let Err(e) = stream.write_all(&client_hello()) {
        r.extra.push(format!("could not send the hello: {e}"));
        return;
    }
    let started = Instant::now();
    let mut last_rx = Instant::now();
    let mut last_ping = Instant::now();
    let mut ended_at: Option<Instant> = None;
    let mut buf = vec![0u8; 64 * 1024];
    loop {
        if sh.cancel.load(Ordering::SeqCst) {
            r.extra.push("disconnected by the user".into());
            r.early = Some("disconnected by the user".into());
            return;
        }
        let state = sh.inner.lock().unwrap().session.state.clone();
        match (&state, ended_at) {
            (LinkState::Handshake, _) if started.elapsed() > cfg.hello_timeout => {
                r.extra.push("the console sent no HELLO in time".into());
                return;
            }
            (_, Some(t)) if t.elapsed() > cfg.close_wait => {
                r.extra
                    .push("the console did not close the link after END".into());
                return;
            }
            (LinkState::Streaming, None) => {
                if last_rx.elapsed() > cfg.liveness_timeout {
                    r.extra.push("the console stopped responding".into());
                    r.early = Some("the console stopped responding".into());
                    return;
                }
                if last_ping.elapsed() >= cfg.ping_every {
                    last_ping = Instant::now();
                    if let Err(e) = stream.write_all(&ping()) {
                        r.extra.push(format!("could not send a PING: {e}"));
                        r.early = Some(format!("could not send a PING: {e}"));
                        return;
                    }
                }
            }
            _ => {}
        }
        match stream.read(&mut buf) {
            Ok(0) => {
                let mut g = sh.inner.lock().unwrap();
                r.clean_eof = matches!(g.session.state, LinkState::Ended(_));
                if matches!(g.session.state, LinkState::Handshake | LinkState::Streaming) {
                    r.early = Some("the connection closed before END".into());
                }
                g.session.closed(); // a failure before END
                set_link(sh, &mut g);
                return;
            }
            Ok(n) => {
                last_rx = Instant::now();
                let mut new = Vec::new();
                let (failed, ended, hello) = {
                    let mut g = sh.inner.lock().unwrap();
                    g.session
                        .feed_with(&buf[..n], &mut |d| new.extend_from_slice(d));
                    set_link(sh, &mut g);
                    (
                        g.session.failed(),
                        matches!(g.session.state, LinkState::Ended(_)),
                        g.session.hello.clone(),
                    )
                };
                if let (Local::NotYet, Some(h)) = (&r.local, hello) {
                    r.local = open_local(cfg, &h);
                    if let Local::Failed(e) = &r.local {
                        sh.inner.lock().unwrap().view.local_error = Some(e.clone());
                    }
                }
                if let Local::Open { copy, .. } = &mut r.local {
                    match EarlyFault::check(&cfg.early_fault, EarlyStep::LocalAppend) {
                        Ok(()) => copy.append(&new),
                        Err(e) => {
                            if copy.error.is_none() {
                                copy.error = Some(format!("local write failed: {e}"));
                            }
                        }
                    }
                    if let Some(e) = &copy.error {
                        sh.inner.lock().unwrap().view.local_error = Some(e.clone());
                    }
                }
                if failed {
                    return;
                }
                if ended && ended_at.is_none() {
                    ended_at = Some(Instant::now());
                }
            }
            Err(e) if matches!(e.kind(), ErrorKind::WouldBlock | ErrorKind::TimedOut) => {}
            Err(e) if e.kind() == ErrorKind::Interrupted => {}
            Err(e) => {
                // Any read error refuses publication, after END too.
                r.extra.push(format!("connection error: {e}"));
                let mut g = sh.inner.lock().unwrap();
                if matches!(g.session.state, LinkState::Handshake | LinkState::Streaming) {
                    r.early = Some(format!("connection error: {e}"));
                }
                g.session.closed();
                set_link(sh, &mut g);
                return;
            }
        }
    }
}

fn open_local(cfg: &ClientConfig, h: &Hello) -> Local {
    let stem = local_stem(h);
    let partial = cfg.out_dir.join(format!("{stem}.partial"));
    match FileSink::create(&partial) {
        Ok(sink) => Local::Open {
            copy: LocalCopy::new(Box::new(sink)),
            partial,
            stem,
        },
        Err(e) => Local::Failed(format!("could not create {}: {e}", partial.display())),
    }
}

/// An end that may publish an accepted prefix, by kind.
enum Early {
    /// Before END: the user's Disconnect, EOF or a read error before END, no response, a failed PING.
    BeforeEnd(String),
    /// END said no trailer was written and reported nothing else that refuses.
    NoTrailer(String),
}

fn finish(sh: &Shared, cfg: &ClientConfig, mut r: NetResult) {
    // The received bytes are final: hand them to the worker and wait for its
    // parse of exactly these bytes (the only parse; no copy here).
    let (state, received, reader_complete) = {
        let mut g = sh.inner.lock().unwrap();
        g.net_done = true;
        g.dirty = true;
        sh.wake.notify_all();
        while !g.final_parsed {
            g = sh.wake.wait(g).unwrap();
        }
        let complete = g
            .view
            .doc
            .as_ref()
            .is_some_and(|d| d.verdict().tone == VerdictTone::Complete);
        (g.session.state.clone(), g.session.received.len(), complete)
    };
    // An early end may publish an accepted prefix only when the link itself did not fail a check: still
    // streaming (the user's Disconnect, no response, a failed PING) or closed before END.  END stays authoritative
    // even when a Disconnect raced it.
    let before_end = r.early.take().filter(|_| {
        matches!(state, LinkState::Handshake | LinkState::Streaming | LinkState::Failed(LinkError::Closed))
    });
    if matches!(state, LinkState::Ended(_)) && !r.clean_eof && r.extra.is_empty() {
        r.extra
            .push("the link did not end with a clean close".into());
    }
    // An END that says no trailer was written is an ending without a trailer too, but only the missing trailer is
    // forgiven (`trailerless_end_refusals` names exactly what is): a join failure, any other shutdown result, an
    // accepted-byte mismatch, an unclean close or a local-copy failure still refuse, and are reported.
    let mut refused_trailerless: Vec<String> = Vec::new();
    let early = match (before_end, &state) {
        (Some(c), _) => Some(Early::BeforeEnd(c)),
        (None, LinkState::Ended(e)) if !e.trailer_written => {
            refused_trailerless = trailerless_end_refusals(e, received);
            refused_trailerless.is_empty().then(|| {
                Early::NoTrailer(format!(
                    "the console ended the capture without a trailer (app quit or the link ended); END forgiven: no trailer, I/O finalized {}, shutdown result {}",
                    u8::from(e.io_finalized),
                    e.shutdown_result
                ))
            })
        }
        _ => None,
    };
    let outcome = match r.local {
        Local::NotYet | Local::Failed(_) => {
            let mut reasons = r.extra;
            if let LinkState::Failed(e) = &state {
                reasons.push(e.to_string());
            }
            if let Local::Failed(e) = &r.local {
                reasons.push(e.clone());
            }
            reasons.push("nothing was saved".into());
            Outcome::Incomplete {
                saved: None,
                reasons,
            }
        }
        Local::Open {
            mut copy,
            partial,
            stem,
        } => {
            let local_ok = copy.finish();
            let mut extra = r.extra;
            if copy.written != received {
                extra.push(format!(
                    "the local copy holds {} of {received} bytes",
                    copy.written
                ));
            }
            if let Some(e) = &copy.error {
                sh.inner.lock().unwrap().view.local_error = Some(e.clone());
            }
            let whole = local_ok && copy.written == received && copy.error.is_none();
            let independent = !extra.is_empty() || !whole;
            let decision = match publish_decision(&state, received, reader_complete, local_ok) {
                Publish::Complete if extra.is_empty() => Publish::Complete,
                Publish::Complete => Publish::Incomplete(extra),
                Publish::Incomplete(mut why) => {
                    why.extend(extra);
                    if !refused_trailerless.is_empty() {
                        why.push(format!(
                            "no trailer was written, and END also reports what a missing trailer does not excuse: {}",
                            refused_trailerless.join("; ")
                        ));
                    }
                    Publish::Incomplete(why)
                }
            };
            let early = match (&decision, early) {
                // Before END the failure reasons are the early end itself; the local copy must still be whole.
                (Publish::Incomplete(_), Some(Early::BeforeEnd(cause))) if whole => Some(cause),
                // After END: no independent failure at all (clean close, no read error, a whole local copy).
                (Publish::Incomplete(_), Some(Early::NoTrailer(cause))) if !independent => Some(cause),
                _ => None,
            };
            let decision = match early {
                None => decision,
                Some(cause) => match publish_early(sh, cfg, &partial, &stem, cause) {
                    Ok(o) => {
                        let mut g = sh.inner.lock().unwrap();
                        g.view.outcome = Some(o);
                        sh.wake.notify_all();
                        return;
                    }
                    Err(why) => match decision {
                        Publish::Incomplete(mut w) => {
                            w.push(why);
                            Publish::Incomplete(w)
                        }
                        d => d,
                    },
                },
            };
            match decision {
                Publish::Complete => {
                    match publish_file(&partial, &cfg.out_dir, &stem, SUFFIX_COMPLETE) {
                        Ok(p) => Outcome::Published(p),
                        Err(e) => Outcome::Incomplete {
                            saved: None,
                            reasons: vec![format!(
                                "could not publish: {e}; the copy is at {}",
                                partial.display()
                            )],
                        },
                    }
                }
                Publish::Incomplete(mut reasons) => {
                    let saved = match publish_file(&partial, &cfg.out_dir, &stem, SUFFIX_INCOMPLETE)
                    {
                        Ok(p) => Some(p),
                        Err(e) => {
                            reasons.push(format!(
                                "could not keep it as incomplete: {e}; the copy is at {}",
                                partial.display()
                            ));
                            None
                        }
                    };
                    Outcome::Incomplete { saved, reasons }
                }
            }
        }
    };
    if matches!(outcome, Outcome::Incomplete { .. }) {
        strict_doc(sh);
    }
    let mut g = sh.inner.lock().unwrap();
    g.view.outcome = Some(outcome);
    sh.wake.notify_all();
}

/// A session that failed a check keeps the STRICT analysis on screen: its bytes are never shown as a capture that
/// ended early, whatever the reader would accept.
fn strict_doc(sh: &Shared) {
    let bytes = sh.inner.lock().unwrap().session.received.snapshot().to_vec();
    let doc = Arc::new(Document::strict(PathBuf::from("live capture"), &bytes));
    let mut g = sh.inner.lock().unwrap();
    g.view.doc = Some(doc);
    g.view.generation += 1;
}

/// Publishes the accepted prefix of a session that ended early: the final bytes must be an accepted prefix
/// (`ps3dk_prof::load_prefix`); a torn final chunk is cut from the closed local copy (checked set_len + sync)
/// before it is published as a `.prof`.  `Err` names why it stays incomplete.
fn publish_early(
    sh: &Shared,
    cfg: &ClientConfig,
    partial: &std::path::Path,
    stem: &str,
    cause: String,
) -> Result<Outcome, String> {
    let bytes = sh.inner.lock().unwrap().session.received.snapshot().to_vec();
    let a = ps3dk_prof::load_prefix(&bytes).map_err(|e| format!("not a capture that ended early: {e}"))?;
    let p = a.prefix.as_ref().expect("load_prefix sets it");
    let (cut, torn) = (p.cut, p.torn_dropped);
    if torn > 0 {
        cut_file(partial, cut as u64, &cfg.early_fault)?;
    }
    let published = EarlyFault::check(&cfg.early_fault, EarlyStep::Publish)
        .and_then(|_| publish_file(partial, &cfg.out_dir, stem, SUFFIX_COMPLETE))
        .map_err(|e| format!("could not publish ({:?}): {e}", EarlyStep::Publish))?;
    // The document on screen is the analysis of exactly the published bytes.
    let doc = Arc::new(Document::from_bytes(PathBuf::from("live capture"), &bytes[..cut]));
    {
        let mut g = sh.inner.lock().unwrap();
        g.view.doc = Some(doc);
        g.view.generation += 1;
    }
    Ok(Outcome::PublishedEarly { published, cause, cut: cut as u64, torn_dropped: torn as u64 })
}
