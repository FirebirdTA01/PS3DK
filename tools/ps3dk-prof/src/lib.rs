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

#[cfg(test)]
extern crate self as ps3dk_prof;

pub mod analysis;
pub mod clockmap;
pub mod checkpoints;
pub mod source_checkpoints;
pub mod dependencies;
pub mod durations;
pub mod ext;
pub mod fifo;
pub mod foreign_paths;
pub mod foreign_timeline;
pub mod func_paths;
pub mod func_report;
pub mod elf_symbols;
pub mod frames_foreign;
pub mod hash;
pub mod model;
pub mod paths;
pub mod profile;
pub mod reader;
pub mod rsx_measured;
pub mod report;
pub mod rsx_capture;
pub mod spu_ticks;
pub mod waits;

pub use analysis::{
    compare, compare_with, load, load_file, load_prefix, AbResult, AbRow, Analysis, Attribution, CompareOptions,
    Comparison, ComparisonKind, FrameElapsed, LaneView, Outcome, SpanView, SpansN, TokenOutcome,
    TokenRef, PrefixAcceptance, Work, ZoneStats,
};
pub use model::{Capture, Incomplete, Malformed, Metric, Rule, Verdict};

pub mod arcs;
pub mod companions;
pub mod gmon;
mod wire;
