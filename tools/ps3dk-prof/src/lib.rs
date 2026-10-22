//! ps3dk-prof: reader, checker and report for ps3dk-prof capture files (format v1).
//!
//! The normative format is docs/design/ps3dk-prof-format.md.  [`load`] gives either a MALFORMED verdict (with the
//! exact rule broken) or an [`Analysis`] of a COMPLETE or INCOMPLETE capture.

pub mod analysis;
pub mod hash;
pub mod model;
pub mod reader;
pub mod report;

pub use analysis::{
    compare, load, AbResult, AbRow, Analysis, Attribution, FrameElapsed, LaneView, Outcome, SpanView, SpansN,
    TokenOutcome, TokenRef, Work, ZoneStats,
};
pub use model::{Capture, Incomplete, Malformed, Metric, Rule, Verdict};
