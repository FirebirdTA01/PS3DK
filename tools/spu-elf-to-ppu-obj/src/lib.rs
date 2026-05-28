//! Independent SPU ELF to PPU object wrapper support.
//!
//! The crate can inspect existing artifact sets and wrap a linked SPU ELF into
//! a PPU relocatable object carrying SPU image sections.

pub const JOBBIN2_PREFIX_SIZE: usize = 0x100;

pub mod elf_embed;
pub mod encoder;
pub mod jobbin2;
pub mod jobheader;
pub mod patches;
pub mod ppu_obj;
pub mod ppu_write;
pub mod report;
pub mod spu_elf;
pub mod wrapper;
