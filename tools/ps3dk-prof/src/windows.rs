//! Profiler capture windows (file_flags bit 9, chunks 41 WINDOW_BEGIN and 42 WINDOW_END).
//!
//! Windows are sequential, non-overlapping intervals of frame tokens.  Every WINDOW_END
//! matches the open WINDOW_BEGIN, with strictly increasing window IDs.

use crate::model::{
    malformed, Capture, CaptureWindow, Malformed, Rule, WindowKind, WindowReason, WindowSource,
    CHUNK_WINDOW_BEGIN, CHUNK_WINDOW_END, WINDOW_BEGIN_LEN, WINDOW_END_LEN,
    WINDOW_KIND_FRAMES, WINDOW_KIND_MANUAL, WINDOW_KIND_SECTION, WINDOW_REASON_COUNT,
    WINDOW_REASON_END, WINDOW_REASON_REFUSED_CAPACITY, WINDOW_REASON_SEAL,
    WINDOW_REASON_SHUTDOWN, WINDOW_SOURCE_APP, WINDOW_SOURCE_LIVE,
};
use crate::wire::Cursor;

/// WINDOW_BEGIN name_desc of an unnamed window (CAPTURE_WHOLE_RUN): no descriptor is declared for it.
pub const WINDOW_UNNAMED: u32 = u32::MAX;

pub const SCHEMA: u16 = 1;

#[derive(Debug, Clone)]
pub struct PendingWindow {
    pub id: u32,
    pub name: String,
    pub name_desc: u32,
    pub first_frame: u64,
    pub source: WindowSource,
    pub kind: WindowKind,
}

#[derive(Debug, Default)]
pub struct Reader {
    pub windows: Vec<CaptureWindow>,
    pub open: Option<PendingWindow>,
    pub last_id: u32,
    pub prev_last_frame: Option<u64>,
}

impl Reader {
    pub fn new() -> Self {
        Self::default()
    }

    pub fn chunk(&mut self, ty: u32, payload: &[u8], cap: &Capture) -> Result<(), Malformed> {
        match ty {
            CHUNK_WINDOW_BEGIN => self.begin(payload, cap),
            CHUNK_WINDOW_END => self.end(payload),
            _ => malformed(Rule::Windows, format!("unknown window chunk type {ty}")),
        }
    }

    fn begin(&mut self, payload: &[u8], cap: &Capture) -> Result<(), Malformed> {
        if payload.len() != WINDOW_BEGIN_LEN {
            return malformed(
                Rule::Windows,
                format!("WINDOW_BEGIN payload len {} != {WINDOW_BEGIN_LEN}", payload.len()),
            );
        }
        if let Some(open) = &self.open {
            return malformed(
                Rule::Windows,
                format!("window BEGIN while window {} is still open", open.id),
            );
        }
        let mut c = Cursor::new(payload, "WINDOW_BEGIN");
        let schema = c.u16()?;
        let flags = c.u16()?;
        let id = c.u32()?;
        let name_desc = c.u32()?;
        let first_frame = c.u64()?;
        let source_raw = c.u8()?;
        let kind_raw = c.u8()?;
        let reserved = c.u16()?;
        let frames_count = c.u32()?;
        let reserved2 = c.u32()?;

        if schema != SCHEMA {
            return malformed(Rule::Windows, format!("window {id}: schema {schema}"));
        }
        if flags != 0 {
            return malformed(Rule::Windows, format!("window {id}: flags {flags:#x}"));
        }
        if reserved != 0 || reserved2 != 0 {
            return malformed(Rule::Windows, format!("window {id}: reserved nonzero"));
        }
        if id == 0 || id <= self.last_id {
            return malformed(
                Rule::Windows,
                format!("window id {id} <= previous window id {}", self.last_id),
            );
        }
        if let Some(prev_last) = self.prev_last_frame {
            if first_frame <= prev_last {
                return malformed(
                    Rule::Windows,
                    format!("window {id} first_frame {first_frame} <= previous window last_frame {prev_last}"),
                );
            }
        }
        // name_desc u32::MAX: an unnamed window (CAPTURE_WHOLE_RUN), which takes no descriptor; only MANUAL
        let name = if name_desc == WINDOW_UNNAMED {
            if kind_raw != WINDOW_KIND_MANUAL || source_raw != WINDOW_SOURCE_APP {
                return malformed(Rule::Windows, format!("window {id}: unnamed window of kind {kind_raw} source {source_raw}                     (only an application MANUAL window may be unnamed)"));
            }
            "whole run".to_string()
        } else {
            cap.descriptors.get(&name_desc).ok_or_else(|| Malformed {
                rule: Rule::Windows,
                detail: format!("window {id}: undeclared name descriptor {name_desc}"),
            })?.name.clone()
        };

        let source = match source_raw {
            WINDOW_SOURCE_APP => WindowSource::App,
            WINDOW_SOURCE_LIVE => WindowSource::Live,
            _ => return malformed(Rule::Windows, format!("window {id}: unknown source {source_raw}")),
        };
        let kind = match kind_raw {
            WINDOW_KIND_MANUAL => {
                if frames_count != 0 {
                    return malformed(Rule::Windows, format!("window {id}: non-zero frames_count for MANUAL"));
                }
                WindowKind::Manual
            }
            WINDOW_KIND_FRAMES => {
                if frames_count == 0 {
                    return malformed(Rule::Windows, format!("window {id}: zero frames_count for FRAMES"));
                }
                WindowKind::Frames(frames_count)
            }
            WINDOW_KIND_SECTION => {
                if frames_count != 0 {
                    return malformed(Rule::Windows, format!("window {id}: non-zero frames_count for SECTION"));
                }
                WindowKind::Section
            }
            _ => return malformed(Rule::Windows, format!("window {id}: unknown kind {kind_raw}")),
        };

        self.open = Some(PendingWindow {
            id,
            name,
            name_desc,
            first_frame,
            source,
            kind,
        });
        Ok(())
    }

    fn end(&mut self, payload: &[u8]) -> Result<(), Malformed> {
        if payload.len() != WINDOW_END_LEN {
            return malformed(
                Rule::Windows,
                format!("WINDOW_END payload len {} != {WINDOW_END_LEN}", payload.len()),
            );
        }
        let open = self.open.take().ok_or_else(|| Malformed {
            rule: Rule::Windows,
            detail: "WINDOW_END without an open window".to_string(),
        })?;
        let mut c = Cursor::new(payload, "WINDOW_END");
        let schema = c.u16()?;
        let flags = c.u16()?;
        let id = c.u32()?;
        let reason_raw = c.u8()?;
        let r0 = c.u8()?;
        let r1 = c.u8()?;
        let r2 = c.u8()?;
        let last_frame = c.u64()?;
        let reserved2 = c.take(12)?;

        if schema != SCHEMA {
            return malformed(Rule::Windows, format!("WINDOW_END id {id}: schema {schema}"));
        }
        if flags != 0 {
            return malformed(Rule::Windows, format!("WINDOW_END id {id}: flags {flags:#x}"));
        }
        if r0 != 0 || r1 != 0 || r2 != 0 || reserved2.iter().any(|&b| b != 0) {
            return malformed(Rule::Windows, format!("WINDOW_END id {id}: reserved nonzero"));
        }
        if id != open.id {
            return malformed(
                Rule::Windows,
                format!("WINDOW_END id {id} does not match open window id {}", open.id),
            );
        }
        if last_frame < open.first_frame {
            return malformed(
                Rule::Windows,
                format!("window {id}: last_frame {last_frame} < first_frame {}", open.first_frame),
            );
        }
        let reason = match reason_raw {
            WINDOW_REASON_END => WindowReason::End,
            WINDOW_REASON_COUNT => {
                match open.kind {
                    WindowKind::Frames(expected) => {
                        let span = last_frame - open.first_frame + 1;
                        if span != expected as u64 {
                            return malformed(
                                Rule::Windows,
                                format!("window {id}: reason COUNT span {span} != frames_count {expected}"),
                            );
                        }
                        WindowReason::Count
                    }
                    _ => {
                        return malformed(
                            Rule::Windows,
                            format!("window {id}: reason COUNT is valid only for kind FRAMES"),
                        );
                    }
                }
            }
            WINDOW_REASON_SHUTDOWN => WindowReason::Shutdown,
            WINDOW_REASON_SEAL => WindowReason::Seal,
            WINDOW_REASON_REFUSED_CAPACITY => WindowReason::RefusedCapacity,
            _ => return malformed(Rule::Windows, format!("window {id}: unknown reason {reason_raw}")),
        };

        self.last_id = id;
        self.prev_last_frame = Some(last_frame);
        self.windows.push(CaptureWindow {
            id,
            name: open.name,
            name_desc: open.name_desc,
            first_frame: open.first_frame,
            last_frame: Some(last_frame),
            source: open.source,
            kind: open.kind,
            reason,
            is_open: false,
            incomplete: false,
        });
        Ok(())
    }

    pub fn finish(mut self, frame_marks: &[u64]) -> Vec<CaptureWindow> {
        if let Some(open) = self.open.take() {
            let last_frame = frame_marks.iter().copied().filter(|&f| f >= open.first_frame).max();
            self.windows.push(CaptureWindow {
                id: open.id,
                name: open.name,
                name_desc: open.name_desc,
                first_frame: open.first_frame,
                last_frame,
                source: open.source,
                kind: open.kind,
                reason: WindowReason::OpenAtEnd,
                is_open: true,
                incomplete: true,
            });
        }
        self.windows
    }
}
