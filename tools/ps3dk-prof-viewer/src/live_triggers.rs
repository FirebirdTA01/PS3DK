//! Live capture triggers, viewer side: whether the buttons may send, the one command waiting for its answer, and the
//! last answer.  No sockets or egui types; `live_client` feeds it the link state and the ACKs, the UI reads it.
//!
//! The agent answers every command exactly once (a CAPTURE_ACK) unless the link ends first, and accepts one command
//! at a time, so the buttons are disabled while a command is pending.  A v1 agent never sees a command frame: the
//! buttons stay disabled with the reason.

use crate::live::{result_text, Ack, Command, Hello, LinkState, CAP_TRIGGERS};

/// Whether this session can send commands at all.
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum Availability {
    /// No HELLO yet.
    Connecting,
    /// The agent speaks protocol 1 (an older agent): it has no triggers.
    V1Agent,
    /// A version-2 agent that does not apply triggers (it serves a file without a runtime).
    NoCapability,
    Ready,
    /// The link ended or failed: nothing more can be sent.
    Ended,
}

/// The state of the last command.
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum Status {
    Idle,
    /// Sent; its answer has not arrived.
    Pending { request: u32, command: Command },
    /// Done: the window opened (Begin, Frames) or will close (End) at the frame boundary.
    Acked { command: Command, window_id: u32 },
    /// Refused by the app: nothing changed in the capture.
    Refused { command: Command, code: i32, text: String },
    /// The link ended before the answer arrived: whether it was applied is unknown.
    Unanswered { command: Command },
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Triggers {
    pub availability: Availability,
    pub status: Status,
    next_request: u32,
}

impl Default for Triggers {
    fn default() -> Self {
        Triggers { availability: Availability::Connecting, status: Status::Idle, next_request: 1 }
    }
}

impl Triggers {
    /// Follows the link: availability from HELLO, and a pending command that the link's end left unanswered.
    pub fn on_link(&mut self, link: &LinkState, hello: Option<&Hello>) {
        self.availability = match (link, hello) {
            (LinkState::Ended(_) | LinkState::Failed(_), _) => Availability::Ended,
            (_, None) => Availability::Connecting,
            (_, Some(h)) if h.protocol < 2 => Availability::V1Agent,
            (_, Some(h)) if h.capabilities & CAP_TRIGGERS == 0 => Availability::NoCapability,
            _ => Availability::Ready,
        };
        if self.availability == Availability::Ended {
            if let Status::Pending { command, .. } = &self.status {
                self.status = Status::Unanswered { command: command.clone() };
            }
        }
    }

    /// Why nothing can be sent now, or None.
    pub fn blocked(&self) -> Option<String> {
        match self.availability {
            Availability::Connecting => return Some("waiting for the console's HELLO".into()),
            Availability::V1Agent => {
                return Some(
                    "the app's live agent speaks protocol 1, which has no capture triggers; rebuild the app with a \
                     newer libps3dkprof to use them"
                        .into(),
                )
            }
            Availability::NoCapability => return Some("this agent does not apply capture triggers".into()),
            Availability::Ended => return Some("the live link has ended".into()),
            Availability::Ready => {}
        }
        match &self.status {
            Status::Pending { command, .. } => Some(format!("waiting for the app to answer {}", command.label())),
            _ => None,
        }
    }

    /// Takes a request id for `command` and marks it pending.  `Err` says why it cannot be sent (nothing changes).
    pub fn start(&mut self, command: Command) -> Result<u32, String> {
        if let Some(why) = self.blocked() {
            return Err(why);
        }
        command.check()?;
        let request = self.next_request;
        self.next_request = self.next_request.wrapping_add(1).max(1);
        self.status = Status::Pending { request, command };
        Ok(request)
    }

    /// The send of a pending command failed: it was never sent.
    pub fn send_failed(&mut self, request: u32, why: &str) {
        if let Status::Pending { request: r, command } = &self.status {
            if *r == request {
                self.status = Status::Refused { command: command.clone(), code: 0, text: format!("not sent: {why}") };
            }
        }
    }

    /// The answer to the pending command (the session already checked that it matches).
    pub fn on_ack(&mut self, ack: Ack) {
        let Status::Pending { request, command } = &self.status else {
            return;
        };
        if *request != ack.request {
            return;
        }
        let command = command.clone();
        self.status = if ack.result == 0 {
            Status::Acked { command, window_id: ack.window_id }
        } else {
            Status::Refused { command, code: ack.result, text: result_text(ack.result) }
        };
    }

    /// The status line under the buttons.
    pub fn status_line(&self) -> Option<(bool, String)> {
        match &self.status {
            Status::Idle => None,
            Status::Pending { command, .. } => {
                Some((false, format!("{}: sent, waiting for the next frame mark…", command.label())))
            }
            Status::Acked { command: Command::End, window_id } => Some((
                false,
                format!("End: window {window_id} closes after the current frame; it appears in the capture as it closes"),
            )),
            Status::Acked { command, window_id } => {
                Some((false, format!("{}: window {window_id} opened at the next frame boundary", command.label())))
            }
            Status::Refused { command, code: 0, text } => Some((true, format!("{}: {text}", command.label()))),
            Status::Refused { command, text, .. } => Some((true, format!("{}: refused, {text}", command.label()))),
            Status::Unanswered { command } => Some((
                true,
                format!("{}: the link ended before the app answered; check the capture's window list", command.label()),
            )),
        }
    }
}

/// The trigger row's inputs.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Form {
    pub name: String,
    pub frames: u32,
}

impl Default for Form {
    fn default() -> Self {
        Form { name: "viewer".into(), frames: 60 }
    }
}

/// Each button's state: Ok = enabled, Err = disabled with the reason (its hover text).
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Buttons {
    pub begin: Result<Command, String>,
    pub end: Result<Command, String>,
    pub frames: Result<Command, String>,
}

pub fn buttons(t: &Triggers, form: &Form) -> Buttons {
    let gate = |c: Command| -> Result<Command, String> {
        if let Some(why) = t.blocked() {
            return Err(why);
        }
        c.check().map(|_| c)
    };
    let name = form.name.trim().to_string();
    Buttons {
        begin: gate(Command::Begin { name: name.clone() }),
        end: gate(Command::End),
        frames: gate(Command::Frames { name, frames: form.frames }),
    }
}
