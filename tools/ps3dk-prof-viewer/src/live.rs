//! Live link, viewer side: the frame decoder, the session state machine and
//! the rules for publishing the received capture. No sockets, threads or
//! egui types, so all of it is tested with recorded byte streams.
//!
//! The wire schema is `sdk/libps3dkprof/src/live/live_proto.h`; the
//! constants below mirror it and a test checks them against that header.

use std::fmt;
use std::sync::Arc;

pub const HDR_BYTES: usize = 8;
pub const MAX_PAYLOAD: u32 = 65536;
pub const T_CLIENT_HELLO: u16 = 1;
pub const MAGIC: u32 = 0x564c_3350;
/// The oldest protocol version an agent serves (and the one a v1 agent speaks).
pub const VERSION_MIN: u16 = 1;
/// The newest protocol version this viewer asks for: capture triggers. Agents serve version 3 (the RSX frame request,
/// the constants below); asking for it needs a 3 -> 2 -> 1 fallback, since a version 2 agent closes a version 3 hello.
pub const VERSION: u16 = 2;
pub const T_HELLO: u16 = 2;
/// HELLO capability bit (version 2): the agent applies capture commands.
pub const CAP_TRIGGERS: u32 = 1;
/// HELLO capability bit (version 3): the agent serves RSX_FRAME_REQ (sdk/libps3dkprof/src/live/live_proto.h).
pub const CAP_RSX_FRAME: u32 = 2;
pub const T_RSX_FRAME_REQ: u16 = 12;
pub const T_RSX_FRAME_ACK: u16 = 13;
pub const T_RSX_FRAME_STATE: u16 = 14;
pub const RSXF_ACK_BYTES: u32 = 16;
pub const RSXF_STATE_BYTES: u32 = 32;
pub const MAX_APP: usize = 32;
pub const MAX_BUILD_ID: usize = 512;
pub const MAX_NAME: usize = 255;
pub const T_DATA: u16 = 3;
pub const MAX_DATA: u32 = MAX_PAYLOAD - 8;
pub const T_END: u16 = 4;
pub const END_BYTES: u32 = 24;
pub const T_PING: u16 = 5;
pub const T_PONG: u16 = 6;
pub const T_BUSY: u16 = 7;
pub const T_CAPTURE_BEGIN: u16 = 8;
pub const T_CAPTURE_END: u16 = 9;
pub const T_CAPTURE_FRAMES: u16 = 10;
pub const T_CAPTURE_ACK: u16 = 11;
pub const MAX_WINDOW_NAME: usize = 63;
pub const MAX_WINDOW_FRAMES: u32 = 1_000_000;
pub const ACK_BYTES: u32 = 16;

/// Bytes accumulated per session before the link is closed and labelled.
pub const MAX_SESSION_BYTES: u64 = 256 << 20;

/// The CLIENT_HELLO frame a viewer sends first, asking for the newest version.
pub fn client_hello() -> Vec<u8> {
    client_hello_version(VERSION)
}

/// A CLIENT_HELLO asking for `version` (a v2 viewer asks for 1 only after a v1 agent closed its v2 hello).
pub fn client_hello_version(version: u16) -> Vec<u8> {
    let mut f = frame_header(T_CLIENT_HELLO, 8);
    f.extend_from_slice(&MAGIC.to_le_bytes());
    f.extend_from_slice(&version.to_le_bytes());
    f.extend_from_slice(&0u16.to_le_bytes());
    f
}

/// A capture command (protocol version 2): what the viewer asks the running app to do with its capture window.
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum Command {
    /// A MANUAL window named `name`, from the next frame mark until End.
    Begin { name: String },
    /// Ends the window this viewer opened.
    End,
    /// A FRAMES window of `frames` frames named `name`; it closes itself (reason COUNT).
    Frames { name: String, frames: u32 },
}

impl Command {
    pub fn frame_type(&self) -> u16 {
        match self {
            Command::Begin { .. } => T_CAPTURE_BEGIN,
            Command::End => T_CAPTURE_END,
            Command::Frames { .. } => T_CAPTURE_FRAMES,
        }
    }

    pub fn label(&self) -> String {
        match self {
            Command::Begin { name } => format!("Begin '{name}'"),
            Command::End => "End".to_string(),
            Command::Frames { name, frames } => format!("Capture {frames} frames '{name}'"),
        }
    }

    /// The bounds the agent enforces (it closes the link on a violation), checked before anything is sent.
    pub fn check(&self) -> Result<(), String> {
        let name = match self {
            Command::End => return Ok(()),
            Command::Begin { name } => name,
            Command::Frames { name, frames } => {
                if *frames < 1 || *frames > MAX_WINDOW_FRAMES {
                    return Err(format!("the frame count must be 1..{MAX_WINDOW_FRAMES}"));
                }
                name
            }
        };
        if name.is_empty() || name.len() > MAX_WINDOW_NAME {
            return Err(format!("the window name must be 1..{MAX_WINDOW_NAME} bytes"));
        }
        if name.contains('\0') {
            return Err("the window name must not contain NUL".into());
        }
        Ok(())
    }

    /// The command frame with request id `request` (never 0).
    pub fn encode(&self, request: u32) -> Result<Vec<u8>, String> {
        self.check()?;
        if request == 0 {
            return Err("request id 0".into());
        }
        let mut p = request.to_le_bytes().to_vec();
        match self {
            Command::End => {}
            Command::Begin { name } => {
                p.extend_from_slice(&(name.len() as u16).to_le_bytes());
                p.extend_from_slice(&0u16.to_le_bytes());
                p.extend_from_slice(name.as_bytes());
            }
            Command::Frames { name, frames } => {
                p.extend_from_slice(&frames.to_le_bytes());
                p.extend_from_slice(&(name.len() as u16).to_le_bytes());
                p.extend_from_slice(&0u16.to_le_bytes());
                p.extend_from_slice(name.as_bytes());
            }
        }
        let mut f = frame_header(self.frame_type(), p.len() as u32);
        f.extend_from_slice(&p);
        Ok(f)
    }
}

/// A CAPTURE_ACK: the answer to one command.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct Ack {
    pub request: u32,
    /// 0, or the `PS3DK_PROF_*` refusal (prof.h).
    pub result: i32,
    /// The window opened or closed; 0 when refused.
    pub window_id: u32,
    pub command: u16,
}

/// What a `PS3DK_PROF_*` result means for a capture command (prof.h codes; the agent and runtime choose which).
pub fn result_text(code: i32) -> String {
    match code {
        0 => "done".into(),
        -1 => "EPERM: the open window was opened by the app, not by this viewer".into(),
        -2 => "EBUSY: a capture window is already open or about to open (one at a time)".into(),
        -5 => "EINVAL: no window to end, or the name is not valid UTF-8".into(),
        -12 => "ETHREAD: the frame thread is not registered with the profiler".into(),
        -14 => "ENOTINIT: the profiler is not initialized".into(),
        -15 => "ESTOPPED: the capture is shutting down".into(),
        -16 => "EFULL: no room (the drainer is behind, or this run's table of viewer window names is full)".into(),
        -17 => "ENOTSUP: this agent does not apply capture triggers".into(),
        -19 => "ETIMEDOUT: no frame was marked within the agent's deadline; nothing changed".into(),
        c => format!("refused with code {c}"),
    }
}

/// A PING frame.
pub fn ping() -> Vec<u8> {
    frame_header(T_PING, 0)
}

fn frame_header(t: u16, len: u32) -> Vec<u8> {
    let mut f = Vec::with_capacity(HDR_BYTES + len as usize);
    f.extend_from_slice(&t.to_le_bytes());
    f.extend_from_slice(&0u16.to_le_bytes());
    f.extend_from_slice(&len.to_le_bytes());
    f
}

/// The agent's identity, from HELLO. For display; there is no resume.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Hello {
    /// The protocol version of this session (the one CLIENT_HELLO asked for).
    pub protocol: u16,
    /// Version 2: LIVE_CAP_* bits (0 in version 1).
    pub capabilities: u32,
    pub agent_version: u16,
    pub session_id: u64,
    pub app: String,
    pub build_id: Vec<u8>,
    pub file_name: String,
}

/// END, as the agent copied it from the runtime's finalization record.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct End {
    pub io_finalized: bool,
    pub trailer_written: bool,
    pub join_failed: bool,
    pub shutdown_result: i32,
    pub accepted_bytes: u64,
    pub sent_bytes: u64,
}

/// Why a link failed. Every failure is reported locally, never only over
/// the (broken) link.
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum LinkError {
    Busy(u32),
    BadFrame(String),
    Order(String),
    Gap { expected: u64, got: u64 },
    TooLarge,
    AfterEnd,
    EndMismatch { received: u64, sent: u64 },
    Closed,
    Io(String),
}

impl fmt::Display for LinkError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            LinkError::Busy(r) => write!(f, "the console is serving another viewer (BUSY {r})"),
            LinkError::BadFrame(s) => write!(f, "malformed frame: {s}"),
            LinkError::Order(s) => write!(f, "frame out of order: {s}"),
            LinkError::Gap { expected, got } => {
                write!(f, "DATA at offset {got}, expected {expected}")
            }
            LinkError::TooLarge => write!(f, "the capture exceeded the session limit"),
            LinkError::AfterEnd => write!(f, "a frame arrived after END"),
            LinkError::EndMismatch { received, sent } => {
                write!(f, "END says {sent} bytes were sent, {received} arrived")
            }
            LinkError::Closed => write!(f, "the connection closed before END"),
            LinkError::Io(s) => write!(f, "{s}"),
        }
    }
}

/// The link's state, kept apart from the capture verdict and lane validity.
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum LinkState {
    Handshake,
    Streaming,
    /// END arrived and its sent size matched what was received: transport
    /// completion only.
    Ended(End),
    Failed(LinkError),
}

/// Received bytes are stored in sealed blocks of this size, however small
/// the DATA frames were, so storage metadata is bounded by bytes, not frames.
pub const BLOCK_BYTES: usize = 1 << 20;

/// The received bytes: sealed immutable blocks of `BLOCK_BYTES` plus one
/// open tail block. Appending never copies sealed bytes; a snapshot shares
/// the sealed blocks and copies at most one block (the tail).
#[derive(Debug, Clone, Default)]
pub struct Received {
    sealed: Vec<Arc<[u8]>>,
    tail: Vec<u8>,
    len: u64,
}

impl Received {
    pub fn len(&self) -> u64 {
        self.len
    }
    pub fn is_empty(&self) -> bool {
        self.len == 0
    }
    /// Sealed blocks plus the tail (if any): the storage metadata count.
    pub fn block_count(&self) -> usize {
        self.sealed.len() + usize::from(!self.tail.is_empty())
    }
    /// An immutable snapshot: shares the sealed blocks, copies the tail.
    pub fn snapshot(&self) -> Received {
        self.clone()
    }
    /// The bytes as one buffer, for the reader. One copy per call; the
    /// parse worker calls it at most once per coalesced parse.
    pub fn to_vec(&self) -> Vec<u8> {
        let mut v = Vec::with_capacity(self.len as usize);
        for s in &self.sealed {
            v.extend_from_slice(s);
        }
        v.extend_from_slice(&self.tail);
        v
    }
    /// The first `n` bytes as one buffer (copies only those), or None when fewer were received.
    pub fn prefix_vec(&self, n: u64) -> Option<Vec<u8>> {
        if n > self.len {
            return None;
        }
        let mut v = Vec::with_capacity(n as usize);
        for s in self.sealed.iter().map(|s| &s[..]).chain(std::iter::once(&self.tail[..])) {
            let want = n as usize - v.len();
            if want == 0 {
                break;
            }
            v.extend_from_slice(&s[..want.min(s.len())]);
        }
        Some(v)
    }
    fn push(&mut self, mut bytes: &[u8]) {
        self.len += bytes.len() as u64;
        while !bytes.is_empty() {
            if self.tail.capacity() == 0 {
                self.tail.reserve_exact(BLOCK_BYTES);
            }
            let take = (BLOCK_BYTES - self.tail.len()).min(bytes.len());
            self.tail.extend_from_slice(&bytes[..take]);
            bytes = &bytes[take..];
            if self.tail.len() == BLOCK_BYTES {
                let full = std::mem::take(&mut self.tail);
                self.sealed.push(Arc::from(full));
            }
        }
    }
}

/// Decodes frames from arbitrary chunks and enforces the protocol.
#[derive(Debug, Clone)]
pub struct Session {
    pending: Vec<u8>,
    pub state: LinkState,
    pub hello: Option<Hello>,
    pub received: Received,
    pub pongs: u64,
    cap: u64,
    /// The version CLIENT_HELLO asked for: HELLO must name it.
    asked: u16,
    /// The one command sent and not yet answered: (request id, command type).
    outstanding: Option<(u32, u16)>,
    /// ACKs received and not yet taken (`take_acks`); at most one per command sent.
    acks: Vec<Ack>,
}

impl Default for Session {
    fn default() -> Self {
        Session::new()
    }
}

impl Session {
    pub fn new() -> Session {
        Session::with_cap(MAX_SESSION_BYTES)
    }

    /// A session that accepts at most `cap` capture bytes.
    pub fn with_cap(cap: u64) -> Session {
        Session::asking(cap, VERSION)
    }

    /// A session whose CLIENT_HELLO asked for `version`.
    pub fn asking(cap: u64, version: u16) -> Session {
        Session {
            pending: Vec::new(),
            state: LinkState::Handshake,
            hello: None,
            received: Received::default(),
            pongs: 0,
            cap,
            asked: version,
            outstanding: None,
            acks: Vec::new(),
        }
    }

    /// The version CLIENT_HELLO asked for.
    pub fn asked(&self) -> u16 {
        self.asked
    }

    /// The session's protocol version once HELLO arrived.
    pub fn protocol(&self) -> Option<u16> {
        self.hello.as_ref().map(|h| h.protocol)
    }

    /// Records that a command frame is about to be sent; refused (nothing may be sent) unless the session is a
    /// streaming version-2 session whose agent applies triggers and no command is outstanding.
    pub fn expect_ack(&mut self, request: u32, command: u16) -> Result<(), String> {
        if !matches!(self.state, LinkState::Streaming) {
            return Err("the link is not streaming".into());
        }
        match &self.hello {
            Some(h) if h.protocol >= 2 && h.capabilities & CAP_TRIGGERS != 0 => {}
            Some(h) if h.protocol < 2 => return Err("the agent speaks live protocol 1 (no capture triggers)".into()),
            _ => return Err("the agent does not apply capture triggers".into()),
        }
        if self.outstanding.is_some() {
            return Err("a command is still waiting for its answer".into());
        }
        if request == 0 {
            return Err("request id 0".into());
        }
        self.outstanding = Some((request, command));
        Ok(())
    }

    /// The command sent and not yet answered.
    pub fn outstanding(&self) -> Option<(u32, u16)> {
        self.outstanding
    }

    /// Takes the ACKs received so far.
    pub fn take_acks(&mut self) -> Vec<Ack> {
        std::mem::take(&mut self.acks)
    }

    pub fn failed(&self) -> bool {
        matches!(self.state, LinkState::Failed(_))
    }

    /// Feeds bytes as they arrive; any split is fine. Returns false once the
    /// link has failed (further bytes are ignored).
    pub fn feed(&mut self, bytes: &[u8]) -> bool {
        self.feed_with(bytes, &mut |_| {})
    }

    /// Like `feed`, and calls `on_data` with each DATA payload the session
    /// accepted, in order (what a local copy must append).
    pub fn feed_with(&mut self, mut bytes: &[u8], on_data: &mut dyn FnMut(&[u8])) -> bool {
        while !self.failed() {
            // After END the stream is over: any further byte is an error,
            // even one that would only start a frame.
            if matches!(self.state, LinkState::Ended(_)) {
                if !bytes.is_empty() || !self.pending.is_empty() {
                    self.state = LinkState::Failed(LinkError::AfterEnd);
                    return false;
                }
                return true;
            }
            // Complete the header, then the payload, using `pending`.
            let need = if self.pending.len() < HDR_BYTES {
                HDR_BYTES
            } else {
                let len = u32::from_le_bytes(self.pending[4..8].try_into().unwrap());
                HDR_BYTES + len as usize
            };
            if self.pending.len() < need {
                if bytes.is_empty() {
                    return true;
                }
                let take = (need - self.pending.len()).min(bytes.len());
                self.pending.extend_from_slice(&bytes[..take]);
                bytes = &bytes[take..];
                if self.pending.len() == HDR_BYTES {
                    if let Err(e) = check_header(&self.pending) {
                        self.state = LinkState::Failed(e);
                    }
                }
                continue;
            }
            let frame = std::mem::take(&mut self.pending);
            let t = u16::from_le_bytes([frame[0], frame[1]]);
            if let Err(e) = self.on_frame(t, &frame[HDR_BYTES..], on_data) {
                self.state = LinkState::Failed(e);
            }
        }
        false
    }

    /// The connection closed: fine after END, a failure before it.
    pub fn closed(&mut self) {
        if !matches!(self.state, LinkState::Ended(_) | LinkState::Failed(_)) {
            self.state = LinkState::Failed(LinkError::Closed);
        }
    }

    fn on_frame(
        &mut self,
        t: u16,
        p: &[u8],
        on_data: &mut dyn FnMut(&[u8]),
    ) -> Result<(), LinkError> {
        if matches!(self.state, LinkState::Ended(_)) {
            return Err(LinkError::AfterEnd);
        }
        match (t, &self.state) {
            (T_BUSY, LinkState::Handshake) => {
                if p.len() != 4 {
                    return Err(LinkError::BadFrame("BUSY length".into()));
                }
                Err(LinkError::Busy(u32::from_le_bytes(p.try_into().unwrap())))
            }
            (T_HELLO, LinkState::Handshake) => {
                self.hello = Some(parse_hello(p, self.asked)?);
                self.state = LinkState::Streaming;
                Ok(())
            }
            (_, LinkState::Handshake) => Err(LinkError::Order(format!("type {t} before HELLO"))),
            (T_DATA, LinkState::Streaming) => {
                if p.len() <= 8 {
                    return Err(LinkError::BadFrame("empty DATA".into()));
                }
                let off = u64::from_le_bytes(p[..8].try_into().unwrap());
                let n = (p.len() - 8) as u64;
                let expected = self.received.len();
                if off != expected {
                    return Err(LinkError::Gap { expected, got: off });
                }
                let end = off.checked_add(n).ok_or(LinkError::TooLarge)?;
                if end > self.cap {
                    return Err(LinkError::TooLarge);
                }
                self.received.push(&p[8..]);
                on_data(&p[8..]);
                Ok(())
            }
            (T_END, LinkState::Streaming) => {
                if p.len() != END_BYTES as usize || p[3] != 0 || p[0] > 1 || p[1] > 1 || p[2] > 1 {
                    return Err(LinkError::BadFrame("END layout".into()));
                }
                let end = End {
                    io_finalized: p[0] == 1,
                    trailer_written: p[1] == 1,
                    join_failed: p[2] == 1,
                    shutdown_result: i32::from_le_bytes(p[4..8].try_into().unwrap()),
                    accepted_bytes: u64::from_le_bytes(p[8..16].try_into().unwrap()),
                    sent_bytes: u64::from_le_bytes(p[16..24].try_into().unwrap()),
                };
                if end.sent_bytes != self.received.len() {
                    return Err(LinkError::EndMismatch {
                        received: self.received.len(),
                        sent: end.sent_bytes,
                    });
                }
                self.state = LinkState::Ended(end);
                Ok(())
            }
            (T_PONG, LinkState::Streaming) => {
                if !p.is_empty() {
                    return Err(LinkError::BadFrame("PONG length".into()));
                }
                self.pongs += 1;
                Ok(())
            }
            (T_CAPTURE_ACK, LinkState::Streaming) if self.protocol().is_some_and(|v| v >= 2) => {
                if p.len() != ACK_BYTES as usize || p[14..16] != [0, 0] {
                    return Err(LinkError::BadFrame("ACK layout".into()));
                }
                let ack = Ack {
                    request: u32::from_le_bytes(p[0..4].try_into().unwrap()),
                    result: i32::from_le_bytes(p[4..8].try_into().unwrap()),
                    window_id: u32::from_le_bytes(p[8..12].try_into().unwrap()),
                    command: u16::from_le_bytes([p[12], p[13]]),
                };
                if self.outstanding != Some((ack.request, ack.command)) {
                    return Err(LinkError::BadFrame(format!(
                        "ACK for request {} (type {}) that is not outstanding",
                        ack.request, ack.command
                    )));
                }
                if ack.result != 0 && ack.window_id != 0 {
                    return Err(LinkError::BadFrame("a refusal ACK names a window".into()));
                }
                self.outstanding = None;
                self.acks.push(ack);
                Ok(())
            }
            (t, _) => Err(LinkError::BadFrame(format!("unexpected type {t}"))),
        }
    }
}

fn check_header(h: &[u8]) -> Result<(), LinkError> {
    let flags = u16::from_le_bytes([h[2], h[3]]);
    let len = u32::from_le_bytes(h[4..8].try_into().unwrap());
    if flags != 0 {
        return Err(LinkError::BadFrame(format!("flags {flags:#x}")));
    }
    if len > MAX_PAYLOAD {
        return Err(LinkError::BadFrame(format!("length {len}")));
    }
    Ok(())
}

fn parse_hello(p: &[u8], asked: u16) -> Result<Hello, LinkError> {
    let bad = |s: &str| LinkError::BadFrame(format!("HELLO {s}"));
    if p.len() < 16 {
        return Err(bad("too short"));
    }
    let version = u16::from_le_bytes([p[0], p[1]]);
    // The agent answers in the version asked for; a lower one is accepted (a version this viewer also speaks).
    if !(VERSION_MIN..=asked).contains(&version) {
        return Err(bad(&format!("protocol version {version}")));
    }
    let agent_version = u16::from_le_bytes([p[2], p[3]]);
    let capabilities = u32::from_le_bytes(p[4..8].try_into().unwrap());
    if version == 1 && capabilities != 0 {
        return Err(bad("reserved field is not 0"));
    }
    let session_id = u64::from_le_bytes(p[8..16].try_into().unwrap());
    let mut rest = &p[16..];
    let mut string = |max: usize, min: usize| -> Result<Vec<u8>, LinkError> {
        if rest.len() < 2 {
            return Err(bad("truncated"));
        }
        let n = u16::from_le_bytes([rest[0], rest[1]]) as usize;
        if n < min || n > max || rest.len() < 2 + n {
            return Err(bad("string length"));
        }
        let s = rest[2..2 + n].to_vec();
        rest = &rest[2 + n..];
        Ok(s)
    };
    let app = string(MAX_APP, 1)?;
    let build_id = string(MAX_BUILD_ID, 0)?;
    let name = string(MAX_NAME, 1)?;
    if !rest.is_empty() {
        return Err(bad("trailing bytes"));
    }
    if name.contains(&b'/') {
        return Err(bad("file name with '/'"));
    }
    Ok(Hello {
        protocol: version,
        capabilities,
        agent_version,
        session_id,
        app: String::from_utf8_lossy(&app).into_owned(),
        build_id,
        file_name: String::from_utf8_lossy(&name).into_owned(),
    })
}

/// What happens to the local copy when the session ends.
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum Publish {
    /// Every check passed: published as a `.prof`.
    Complete,
    /// Kept as `.incomplete.prof`, with the reasons.
    Incomplete(Vec<String>),
}

/// Decides publication. `reader_complete` is the production reader's
/// verdict on exactly the received bytes; `local_ok` is whether the local
/// copy was written, flushed and closed without error.
pub fn publish_decision(
    state: &LinkState,
    received: u64,
    reader_complete: bool,
    local_ok: bool,
) -> Publish {
    let mut why = Vec::new();
    match state {
        LinkState::Ended(e) => {
            if !e.io_finalized {
                why.push("the console's capture I/O did not finalize".to_string());
            }
            if !e.trailer_written {
                why.push("no trailer was written".to_string());
            }
            if e.join_failed {
                why.push("the profiler's drain thread did not join".to_string());
            }
            if e.shutdown_result != 0 {
                why.push(format!("shutdown result {}", e.shutdown_result));
            }
            if e.io_finalized && e.accepted_bytes != received {
                why.push(format!(
                    "the console wrote {} bytes, {received} arrived",
                    e.accepted_bytes
                ));
            }
        }
        LinkState::Failed(err) => why.push(err.to_string()),
        _ => why.push("the link has not ended".to_string()),
    }
    if !reader_complete {
        why.push("the reader does not find the capture COMPLETE".to_string());
    }
    if !local_ok {
        why.push("the local copy could not be written".to_string());
    }
    if why.is_empty() {
        Publish::Complete
    } else {
        Publish::Incomplete(why)
    }
}

/// `PS3DK_PROF_ESINKFAIL` (prof.h): the runtime's shutdown result whenever it wrote no trailer.
pub const SHUTDOWN_NO_TRAILER: i32 = -10;

/// The END shutdown results that an END without a trailer may carry and still publish its accepted prefix:
/// exactly the runtime's no-trailer result.  Every other result (0 beside a missing trailer is inconsistent;
/// `PS3DK_PROF_ETHREAD` is a join failure; any other code is a failure of its own) refuses.
pub const NO_TRAILER_FORGIVEN_RESULTS: [i32; 1] = [SHUTDOWN_NO_TRAILER];

/// For an END that says no trailer was written: the reasons the missing trailer does NOT excuse.  Only three END
/// facts are forgiven, all consequences of the missing trailer: `trailer_written` 0, `io_finalized` 0 (the runtime
/// sets it only with a trailer) and a shutdown result in [`NO_TRAILER_FORGIVEN_RESULTS`].  A join failure, any
/// other shutdown result and, when the I/O did finalize, an accepted byte count that is not the bytes received
/// still refuse.  Empty for an END with a trailer (nothing is forgiven there).
pub fn trailerless_end_refusals(e: &End, received: u64) -> Vec<String> {
    let mut why = Vec::new();
    if e.trailer_written {
        return why;
    }
    if e.join_failed {
        why.push("the profiler's drain thread did not join".to_string());
    }
    if !NO_TRAILER_FORGIVEN_RESULTS.contains(&e.shutdown_result) {
        why.push(format!(
            "shutdown result {} is not the runtime's no-trailer result {SHUTDOWN_NO_TRAILER}",
            e.shutdown_result
        ));
    }
    if e.io_finalized && e.accepted_bytes != received {
        why.push(format!("the console wrote {} bytes, {received} arrived", e.accepted_bytes));
    }
    why
}

/// A local file name stem for a session: the session id, then the agent's
/// file name without `.prof`, reduced to `[A-Za-z0-9_-]` (dots too become
/// `_`) and at most 100 characters. Never empty, never a path.
pub fn local_stem(hello: &Hello) -> String {
    let base = hello
        .file_name
        .strip_suffix(".prof")
        .unwrap_or(&hello.file_name);
    let mut name: String = base
        .chars()
        .map(|c| {
            if c.is_ascii_alphanumeric() || c == '-' || c == '_' {
                c
            } else {
                '_'
            }
        })
        .take(100)
        .collect();
    if name.is_empty() {
        name = "capture".into();
    }
    format!("{:016x}-{name}", hello.session_id)
}

/// Where the received bytes go on disk, one step per call so tests can
/// fail each step. `close` is called exactly once, also after an error.
pub trait LocalSink: Send {
    fn write_all(&mut self, bytes: &[u8]) -> std::io::Result<()>;
    fn flush(&mut self) -> std::io::Result<()>;
    fn sync(&mut self) -> std::io::Result<()>;
    /// A checked close. A platform that cannot report close errors returns
    /// an error, so the copy is never certified on an unchecked close.
    fn close(&mut self) -> std::io::Result<()>;
}

/// The real sink: `<dir>/<stem>.partial`, created exclusively.
pub struct FileSink {
    file: Option<std::fs::File>,
}

impl FileSink {
    pub fn create(partial: &std::path::Path) -> std::io::Result<FileSink> {
        let file = std::fs::OpenOptions::new()
            .write(true)
            .create_new(true)
            .open(partial)?;
        Ok(FileSink { file: Some(file) })
    }
}

fn closed_err() -> std::io::Error {
    std::io::Error::other("the local file is already closed")
}

impl LocalSink for FileSink {
    fn write_all(&mut self, bytes: &[u8]) -> std::io::Result<()> {
        use std::io::Write;
        self.file.as_mut().ok_or_else(closed_err)?.write_all(bytes)
    }
    fn flush(&mut self) -> std::io::Result<()> {
        use std::io::Write;
        self.file.as_mut().ok_or_else(closed_err)?.flush()
    }
    fn sync(&mut self) -> std::io::Result<()> {
        self.file.as_mut().ok_or_else(closed_err)?.sync_all()
    }
    fn close(&mut self) -> std::io::Result<()> {
        let file = self.file.take().ok_or_else(closed_err)?;
        checked_close(file)
    }
}

/// Closes a file and reports the OS result (std's `Drop` discards it). The
/// handle is released exactly once; a failed close is not retried.
#[cfg(unix)]
pub(crate) fn checked_close(file: std::fs::File) -> std::io::Result<()> {
    use std::os::fd::IntoRawFd;
    extern "C" {
        fn close(fd: i32) -> i32;
    }
    let fd = file.into_raw_fd();
    // SAFETY: `fd` came from `into_raw_fd`, so this is its only owner.
    if unsafe { close(fd) } == 0 {
        Ok(())
    } else {
        Err(std::io::Error::last_os_error())
    }
}

#[cfg(windows)]
pub(crate) fn checked_close(file: std::fs::File) -> std::io::Result<()> {
    use std::os::windows::io::IntoRawHandle;
    #[link(name = "kernel32")]
    extern "system" {
        fn CloseHandle(h: *mut std::ffi::c_void) -> i32;
    }
    let h = file.into_raw_handle();
    // SAFETY: `h` came from `into_raw_handle`, so this is its only owner.
    if unsafe { CloseHandle(h as *mut std::ffi::c_void) } != 0 {
        Ok(())
    } else {
        Err(std::io::Error::last_os_error())
    }
}

#[cfg(not(any(unix, windows)))]
pub(crate) fn checked_close(file: std::fs::File) -> std::io::Result<()> {
    drop(file);
    Err(std::io::Error::new(
        std::io::ErrorKind::Unsupported,
        "close errors cannot be checked on this platform",
    ))
}

/// The local copy of one session. After the first error it stops writing;
/// the in-memory bytes are kept and the first error is shown.
pub struct LocalCopy {
    sink: Box<dyn LocalSink>,
    pub written: u64,
    pub error: Option<String>,
    closed: bool,
}

impl LocalCopy {
    pub fn new(sink: Box<dyn LocalSink>) -> LocalCopy {
        LocalCopy {
            sink,
            written: 0,
            error: None,
            closed: false,
        }
    }
    pub fn append(&mut self, bytes: &[u8]) {
        if self.error.is_some() || self.closed {
            return;
        }
        match self.sink.write_all(bytes) {
            Ok(()) => self.written += bytes.len() as u64,
            Err(e) => self.error = Some(format!("local write failed: {e}")),
        }
    }
    /// Flushes and syncs (unless a write already failed), then closes,
    /// always and exactly once. True only if every step succeeded. Calling
    /// it again changes nothing and never erases an earlier failure.
    pub fn finish(&mut self) -> bool {
        if !self.closed {
            self.closed = true;
            if self.error.is_none() {
                if let Err(e) = self.sink.flush() {
                    self.error = Some(format!("local flush failed: {e}"));
                } else if let Err(e) = self.sink.sync() {
                    self.error = Some(format!("local sync failed: {e}"));
                }
            }
            if let Err(e) = self.sink.close() {
                if self.error.is_none() {
                    self.error = Some(format!("local close failed: {e}"));
                }
            }
        }
        self.error.is_none()
    }
}

/// A published local file.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Published {
    pub path: std::path::PathBuf,
    /// The file was published, but the `.partial` could not be removed.
    pub cleanup_warning: Option<String>,
}

/// A step of publishing a session that ended early, in order: the torn tail is cut from the closed local copy
/// (open, set_len, sync, close), then the copy is published.  Each is a fault-injection point for tests.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum EarlyStep {
    Open,
    SetLen,
    Sync,
    Close,
    Publish,
    /// Not an early-publish step: checked before each append to the local copy while streaming; an error is
    /// that append's write failure.
    LocalAppend,
}

/// A test fault: called before each [`EarlyStep`]; an error it returns is that step's failure.  Production configs
/// carry none.
#[derive(Clone)]
pub struct EarlyFault(pub std::sync::Arc<dyn Fn(EarlyStep) -> Option<std::io::Error> + Send + Sync>);

impl fmt::Debug for EarlyFault {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.write_str("EarlyFault")
    }
}

impl EarlyFault {
    pub fn check(f: &Option<EarlyFault>, s: EarlyStep) -> std::io::Result<()> {
        match f.as_ref().and_then(|h| (h.0)(s)) {
            Some(e) => Err(e),
            None => Ok(()),
        }
    }
}

/// Cuts `path` to `len` bytes with every step checked, the close included (the same checked close as the local
/// copy).  The handle is released exactly once whatever fails.  `Err` names the failed step.
pub fn cut_file(path: &std::path::Path, len: u64, fault: &Option<EarlyFault>) -> Result<(), String> {
    let at = |s: EarlyStep, e: std::io::Error| format!("could not cut the torn tail of {} ({s:?}): {e}", path.display());
    let file = EarlyFault::check(fault, EarlyStep::Open)
        .and_then(|_| std::fs::OpenOptions::new().write(true).open(path))
        .map_err(|e| at(EarlyStep::Open, e))?;
    let body = EarlyFault::check(fault, EarlyStep::SetLen)
        .and_then(|_| file.set_len(len))
        .map_err(|e| at(EarlyStep::SetLen, e))
        .and_then(|_| {
            EarlyFault::check(fault, EarlyStep::Sync)
                .and_then(|_| file.sync_all())
                .map_err(|e| at(EarlyStep::Sync, e))
        });
    let close = checked_close(file);
    body?;
    EarlyFault::check(fault, EarlyStep::Close)
        .and(close)
        .map_err(|e| at(EarlyStep::Close, e))
}

/// The suffixes a session may publish under.
pub const SUFFIX_COMPLETE: &str = ".prof";
pub const SUFFIX_INCOMPLETE: &str = ".incomplete.prof";

/// Publishes `partial` as the first free `<stem><suffix>`, `<stem>-1<suffix>`,
/// ... in `dir`, never replacing an existing file: a hard link fails if the
/// target exists. Only then is `partial` removed; if that fails the result is
/// still a success, with a warning. If the file system cannot hard-link,
/// nothing is published and `partial` stays where it is. `stem` must be one
/// `local_stem` made and `suffix` one of the two above.
pub fn publish_file(
    partial: &std::path::Path,
    dir: &std::path::Path,
    stem: &str,
    suffix: &str,
) -> std::io::Result<Published> {
    let stem_ok = !stem.is_empty()
        && stem.len() <= 17 + 100
        && stem
            .chars()
            .all(|c| c.is_ascii_alphanumeric() || c == '-' || c == '_');
    if !stem_ok || (suffix != SUFFIX_COMPLETE && suffix != SUFFIX_INCOMPLETE) {
        return Err(std::io::Error::new(
            std::io::ErrorKind::InvalidInput,
            "refused local file name",
        ));
    }
    for i in 0..1000 {
        let name = if i == 0 {
            format!("{stem}{suffix}")
        } else {
            format!("{stem}-{i}{suffix}")
        };
        let target = dir.join(name);
        match std::fs::hard_link(partial, &target) {
            Ok(()) => {
                let cleanup_warning = std::fs::remove_file(partial)
                    .err()
                    .map(|e| format!("published, but {} was not removed: {e}", partial.display()));
                return Ok(Published {
                    path: target,
                    cleanup_warning,
                });
            }
            Err(e) if e.kind() == std::io::ErrorKind::AlreadyExists => continue,
            Err(e) => {
                let kept = partial.display();
                let msg = format!("not published (hard links unsupported?); {kept} is kept: {e}");
                return Err(std::io::Error::new(e.kind(), msg));
            }
        }
    }
    Err(std::io::Error::other("no free file name"))
}

#[cfg(test)]
mod prefix_tests {
    use super::*;

    #[test]
    fn prefix_vec_copies_only_the_prefix_across_blocks() {
        let mut r = Received::default();
        let data: Vec<u8> = (0..(BLOCK_BYTES * 2 + 300)).map(|i| (i * 7) as u8).collect();
        r.push(&data);
        assert_eq!(r.block_count(), 3);
        for n in [0, 1, BLOCK_BYTES - 1, BLOCK_BYTES, BLOCK_BYTES + 5, BLOCK_BYTES * 2 + 300] {
            let v = r.prefix_vec(n as u64).unwrap();
            assert_eq!(v, &data[..n], "{n}");
            assert!(v.capacity() == n || n == 0, "{n}: allocated only the prefix");
        }
        assert!(r.prefix_vec(data.len() as u64 + 1).is_none());
    }
}
