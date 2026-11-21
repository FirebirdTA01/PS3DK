//! NV40 (RSX) vertex- and fragment-program microcode disassembler, for the viewer's Shader view.
//!
//! Every field position comes from our own shader compiler's encoder, never from a reading of another tool's
//! output: `tools/rsx-cg-compiler/src/nv40/` - `nvfx_shader.h` (opcodes, fragment fields), `nv40_vertprog.h`
//! (vertex fields), `nv40_vp_assembler.cpp` (how sources, destinations, co-issue, BRA and TXL are packed),
//! `nv40_fp_assembler.cpp` (fragment packing, the halfword swap, inline constant blocks, FENCBR/FENCTR) and the raw
//! LOOP/BRK words `nv40_general_lowering.cpp` emits.  What those files leave open is not guessed:
//!
//! - an opcode with no encoder definition prints as its raw words behind [`UNKNOWN_MARK`] (kind
//!   [`LineKind::Unknown`]);
//! - a set bit the encoder never defines, inside an otherwise known instruction, is appended as
//!   `?bits(wN:0x........)` (the word index and the offending bits, in the LOGICAL layout - for a fragment program,
//!   after the halfword swap).
//!
//! Syntax.  Vertex: `R<n>` temps, `v<n>` inputs, `c<n>` / `c[A0.x+<n>]` constants, `o[NAME]` outputs, `A<n>`
//! address registers, `RC` a condition-code-only write; a co-issued instruction prints `vector + scalar`.  Fragment:
//! `R<n>` / `H<n>` full/half registers (the colour and depth outputs share this file), `f[NAME]` inputs, `c0` the
//! instruction's inline constant (the 16-byte block that follows is printed as a [`LineKind::Constant`] line).  The
//! fragment mnemonic carries the precision (`R` fp32, `H` fp16, `X` fx12), `C` for a condition-code write, the
//! output scale (`_x2`.. `_d8`) and `_SAT`.  A predicate prints after the destination, `(NE.xxxx)` (vertex `NE1`
//! for condition register 1).  Swizzles and write masks are omitted when they are the identity / all lanes.
//! Modifiers follow two spaces: `{disable_pc}`, `?bits(..)`, and [`END_MARK`] on the instruction that ends the
//! program.

use crate::rsx_frame::{FP_MAX_INSTRUCTIONS, VP_MAX_INSTRUCTIONS};

/// Prefix of every line whose encoding is not known.
pub const UNKNOWN_MARK: &str = "??";
/// Suffix of the instruction carrying the end bit (vertex LAST, fragment PROGRAM_END).
pub const END_MARK: &str = "[end]";

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum LineKind {
    /// A decoded instruction.
    Instruction,
    /// A fragment program's inline constant block (data, not code).
    Constant,
    /// Words whose encoding is not known (unknown opcode, truncated trailing bytes); printed raw.
    Unknown,
    /// A remark about the program as a whole (no end bit); `words` is empty.
    Note,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Line {
    /// Byte offset of the first word from the start of the program.
    pub offset: usize,
    /// The words as they lie in the program: a vertex program's words as given, a fragment program's as stored in
    /// memory (big-endian, halfword-swapped - the form the compiler's word dump prints).
    pub words: Vec<u32>,
    pub text: String,
    pub kind: LineKind,
}

const LANES: [char; 4] = ['x', 'y', 'z', 'w'];
const COND: [&str; 8] = ["FL", "LT", "EQ", "LE", "GT", "NE", "GE", "TR"];
const COND_TR: u32 = 7;

fn bits(w: u32, lo: u32, n: u32) -> u32 {
    (w >> lo) & ((1u32 << n) - 1)
}

fn bit(w: u32, b: u32) -> bool {
    (w >> b) & 1 != 0
}

/// `.xyzw`-style selector text; empty for the identity.
fn swizzle(sel: [u32; 4]) -> String {
    if sel == [0, 1, 2, 3] {
        return String::new();
    }
    let mut s = String::from(".");
    for v in sel {
        s.push(LANES[(v & 3) as usize]);
    }
    s
}

/// Write-mask text in x,y,z,w order: `None` for no lane, empty for all four.
fn mask_text(m: [bool; 4]) -> Option<String> {
    if !m.iter().any(|&b| b) {
        return None;
    }
    if m.iter().all(|&b| b) {
        return Some(String::new());
    }
    let mut s = String::from(".");
    for (i, &on) in m.iter().enumerate() {
        if on {
            s.push(LANES[i]);
        }
    }
    Some(s)
}

fn dest_text(reg: &str, m: [bool; 4]) -> String {
    match mask_text(m) {
        Some(mt) => format!("{reg}{mt}"),
        None => "-".to_string(),
    }
}

fn raw_text(words: &[u32], why: &str) -> String {
    let hex: Vec<String> = words.iter().map(|w| format!("{w:08x}")).collect();
    format!("{UNKNOWN_MARK} {} ; {why}", hex.join(" "))
}

fn with_modifiers(mut text: String, mods: &[String], unknown: &[String], end: bool) -> String {
    for m in mods {
        text.push_str("  ");
        text.push_str(m);
    }
    if !unknown.is_empty() {
        text.push_str(&format!("  ?bits({})", unknown.join(" ")));
    }
    if end {
        text.push_str("  ");
        text.push_str(END_MARK);
    }
    text
}

fn source_mods(name: String, swz: String, neg: bool, abs: bool) -> String {
    let body = format!("{name}{swz}");
    let body = if abs { format!("|{body}|") } else { body };
    if neg {
        format!("-{body}")
    } else {
        body
    }
}

// ---- vertex programs ------------------------------------------------------------------------------------------------

/// A vector-slot opcode: name and the hardware source positions it reads (nvfx_shader.h NVFX_VP_INST_VEC_OP_*;
/// ADD reads positions 0 and 2, as the lowering's ADD remap and co-issue rule record).
fn vp_vec_op(op: u32) -> Option<(&'static str, &'static [usize])> {
    const NONE: &[usize] = &[];
    const S0: &[usize] = &[0];
    const S01: &[usize] = &[0, 1];
    const S02: &[usize] = &[0, 2];
    const S012: &[usize] = &[0, 1, 2];
    Some(match op {
        0x00 => ("NOP", NONE),
        0x01 => ("MOV", S0),
        0x02 => ("MUL", S01),
        0x03 => ("ADD", S02),
        0x04 => ("MAD", S012),
        0x05 => ("DP3", S01),
        0x06 => ("DPH", S01),
        0x07 => ("DP4", S01),
        0x08 => ("DST", S01),
        0x09 => ("MIN", S01),
        0x0A => ("MAX", S01),
        0x0B => ("SLT", S01),
        0x0C => ("SGE", S01),
        0x0D => ("ARL", S0),
        0x0E => ("FRC", S0),
        0x0F => ("FLR", S0),
        0x10 => ("SEQ", S01),
        0x11 => ("SFL", S01),
        0x12 => ("SGT", S01),
        0x13 => ("SLE", S01),
        0x14 => ("SNE", S01),
        0x15 => ("STR", S01),
        0x16 => ("SSG", S0),
        0x17 => ("ARR", S0),
        0x18 => ("ARA", S0),
        // NV40_VP_INST_VEC_OP_TXL: the coordinate (with its LOD lane) in position 0; the unit is hw[2] bits 8..9.
        0x19 => ("TXL", S0),
        _ => return None,
    })
}

#[derive(Clone, Copy, PartialEq, Eq)]
enum ScaKind {
    Arith,
    /// BRA / CAL: a 9-bit instruction target (IADDRH hw[2] bits 0..5, IADDRL hw[3] bits 29..31).
    Target,
    /// RET, PUSHA, POPA: no operand the encoder defines.
    Bare,
}

/// A scalar-slot opcode (NVFX_VP_INST_SCA_OP_*, NV40_VP_INST_SCA_OP_*).  Its one source is hardware position 2.
fn vp_sca_op(op: u32) -> Option<(&'static str, ScaKind)> {
    Some(match op {
        0x00 => ("NOP", ScaKind::Bare),
        0x01 => ("MOV", ScaKind::Arith),
        0x02 => ("RCP", ScaKind::Arith),
        0x03 => ("RCC", ScaKind::Arith),
        0x04 => ("RSQ", ScaKind::Arith),
        0x05 => ("EXP", ScaKind::Arith),
        0x06 => ("LOG", ScaKind::Arith),
        0x07 => ("LIT", ScaKind::Arith),
        0x09 => ("BRA", ScaKind::Target),
        0x0B => ("CAL", ScaKind::Target),
        0x0C => ("RET", ScaKind::Bare),
        0x0D => ("LG2", ScaKind::Arith),
        0x0E => ("EX2", ScaKind::Arith),
        0x0F => ("SIN", ScaKind::Arith),
        0x10 => ("COS", ScaKind::Arith),
        0x13 => ("PUSHA", ScaKind::Bare),
        0x14 => ("POPA", ScaKind::Bare),
        _ => return None,
    })
}

/// NV40_VP_INST_DEST_*: the one output-register field (hw[3] bits 2..6).  Register 6 carries both PSZ and the
/// encoder's TEXCOORD9 (nv40_vp_assembler.cpp routes TC(9) there).
fn vp_output(d: u32) -> String {
    match d {
        0 => "o[HPOS]".into(),
        1 => "o[COL0]".into(),
        2 => "o[COL1]".into(),
        3 => "o[BFC0]".into(),
        4 => "o[BFC1]".into(),
        5 => "o[FOGC]".into(),
        6 => "o[PSZ/TEX9]".into(),
        7..=15 => format!("o[TEX{}]", d - 7),
        _ => format!("o[{d}]"),
    }
}

struct Vp {
    hw: [u32; 4],
}

impl Vp {
    fn src_field(&self, pos: usize) -> u32 {
        let hw = &self.hw;
        match pos {
            0 => (bits(hw[1], 0, 8) << 9) | bits(hw[2], 23, 9),
            1 => bits(hw[2], 6, 17),
            _ => (bits(hw[2], 0, 6) << 11) | bits(hw[3], 21, 11),
        }
    }

    fn addr(&self) -> String {
        format!("A{}.{}", u32::from(bit(self.hw[0], 24)), LANES[bits(self.hw[0], 0, 2) as usize])
    }

    /// NV40_VP_SRC_*: type bits 0..1 (1 temp, 2 input, 3 const), temp index 2..7, swizzle 8..15 (x at 14), negate
    /// 16; the per-position abs flags are hw[0] bits 21..23.
    fn src(&self, pos: usize) -> String {
        let hw = &self.hw;
        let f = self.src_field(pos);
        let name = match f & 3 {
            1 => format!("R{}", bits(f, 2, 6)),
            2 => {
                let n = bits(hw[1], 8, 4);
                if bit(hw[0], 27) {
                    format!("v[{}+{n}]", self.addr())
                } else {
                    format!("v{n}")
                }
            }
            3 => {
                let n = bits(hw[1], 12, 9);
                if bit(hw[3], 1) {
                    format!("c[{}+{n}]", self.addr())
                } else {
                    format!("c{n}")
                }
            }
            _ => format!("{UNKNOWN_MARK}type0"),
        };
        let swz = swizzle([bits(f, 14, 2), bits(f, 12, 2), bits(f, 10, 2), bits(f, 8, 2)]);
        source_mods(name, swz, bit(f, 16), bit(hw[0], 21 + pos as u32))
    }

    fn cc_update(&self) -> bool {
        bit(self.hw[0], 14) && bit(self.hw[0], 29)
    }

    fn cc_reg_suffix(&self) -> &'static str {
        if bit(self.hw[0], 25) {
            "1"
        } else {
            ""
        }
    }

    /// The condition field (hw[0] bits 10..12) with its swizzle (x at bits 8..9 .. w at 2..3).
    fn cond_text(&self) -> String {
        let hw0 = self.hw[0];
        let sel = [bits(hw0, 8, 2), bits(hw0, 6, 2), bits(hw0, 4, 2), bits(hw0, 2, 2)];
        format!("({}{}{})", COND[bits(hw0, 10, 3) as usize], self.cc_reg_suffix(), swizzle(sel))
    }

    /// Mnemonic and predicate for the half that owns the shared modifier fields (the vector half when both issue,
    /// as the encoder's co-issue packing writes only the vector instruction's flags).
    fn mnemonic(&self, name: &str, owns_flags: bool) -> String {
        let mut m = name.to_string();
        if owns_flags && self.cc_update() {
            m.push('C');
            m.push_str(self.cc_reg_suffix());
        }
        if owns_flags && bit(self.hw[0], 26) {
            m.push_str("_SAT");
        }
        m
    }

    fn predicate(&self, owns_flags: bool) -> String {
        if owns_flags && bit(self.hw[0], 13) {
            format!(" {}", self.cond_text())
        } else {
            String::new()
        }
    }

    fn vec_text(&self, name: &str, srcs: &[usize], unknown: &mut Vec<String>) -> String {
        let hw = &self.hw;
        let temp = bits(hw[0], 15, 6);
        let result = bit(hw[0], 30);
        let mask = [bit(hw[3], 16), bit(hw[3], 15), bit(hw[3], 14), bit(hw[3], 13)];
        let reg = if matches!(name, "ARL" | "ARR" | "ARA") {
            format!("A{temp}")
        } else if result {
            if temp != 0x3F {
                unknown.push(format!("w0:{:#010x}", hw[0] & (0x3F << 15)));
            }
            vp_output(bits(hw[3], 2, 5))
        } else if temp != 0x3F {
            format!("R{temp}")
        } else if self.cc_update() {
            "RC".to_string()
        } else {
            "-".to_string()
        };
        let mut ops = vec![format!("{}{}", dest_text(&reg, mask), self.predicate(true))];
        if reg == "-" {
            ops[0] = format!("-{}", self.predicate(true));
        }
        for &p in srcs {
            ops.push(self.src(p));
        }
        if name == "TXL" {
            ops.push(format!("TEX{}", bits(hw[2], 8, 2)));
        }
        format!("{} {}", self.mnemonic(name, true), ops.join(", "))
    }

    fn sca_text(&self, name: &str, kind: ScaKind, owns_flags: bool, unknown: &mut Vec<String>) -> String {
        let hw = &self.hw;
        let mnemonic = self.mnemonic(name, owns_flags);
        match kind {
            ScaKind::Target => {
                let target = (bits(hw[2], 0, 6) << 3) | bits(hw[3], 29, 3);
                let cond = if bits(hw[0], 10, 3) != COND_TR { format!(" {}", self.cond_text()) } else { String::new() };
                return format!("{mnemonic}{cond} @{target}");
            }
            ScaKind::Bare => {
                let cond = if bits(hw[0], 10, 3) != COND_TR && name == "RET" {
                    format!(" {}", self.cond_text())
                } else {
                    String::new()
                };
                return format!("{mnemonic}{cond}");
            }
            ScaKind::Arith => {}
        }
        let temp = bits(hw[3], 7, 5);
        let result = bit(hw[3], 12);
        let dest = bits(hw[3], 2, 5);
        let mask = [bit(hw[3], 20), bit(hw[3], 19), bit(hw[3], 18), bit(hw[3], 17)];
        let reg = if result && dest != 0x1F {
            if temp != 0x1F {
                unknown.push(format!("w3:{:#010x}", hw[3] & (0x1F << 7)));
            }
            vp_output(dest)
        } else if temp != 0x1F {
            format!("R{temp}")
        } else if owns_flags && self.cc_update() {
            "RC".to_string()
        } else {
            "-".to_string()
        };
        let d = dest_text(&reg, mask);
        format!("{mnemonic} {d}{}, {}", self.predicate(owns_flags), self.src(2))
    }
}

fn vp_insn(hw: [u32; 4]) -> Option<String> {
    let (vname, vsrcs) = vp_vec_op(bits(hw[1], 22, 5))?;
    let (sname, skind) = vp_sca_op(bits(hw[1], 27, 5))?;
    let vp = Vp { hw };
    let mut unknown = Vec::new();
    // hw[0] bits 28 and 31 are defined nowhere; the condition-update enable is the PAIR of bits 14 and 29.
    let mut u0 = hw[0] & ((1 << 28) | (1 << 31));
    if bit(hw[0], 14) != bit(hw[0], 29) {
        u0 |= hw[0] & ((1 << 14) | (1 << 29));
    }
    if u0 != 0 {
        unknown.push(format!("w0:{u0:#010x}"));
    }
    if bit(hw[1], 21) {
        unknown.push(format!("w1:{:#010x}", 1u32 << 21));
    }
    let vec_on = vname != "NOP";
    let sca_on = sname != "NOP";
    let mut parts = Vec::new();
    if vec_on {
        parts.push(vp.vec_text(vname, vsrcs, &mut unknown));
    }
    if sca_on {
        parts.push(vp.sca_text(sname, skind, !vec_on, &mut unknown));
    }
    if parts.is_empty() {
        parts.push("NOP".to_string());
    }
    Some(with_modifiers(parts.join(" + "), &[], &unknown, bit(hw[3], 0)))
}

/// Disassemble a vertex program: 128-bit instructions of four words, walked until the instruction whose hw[3]
/// bit 0 (NVFX_VP_INST_LAST) is set, at most [`VP_MAX_INSTRUCTIONS`] (the walk `rsx_frame::vp_hash` makes).
pub fn disasm_vp(words: &[u32]) -> Vec<Line> {
    let mut out = Vec::new();
    let mut n = 0usize;
    while (n + 1) * 4 <= words.len() && n < VP_MAX_INSTRUCTIONS {
        let hw = [words[4 * n], words[4 * n + 1], words[4 * n + 2], words[4 * n + 3]];
        let last = bit(hw[3], 0);
        let (text, kind) = match vp_insn(hw) {
            Some(t) => (t, LineKind::Instruction),
            None => {
                let why = format!(
                    "unknown vertex opcode (vector {:#04x}, scalar {:#04x})",
                    bits(hw[1], 22, 5),
                    bits(hw[1], 27, 5)
                );
                (with_modifiers(raw_text(&hw, &why), &[], &[], last), LineKind::Unknown)
            }
        };
        out.push(Line { offset: n * 16, words: hw.to_vec(), text, kind });
        n += 1;
        if last {
            return out;
        }
    }
    let rest = &words[(n * 4).min(words.len())..];
    if n < VP_MAX_INSTRUCTIONS && !rest.is_empty() {
        out.push(Line {
            offset: n * 16,
            words: rest.to_vec(),
            text: raw_text(rest, &format!("{} trailing word(s), not a whole instruction", rest.len())),
            kind: LineKind::Unknown,
        });
    }
    let why = if n >= VP_MAX_INSTRUCTIONS {
        format!("no end bit within {VP_MAX_INSTRUCTIONS} instructions")
    } else {
        "no end bit: the program is unterminated".to_string()
    };
    out.push(Line { offset: n * 16, words: Vec::new(), text: why, kind: LineKind::Note });
    out
}

// ---- fragment programs ----------------------------------------------------------------------------------------------

#[derive(Clone, Copy, PartialEq, Eq)]
enum FpKind {
    /// Destination, sources.
    Arith,
    /// Destination, sources, then the texture unit (hw[0] bits 17..20).
    Tex,
    /// KIL: no operand, the condition test only.
    Kil,
    /// NOP, FENCBR, FENCTR: nothing to print.
    Bare,
}

/// A fragment opcode (hw[0] bits 24..29; NVFX_FP_OP_OPCODE_*, plus FENCBR 0x3E / FENCTR 0x3D from
/// FpAssembler::emitFencbr/emitFenctr) and how many source slots it reads.  NV30-only opcodes are not RSX
/// encodings our encoder defines and stay unknown.
fn fp_op(op: u32) -> Option<(&'static str, usize, FpKind)> {
    use FpKind::*;
    Some(match op {
        0x00 => ("NOP", 0, Bare),
        0x01 => ("MOV", 1, Arith),
        0x02 => ("MUL", 2, Arith),
        0x03 => ("ADD", 2, Arith),
        0x04 => ("MAD", 3, Arith),
        0x05 => ("DP3", 2, Arith),
        0x06 => ("DP4", 2, Arith),
        0x07 => ("DST", 2, Arith),
        0x08 => ("MIN", 2, Arith),
        0x09 => ("MAX", 2, Arith),
        0x0A => ("SLT", 2, Arith),
        0x0B => ("SGE", 2, Arith),
        0x0C => ("SLE", 2, Arith),
        0x0D => ("SGT", 2, Arith),
        0x0E => ("SNE", 2, Arith),
        0x0F => ("SEQ", 2, Arith),
        0x10 => ("FRC", 1, Arith),
        0x11 => ("FLR", 1, Arith),
        0x12 => ("KIL", 0, Kil),
        0x13 => ("PK4B", 1, Arith),
        0x14 => ("UP4B", 1, Arith),
        0x15 => ("DDX", 1, Arith),
        0x16 => ("DDY", 1, Arith),
        0x17 => ("TEX", 1, Tex),
        0x18 => ("TXP", 1, Tex),
        0x19 => ("TXD", 3, Tex),
        0x1A => ("RCP", 1, Arith),
        0x1B => ("RSQ", 1, Arith),
        0x1C => ("EX2", 1, Arith),
        0x1D => ("LG2", 1, Arith),
        0x20 => ("STR", 2, Arith),
        0x21 => ("SFL", 2, Arith),
        0x22 => ("COS", 1, Arith),
        0x23 => ("SIN", 1, Arith),
        0x24 => ("PK2H", 1, Arith),
        0x25 => ("UP2H", 1, Arith),
        0x27 => ("PK4UB", 1, Arith),
        0x28 => ("UP4UB", 1, Arith),
        0x29 => ("PK2US", 1, Arith),
        0x2A => ("UP2US", 1, Arith),
        0x2E => ("DP2A", 3, Arith),
        // TXL: the coordinate, then the LOD in lane x of the second source.
        0x2F => ("TXL", 2, Tex),
        0x31 => ("TXB", 1, Tex),
        0x38 => ("DP2", 2, Arith),
        0x39 => ("NRM", 1, Arith),
        // DIV / DIVSQ: a vector numerator, the divisor in lane x of the second source.
        0x3A => ("DIV", 2, Arith),
        0x3B => ("DIVSQ", 2, Arith),
        0x3C => ("LITEX2", 1, Arith),
        0x3D => ("FENCTR", 0, Bare),
        0x3E => ("FENCBR", 0, Bare),
        _ => return None,
    })
}

/// NVFX_FP_OP_INPUT_SRC_* (hw[0] bits 13..16): one input selector for the whole instruction.
fn fp_input(sel: u32) -> String {
    match sel {
        0x0 => "f[WPOS]".into(),
        0x1 => "f[COL0]".into(),
        0x2 => "f[COL1]".into(),
        0x3 => "f[FOGC]".into(),
        0x4..=0xD => format!("f[TEX{}]", sel - 4),
        0xE => "f[FACING]".into(),
        _ => format!("{UNKNOWN_MARK}f[{sel}]"),
    }
}

fn fp_cond(hw1: u32) -> String {
    let sel = [bits(hw1, 21, 2), bits(hw1, 23, 2), bits(hw1, 25, 2), bits(hw1, 27, 2)];
    format!("({}{})", COND[bits(hw1, 18, 3) as usize], swizzle(sel))
}

fn fp_predicate(hw1: u32) -> String {
    if bits(hw1, 18, 3) != COND_TR {
        format!(" {}", fp_cond(hw1))
    } else {
        String::new()
    }
}

/// Byte offset of a dword index the branch encodings carry (absolute, from the program start).
fn fp_target(dwords: u32) -> String {
    format!("@{:#x}", u64::from(dwords) * 4)
}

/// A branch instruction: hw[2] bit 31 (NV40_FP_OP_OPCODE_IS_BRANCH) set, NV40_FP_OP_BRA_OPCODE_* in the opcode
/// field.  LOOP and BRK are the shapes our lowering emits; IF/CAL/REP/RET are laid out by nvfx_shader.h.
fn fp_branch(hw: [u32; 4]) -> Option<String> {
    let pred = fp_predicate(hw[1]);
    let end = fp_target(bits(hw[3], 0, 31));
    Some(match bits(hw[0], 24, 6) {
        0x0 => format!("BRK{pred}"),
        0x1 => format!("CAL{pred} {}", fp_target(bits(hw[2], 0, 31))),
        0x2 => format!("IF{pred} else={} end={end}", fp_target(bits(hw[2], 0, 31))),
        0x3 => format!(
            "LOOP{pred} count={} init={} step={} end={end}",
            bits(hw[2], 2, 8),
            bits(hw[2], 10, 8),
            bits(hw[2], 19, 8)
        ),
        0x4 => {
            let (a, b, c) = (bits(hw[2], 2, 8), bits(hw[2], 10, 8), bits(hw[2], 19, 8));
            if a == b && b == c {
                format!("REP{pred} count={a} end={end}")
            } else {
                format!("REP{pred} count={a},{b},{c} end={end}")
            }
        }
        0x5 => format!("RET{pred}"),
        _ => return None,
    })
}

/// One fragment source slot (hw[1 + slot]): type bits 0..1 (0 temp, 1 input, 2 const), register 2..7, half 8,
/// swizzle 9..16 (x first), negate 17; abs is hw[1] bit 29, hw[2] bit 18, hw[3] bit 18 for slots 0, 1, 2.
fn fp_src(hw: &[u32; 4], slot: usize, unknown: &mut Vec<String>) -> String {
    let f = hw[1 + slot];
    let name = match f & 3 {
        0 => format!("{}{}", if bit(f, 8) { "H" } else { "R" }, bits(f, 2, 6)),
        1 => {
            if bit(f, 8) {
                unknown.push(format!("w{}:{:#010x}", 1 + slot, 1u32 << 8));
            }
            fp_input(bits(hw[0], 13, 4))
        }
        2 => format!("c{}", bits(f, 2, 6)),
        _ => format!("{UNKNOWN_MARK}type3"),
    };
    let swz = swizzle([bits(f, 9, 2), bits(f, 11, 2), bits(f, 13, 2), bits(f, 15, 2)]);
    let abs = match slot {
        0 => bit(hw[1], 29),
        1 => bit(hw[2], 18),
        _ => bit(hw[3], 18),
    };
    source_mods(name, swz, bit(f, 17), abs)
}

fn fp_insn(hw: [u32; 4]) -> Option<String> {
    let end = bit(hw[0], 0);
    if bit(hw[2], 31) {
        return fp_branch(hw).map(|t| with_modifiers(t, &[], &[], end));
    }
    let (name, nsrc, kind) = fp_op(bits(hw[0], 24, 6))?;
    let mut unknown = Vec::new();
    let mut mods = Vec::new();
    let text = match kind {
        FpKind::Bare => name.to_string(),
        FpKind::Kil => format!("KIL{}", fp_predicate(hw[1])),
        FpKind::Arith | FpKind::Tex => {
            let prec = bits(hw[0], 22, 2);
            let mut m = name.to_string();
            match prec {
                0 => m.push('R'),
                1 => m.push('H'),
                2 => m.push('X'),
                _ => unknown.push(format!("w0:{:#010x}", hw[0] & (3 << 22))),
            }
            if bit(hw[0], 8) {
                m.push('C');
            }
            match bits(hw[2], 28, 3) {
                0 => {}
                1 => m.push_str("_x2"),
                2 => m.push_str("_x4"),
                3 => m.push_str("_x8"),
                5 => m.push_str("_d2"),
                6 => m.push_str("_d4"),
                7 => m.push_str("_d8"),
                _ => unknown.push(format!("w2:{:#010x}", hw[2] & (7 << 28))),
            }
            if bit(hw[0], 31) {
                m.push_str("_SAT");
            }
            let mask = [bit(hw[0], 9), bit(hw[0], 10), bit(hw[0], 11), bit(hw[0], 12)];
            let reg = if bit(hw[0], 30) {
                // NV40_FP_OP_OUT_NONE: the register field is a sentinel; only the condition register is written.
                if bit(hw[0], 8) { "RC".to_string() } else { "-".to_string() }
            } else {
                format!("{}{}", if bit(hw[0], 7) { "H" } else { "R" }, bits(hw[0], 1, 6))
            };
            let dest = if reg == "-" { reg } else { dest_text(&reg, mask) };
            let mut ops = vec![format!("{dest}{}", fp_predicate(hw[1]))];
            for s in 0..nsrc {
                ops.push(fp_src(&hw, s, &mut unknown));
            }
            if kind == FpKind::Tex {
                ops.push(format!("TEX{}", bits(hw[0], 17, 4)));
            }
            format!("{m} {}", ops.join(", "))
        }
    };
    let uses_input = (0..nsrc).any(|s| hw[1 + s] & 3 == 1);
    // Fields no instruction of this shape defines.
    let mut u0 = hw[0] & (1 << 21);
    if kind != FpKind::Tex {
        u0 |= hw[0] & (0xF << 17);
    }
    if !uses_input {
        u0 |= hw[0] & (0xF << 13);
    }
    if u0 != 0 {
        unknown.push(format!("w0:{u0:#010x}"));
    }
    if hw[1] & (3 << 30) != 0 {
        unknown.push(format!("w1:{:#010x}", hw[1] & (3 << 30)));
    }
    // hw[2] bits 19..27: the encoder writes 0x168 here for an fx12 comparison (input precision 1 at 19..20 and
    // a constant at 21..27) without naming the fields, so they print raw.
    if hw[2] & (0x1FF << 19) != 0 {
        unknown.push(format!("w2:{:#010x}", hw[2] & (0x1FF << 19)));
    }
    // hw[3] bits 19..29: 0x7fc whenever a varying is read (the encoder's default), else 0.
    let disp = bits(hw[3], 19, 11);
    if disp != 0 && disp != 0x7FC {
        unknown.push(format!("w3:{:#010x}", hw[3] & (0x7FF << 19)));
    }
    if bit(hw[3], 30) {
        mods.push("{index_input}".to_string());
    }
    if bit(hw[3], 31) {
        mods.push("{disable_pc}".to_string());
    }
    Some(with_modifiers(text, &mods, &unknown, end))
}

fn be_words(bytes: &[u8], at: usize) -> [u32; 4] {
    let w = |i: usize| u32::from_be_bytes([bytes[at + i], bytes[at + i + 1], bytes[at + i + 2], bytes[at + i + 3]]);
    [w(0), w(4), w(8), w(12)]
}

fn fp_const_text(logical: [u32; 4]) -> String {
    let v: Vec<String> = logical.iter().map(|&w| format!("{:?}", f32::from_bits(w))).collect();
    format!(".const {}", v.join(", "))
}

/// Disassemble a fragment program from its bytes as they lie in memory (big-endian words with their halfwords
/// swapped, `FpAssembler::words`).  The walk is `rsx_frame::fp_hash`'s: an instruction any of whose three source
/// slots has type 2 (const) is followed by one 16-byte constant block, and the instruction with PROGRAM_END
/// (hw[0] bit 0) is the last; at most [`FP_MAX_INSTRUCTIONS`].
pub fn disasm_fp(bytes: &[u8]) -> Vec<Line> {
    let swap = |w: u32| w.rotate_left(16);
    let mut out = Vec::new();
    let (mut pos, mut n) = (0usize, 0usize);
    while pos + 16 <= bytes.len() && n < FP_MAX_INSTRUCTIONS {
        let raw = be_words(bytes, pos);
        let hw = raw.map(swap);
        let end = bit(hw[0], 0);
        let (text, kind) = match fp_insn(hw) {
            Some(t) => (t, LineKind::Instruction),
            None => {
                let why = format!("unknown fragment opcode {:#04x}", bits(hw[0], 24, 6));
                (with_modifiers(raw_text(&raw, &why), &[], &[], end), LineKind::Unknown)
            }
        };
        out.push(Line { offset: pos, words: raw.to_vec(), text, kind });
        pos += 16;
        n += 1;
        if (1..4).any(|k| hw[k] & 3 == 2) {
            if pos + 16 <= bytes.len() {
                let c = be_words(bytes, pos);
                let text = fp_const_text(c.map(swap));
                out.push(Line { offset: pos, words: c.to_vec(), text, kind: LineKind::Constant });
                pos += 16;
            } else {
                let rest: Vec<u32> = bytes[pos..]
                    .chunks_exact(4)
                    .map(|c| u32::from_be_bytes([c[0], c[1], c[2], c[3]]))
                    .collect();
                let text = raw_text(&rest, &format!("constant block truncated to {} byte(s)", bytes.len() - pos));
                out.push(Line { offset: pos, words: rest, text, kind: LineKind::Unknown });
                pos = bytes.len();
            }
        }
        if end {
            return out;
        }
    }
    if pos < bytes.len() && n < FP_MAX_INSTRUCTIONS {
        let rest: Vec<u32> =
            bytes[pos..].chunks_exact(4).map(|c| u32::from_be_bytes([c[0], c[1], c[2], c[3]])).collect();
        let text = raw_text(&rest, &format!("{} trailing byte(s), not a whole instruction", bytes.len() - pos));
        out.push(Line { offset: pos, words: rest, text, kind: LineKind::Unknown });
    }
    let why = if n >= FP_MAX_INSTRUCTIONS {
        format!("no end bit within {FP_MAX_INSTRUCTIONS} instructions")
    } else {
        "no end bit: the program is unterminated".to_string()
    };
    out.push(Line { offset: pos, words: Vec::new(), text: why, kind: LineKind::Note });
    out
}

// ---- static texture fetch counts -----------------------------------------------------------------------------------

/// The texture-fetch opcodes, in the order [`TexFetches::by_op`] counts them.  A vertex program fetches with TXL only.
pub const TEX_OPS: [&str; 5] = ["TEX", "TXP", "TXL", "TXB", "TXD"];

/// The texture fetches a program's ucode contains, counted statically: one per fetch instruction in the program as
/// stored, whatever its branches and loops do at run time (a fetch inside a loop counts once; one behind a branch not
/// taken counts too).  The walk is the disassembler's (and the canonical hash's): the same instructions, the same
/// constant blocks skipped, the same end.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Default)]
pub struct TexFetches {
    /// Fetch instructions per opcode, in [`TEX_OPS`] order.
    pub by_op: [u32; 5],
    /// Fetch instructions per texture unit: fragment units 0..15, vertex units 0..3.
    pub by_unit: [u32; 16],
    /// Instructions walked (constant blocks excluded).
    pub instructions: u32,
    /// Instructions whose opcode the disassembler does not know: a fetch among them is not counted, so with any the
    /// counts are a lower bound.
    pub unknown: u32,
    /// The walk ended on the program's end bit (false: unterminated or cut short, the counts are a lower bound).
    pub terminated: bool,
}
impl TexFetches {
    pub fn total(&self) -> u32 {
        self.by_op.iter().sum()
    }
    /// Every instruction was decoded through the end bit: the counts are exact.
    pub fn exact(&self) -> bool {
        self.terminated && self.unknown == 0
    }
    /// `TEX 2 TXP 0 TXL 1 TXB 0 TXD 0`-style text of the nonzero opcodes (`none` when there is no fetch).
    pub fn ops_text(&self) -> String {
        let v: Vec<String> =
            TEX_OPS.iter().zip(self.by_op).filter(|(_, n)| *n > 0).map(|(o, n)| format!("{o} {n}")).collect();
        if v.is_empty() { "none".into() } else { v.join(" ") }
    }
    /// The units fetched from, ascending.
    pub fn units(&self) -> Vec<u32> {
        (0..16).filter(|&u| self.by_unit[u as usize] > 0).collect()
    }
}

/// The static texture fetches of a vertex program (`disasm_vp`'s input): TXL in the vector slot, unit hw[2] bits 8..9.
pub fn vp_tex_fetches(words: &[u32]) -> TexFetches {
    let mut t = TexFetches::default();
    let mut n = 0usize;
    while (n + 1) * 4 <= words.len() && n < VP_MAX_INSTRUCTIONS {
        let hw = [words[4 * n], words[4 * n + 1], words[4 * n + 2], words[4 * n + 3]];
        t.instructions += 1;
        n += 1;
        let vec = bits(hw[1], 22, 5);
        if vp_vec_op(vec).is_none() || vp_sca_op(bits(hw[1], 27, 5)).is_none() {
            t.unknown += 1;
        } else if vec == 0x19 {
            t.by_op[2] += 1;
            t.by_unit[bits(hw[2], 8, 2) as usize] += 1;
        }
        if bit(hw[3], 0) {
            t.terminated = true;
            break;
        }
    }
    t
}

/// The static texture fetches of a fragment program (`disasm_fp`'s input: the bytes as they lie in memory): TEX,
/// TXP, TXL, TXB and TXD, unit hw[0] bits 17..20.  Branch instructions (hw[2] bit 31) fetch nothing.
pub fn fp_tex_fetches(bytes: &[u8]) -> TexFetches {
    let swap = |w: u32| w.rotate_left(16);
    let mut t = TexFetches::default();
    let (mut pos, mut n) = (0usize, 0usize);
    while pos + 16 <= bytes.len() && n < FP_MAX_INSTRUCTIONS {
        let hw = be_words(bytes, pos).map(swap);
        t.instructions += 1;
        pos += 16;
        n += 1;
        if bit(hw[2], 31) {
            if fp_branch(hw).is_none() {
                t.unknown += 1;
            }
        } else {
            match fp_op(bits(hw[0], 24, 6)) {
                None => t.unknown += 1,
                Some((name, _, FpKind::Tex)) => {
                    let k = TEX_OPS.iter().position(|o| *o == name).expect("a fetch opcode");
                    t.by_op[k] += 1;
                    t.by_unit[bits(hw[0], 17, 4) as usize] += 1;
                }
                Some(_) => {}
            }
        }
        if (1..4).any(|k| hw[k] & 3 == 2) {
            pos = (pos + 16).min(bytes.len());
        }
        if bit(hw[0], 0) {
            t.terminated = true;
            break;
        }
    }
    t
}
