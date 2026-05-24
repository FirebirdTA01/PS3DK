//! nidgen — PS3 NID/FNID utilities and stub generator.
//!
//! Exposes the FNID algorithm, YAML database loader, stub emitter, and verify
//! routines as a library so other tools (coverage-report) can consume them.

pub mod nid;
pub mod db;
pub mod stubgen;
pub mod archive;
pub mod extract;
pub mod verify;

/// ABI width for stub generation.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum AbiMode {
    /// ELF64 + ILP32 hybrid (default — 32-bit pointers in 64-bit ELF).
    Ilp32,
    /// ELF64 + LP64 (64-bit pointers).
    Lp64,
}

impl AbiMode {
    pub fn from_str(s: &str) -> Option<Self> {
        match s {
            "ilp32" => Some(AbiMode::Ilp32),
            "lp64"  => Some(AbiMode::Lp64),
            _       => None,
        }
    }

    pub fn as_flag(&self) -> &'static str {
        match self {
            AbiMode::Ilp32 => "",
            AbiMode::Lp64  => "-mlp64",
        }
    }
}
