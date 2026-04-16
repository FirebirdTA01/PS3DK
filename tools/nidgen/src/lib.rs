//! nidgen — PS3 NID/FNID utilities and stub generator.
//!
//! Exposes the FNID algorithm, YAML database loader, stub emitter, and verify
//! routines as a library so other tools (coverage-report) can consume them.

pub mod nid;
pub mod db;
pub mod stubgen;
pub mod archive;
pub mod verify;
