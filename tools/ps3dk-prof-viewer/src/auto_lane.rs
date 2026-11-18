//! The reader's per-frame RSX busy time for the Frames page's RSX lane.
//!
//! [`AutoLane`] is aligned with [`Owner`](crate::frames::Owner) slots: one mark per slot, drawn at the
//! same x column as the PPU bar and against the same budget line.  It reads only the reader's
//! `FrameView`s — the same ones the Automatic page displays (`AutoView::report.frames`) and the CLI
//! `auto --json` prints — so no measurement is re-derived here: a frame's RSX busy lower bound in PPU
//! ticks when the reader mapped it (`rsx_busy_ppu.lo`, `Busy`), or the reader's own reason for that
//! frame when it did not (`Unavailable`, the verbatim reader text shown on the page and on hover).
//! Unavailable frames are never 0 ms and are never omitted.
use crate::frames::Owner;
use ps3dk_prof::auto_rsx::FrameView;

/// One slot on the RSX lane.
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum AutoFrame {
    /// `rsx_busy_ppu.lo` — the reader's guaranteed lower bound of the RSX busy time, in PPU ticks.
    Busy(u64),
    /// `rsx_busy_ppu` unqualified: the reader's own reason for this frame (shown on the page and hover).
    Unavailable(String),
}

/// The RSX busy-time lane, one mark per owner slot; `None` at a slot that is not an AUTO frame.
#[derive(Debug, Clone, Default, PartialEq, Eq)]
pub struct AutoLane {
    /// Per slot index.
    pub slots: Vec<Option<AutoFrame>>,
}

impl AutoLane {
    /// `None` when no AUTO frame in `frames` is a frame of `owner`: there is no lane to draw in that case.
    pub fn build(frames: &[FrameView], owner: &Owner) -> Option<AutoLane> {
        let mut lane = AutoLane { slots: vec![None; owner.slots.len()] };
        let mut any = false;
        for fr in frames.iter().filter(|f| f.owner == owner.incarnation) {
            let Some((i, _)) = owner.slot_of(fr.frame) else {
                continue;
            };
            // Any AUTO frame of this owner makes a lane, even when every one is unavailable (for example an
            // unqualified clock map): those frames are shown with the reader's reason, never dropped.
            any = true;
            let arm = match fr.rsx_busy_ppu {
                Some((l, _)) => AutoFrame::Busy(l),
                None => AutoFrame::Unavailable(fr.reason.clone()),
            };
            lane.slots[i] = Some(arm);
        }
        any.then_some(lane)
    }

    /// The lane restricted to a frame range, when the page's scope is a named window or a range of
    /// frames.  Same per-frame facts as [`build`]; frames outside `range` dropped.
    pub fn build_filtered(
        frames: &[FrameView],
        owner: &Owner,
        range: std::ops::RangeInclusive<u64>,
    ) -> Option<AutoLane> {
        let filtered: Vec<FrameView> = frames
            .iter()
            .filter(|f| f.owner == owner.incarnation && range.contains(&f.frame))
            .cloned()
            .collect();
        Self::build(&filtered, owner)
    }
}
