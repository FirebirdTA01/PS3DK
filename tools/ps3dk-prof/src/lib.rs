//! ps3dk-prof: reader, checker and report for ps3dk-prof capture files (format v1).
//!
//! The normative format is docs/design/ps3dk-prof-format.md.  [`load`] gives either a MALFORMED verdict (with the
//! exact rule broken) or an [`Analysis`] of a COMPLETE or INCOMPLETE capture.
//!
//! Two passes, and only the second is a verdict:
//! - [`reader::read`] is the STRUCTURAL pass (sections 1-6).  A successful `read` is NOT a full validity verdict:
//!   token rule 4 (section 9) needs the whole file.
//! - [`load`] runs `read` and then [`analysis::analyze`], which applies rule 4.  `ps3dk-prof check`, `report` and
//!   [`compare`] (which takes two [`Analysis`] values) all go through `load`.

pub mod analysis;
pub mod hash;
pub mod model;
pub mod reader;
pub mod report;

pub use analysis::{
    compare, load, AbResult, AbRow, Analysis, Attribution, Comparison, FrameElapsed, LaneView, Outcome, SpanView,
    SpansN, TokenOutcome, TokenRef, Work, ZoneStats,
};
pub use model::{Capture, Incomplete, Malformed, Metric, Rule, Verdict};
