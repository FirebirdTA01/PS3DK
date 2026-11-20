//! RSX capture request progress. Wire progress never substitutes for parsed DATA.
use crate::live::{Hello, LinkState, CAP_RSX_FRAME, T_RSX_FRAME_REQ};

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct RsxFrameRequest {
    pub name: String,
}
/// Request flag bits (contract r7): isolated timing, keep fragment-program bytes, both modes in two frames.
pub const FLAG_ISOLATED: u16 = 1;
pub const FLAG_FP_BYTES: u16 = 2;
pub const FLAG_BOTH: u16 = 4;

/// The timing mode a capture is requested with.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Default)]
pub enum Mode {
    /// Reached-marker intervals; draws overlap as the RSX pipelines them.
    #[default]
    Pipelined,
    /// Each draw waits for the previous one: isolated cost, a slower frame.
    Isolated,
    /// Two consecutive frames: the first pipelined, the second isolated.
    Both,
}
impl Mode {
    pub fn name(self) -> &'static str {
        match self {
            Mode::Pipelined => "Pipelined",
            Mode::Isolated => "Isolated",
            Mode::Both => "Both",
        }
    }
    pub fn hint(self) -> &'static str {
        match self {
            Mode::Pipelined => "Reached-marker intervals: draws overlap, so their sum is not isolated cost.",
            Mode::Isolated => "The RSX waits for idle around each draw: isolated cost per draw; the captured frame runs slower.",
            Mode::Both => "Two consecutive frames: the first pipelined, the second isolated, shown side by side and paired per draw.",
        }
    }
}
/// The menu's choice: a mode and whether to keep fragment-program bytes for the Shader view.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Default)]
pub struct RequestOptions {
    pub mode: Mode,
    pub fp_bytes: bool,
}
impl RequestOptions {
    pub fn flags(self) -> u16 {
        (match self.mode {
            Mode::Pipelined => 0,
            Mode::Isolated => FLAG_ISOLATED,
            Mode::Both => FLAG_BOTH,
        }) | if self.fp_bytes { FLAG_FP_BYTES } else { 0 }
    }
}

impl RsxFrameRequest {
    pub fn encode(&self, request: u32) -> Result<Vec<u8>, String> {
        self.encode_with(request, 0)
    }
    pub fn encode_with(&self, request: u32, flags: u16) -> Result<Vec<u8>, String> {
        if flags & !(FLAG_ISOLATED | FLAG_FP_BYTES | FLAG_BOTH) != 0 || flags & (FLAG_ISOLATED | FLAG_BOTH) == FLAG_ISOLATED | FLAG_BOTH {
            return Err(refusal(-5));
        }
        if request == 0 || self.name.len() > 63 || self.name.contains('\0') {
            return Err("RSX capture requires a nonzero request and a name of at most 63 UTF-8 bytes without NUL".into());
        }
        let mut b = T_RSX_FRAME_REQ.to_le_bytes().to_vec();
        b.extend(0u16.to_le_bytes());
        b.extend((12 + self.name.len() as u32).to_le_bytes());
        b.extend(request.to_le_bytes());
        b.extend(flags.to_le_bytes());
        b.extend(1u16.to_le_bytes());
        b.extend((self.name.len() as u16).to_le_bytes());
        b.extend(0u16.to_le_bytes());
        b.extend(self.name.as_bytes());
        Ok(b)
    }
}
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct RsxFrameAck {
    pub request: u32,
    pub result: i32,
    pub capture: u32,
}
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct RsxFrameState {
    pub capture: u32,
    pub state: u8,
    pub reason: u8,
    pub draws: u32,
    pub request: u32,
    pub frame: u64,
    pub bytes: u64,
}
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum RsxFrameEvent {
    Ack(RsxFrameAck),
    State(RsxFrameState),
}

pub fn refusal(code: i32) -> String {
    match code {
        -2=>"EBUSY: the console still follows an earlier RSX frame capture (its final state has not arrived); request again once it has".into(),
        -5=>"EINVAL: the request was refused: isolated and both cannot be combined, the name is invalid, or this agent does not support the selected mode or shader-bytes option".into(),
        -6=>"ENOMEM: capture staging memory could not be allocated".into(),
        -14=>"ENOTINIT: the profiler is not initialized".into(),
        -15=>"ESTOPPED: the profiler is shutting down".into(),
        -17=>"ENOTSUP: RSX frame capture needs PS3_PROF_RSX_FRAME, an attached report ring and an AUTO owner".into(),
        n=>format!("RSX capture refused with code {n}"),
    }
}

/// A fresh tracker belongs to one LiveClient; it never survives reconnection.
#[derive(Debug, Clone)]
pub struct RequestTracker {
    pub request: Option<u32>,
    pub ack: Option<RsxFrameAck>,
    pub progress: Option<RsxFrameState>,
    pub error: Option<String>,
    pub unavailable: Option<String>,
    pub ended: bool,
    pub resolved: bool,
    pub timing_counts: Option<(u32, u32)>,
    next: u32,
}
impl Default for RequestTracker {
    fn default() -> Self {
        Self {
            request: None,
            ack: None,
            progress: None,
            error: None,
            unavailable: Some("waiting for the console's HELLO".into()),
            ended: false,
            resolved: false,
            timing_counts: None,
            next: 1,
        }
    }
}
impl RequestTracker {
    pub fn on_link(&mut self, link: &LinkState, hello: Option<&Hello>) {
        self.ended = matches!(link, LinkState::Ended(_) | LinkState::Failed(_));
        self.unavailable = if self.ended {
            Some("the live link has ended".into())
        } else {
            match hello {
            None=>Some("waiting for the console's HELLO".into()),
            Some(h) if h.protocol<3=>Some("RSX frame requests need live protocol 3; rebuild the app with the newer profiler".into()),
            Some(h) if h.capabilities&CAP_RSX_FRAME==0=>Some(refusal(-17)),
            _=>None,
        }
        };
    }
    pub fn blocked(&self) -> Option<String> {
        self.unavailable.clone().or_else(|| {
            // One followed capture per link: the agent answers EBUSY until it has sent the
            // final STATE (ENCODED or ABANDONED), even when the capture data already arrived.
            if self.request.is_some()
                && self.error.is_none()
                && !self.progress.is_some_and(|s| matches!(s.state, 3 | 4))
            {
                Some("waiting for the outstanding RSX frame capture".into())
            } else {
                None
            }
        })
    }
    pub fn start(&mut self, name: String) -> Result<(u32, Vec<u8>), String> {
        self.start_with(name, 0)
    }
    pub fn start_with(&mut self, name: String, flags: u16) -> Result<(u32, Vec<u8>), String> {
        if let Some(e) = self.blocked() {
            return Err(e);
        }
        let id = self.next;
        let next = id
            .checked_add(1)
            .ok_or("RSX request ids exhausted; reconnect")?;
        let bytes = RsxFrameRequest { name }.encode_with(id, flags)?;
        self.next = next;
        self.request = Some(id);
        self.ack = None;
        self.progress = None;
        self.error = None;
        self.resolved = false;
        self.timing_counts = None;
        Ok((id, bytes))
    }
    pub fn event(&mut self, e: RsxFrameEvent) {
        match e {
            RsxFrameEvent::Ack(a) if self.request == Some(a.request) => {
                self.ack = Some(a);
                if a.result != 0 {
                    self.error = Some(refusal(a.result));
                }
            }
            RsxFrameEvent::State(s)
                if self.request == Some(s.request)
                    && self.ack.is_some_and(|a| a.capture == s.capture) =>
            {
                self.progress = Some(s);
            }
            _ => {}
        }
    }
    pub fn send_failed(&mut self, request: u32, why: &str) {
        if self.request == Some(request) {
            self.error = Some(format!("Not sent: {why}"));
        }
    }
    pub fn observe(&mut self, frames: &[ps3dk_prof::rsx_frame::FrameView]) {
        let Some(ack) = self.ack.filter(|a| a.result == 0) else {
            return;
        };
        use ps3dk_prof::rsx_frame::FrameStatus;
        let terminal = |f: &&ps3dk_prof::rsx_frame::FrameView| {
            matches!(f.status, FrameStatus::Complete | FrameStatus::Truncated { .. } | FrameStatus::Abandoned { .. })
        };
        // A BOTH capture is two views (pipelined, isolated): Ready only when both are; the counts are the
        // first unfinished view's own (never summed across timing modes).
        let views: Vec<_> = frames.iter().filter(|f| f.id == ack.capture && f.request_id == ack.request).collect();
        if let Some(f) = views.iter().find(|f| !terminal(f)).or(views.last()) {
            self.timing_counts = Some((f.totals.timed, f.totals.draws));
            self.resolved = views.iter().all(terminal);
        }
    }
    pub fn status_line(&self) -> Option<String> {
        if let Some(e) = &self.error {
            return Some(e.clone());
        }
        self.request?;
        if self.resolved {
            return Some("Received: open the RSX Frames page for its completion status".into());
        }
        if self.ended && !self.resolved {
            return Some(
                "Live link ended; inspect the received capture for incomplete timing".into(),
            );
        }
        Some(match self.progress {
            Some(s) if s.state == 1 => format!("Armed: frame {}", s.frame),
            Some(s) if s.state == 2 => format!("Closed: {} draws, {} bytes", s.draws, s.bytes),
            Some(s) if s.state == 4 => format!(
                "Abandoned: {}",
                ps3dk_prof::rsx_frame::end_reason(s.reason as u16)
            ),
            Some(s) => {
                let (timed, total) = self.timing_counts.unwrap_or((0, s.draws));
                format!("Waiting for RSX ({timed}/{total})")
            },
            None => "Requested: waiting for a frame".into(),
        })
    }
}
