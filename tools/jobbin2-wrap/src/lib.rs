//! Clean-room SPURS jobbin2 wrapper support.
//!
//! Initial scope is inspection only: parse a generated SPU ELF / jobbin2
//! artifact set and emit a JSON summary suitable for writing the field map
//! without copying reference bytes into the repository.

pub const JOBBIN2_PREFIX_SIZE: usize = 0x100;

pub mod jobbin2;
pub mod jobheader;
pub mod ppu_obj;
pub mod report;
pub mod spu_elf;
