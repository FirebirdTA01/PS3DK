//! NV40 vertex/fragment microcode disassembler (src/nv40_disasm.rs).
//!
//! - The fixture corpus (tests/fixtures/nv40/*.txt) is our shader compiler's own output: each file holds the word
//!   dump `rsx-cg-compiler -p sce_{vp,fp}_rsx` prints (`w` lines) and the listing of the SAME program by the
//!   repository's independent decoders, tests/shader-compiler/vp_words.py and fp_sources.py, run on the container
//!   (`r` lines).  Every program must disassemble with no unknown line, cover its words exactly, end on the end
//!   bit, and agree with the independent listing on opcode, destination, write mask, precision, saturate, end and
//!   sources.
//! - Exact listings of three whole programs (a vertex loop with BRA and relative addressing, a fragment hardware
//!   loop with BRK and constant blocks, a vertex texture fetch).
//! - One unit test per opcode family, and the unknown-encoding, truncated and unterminated shapes.
use ps3dk_prof::nv40_disasm::{disasm_fp, disasm_vp, Line, LineKind, END_MARK, UNKNOWN_MARK};

// ---- helpers --------------------------------------------------------------------------------------------------------

/// Fragment words in LOGICAL layout -> the bytes as stored (halfword-swapped, big-endian).
fn fp_bytes(insns: &[[u32; 4]]) -> Vec<u8> {
    insns.iter().flatten().flat_map(|w| w.rotate_left(16).to_be_bytes()).collect()
}

fn texts(lines: &[Line]) -> Vec<&str> {
    lines.iter().map(|l| l.text.as_str()).collect()
}

/// Build a vertex instruction from its three 17-bit source fields and the remaining bits of each word.
fn vp(s0: u32, s1: u32, s2: u32, h0: u32, h1: u32, h3: u32) -> [u32; 4] {
    [h0, h1 | (s0 >> 9), ((s0 & 0x1FF) << 23) | (s1 << 6) | (s2 >> 11), ((s2 & 0x7FF) << 21) | h3]
}

const PAD: u32 = 0xE4 << 9; // fragment source: TEMP R0, identity swizzle (an unused slot)
const TRI: u32 = (7 << 18) | (0xE4 << 21); // fragment condition: TR, identity swizzle
const VIN: u32 = 0x1B02; // vertex source: input, identity swizzle
const CC: u32 = (7 << 10) | 0x6C; // vertex condition: TR, identity swizzle
const NOSCA: u32 = (0x1F << 7) | (1 << 12); // vertex hw[3]: no scalar temp

// ---- the fixture corpus ---------------------------------------------------------------------------------------------

struct Fixture {
    name: String,
    profile: String,
    words: Vec<u32>,
    refs: Vec<String>,
}

fn fixtures() -> Vec<Fixture> {
    let dir = std::path::Path::new(env!("CARGO_MANIFEST_DIR")).join("tests/fixtures/nv40");
    let mut paths: Vec<_> = std::fs::read_dir(&dir)
        .unwrap()
        .map(|e| e.unwrap().path())
        .filter(|p| p.extension().is_some_and(|x| x == "txt"))
        .collect();
    paths.sort();
    paths
        .iter()
        .map(|p| {
            let text = std::fs::read_to_string(p).unwrap();
            let mut f = Fixture {
                name: p.file_name().unwrap().to_string_lossy().into_owned(),
                profile: String::new(),
                words: Vec::new(),
                refs: Vec::new(),
            };
            for line in text.lines() {
                if let Some(v) = line.strip_prefix("profile ") {
                    f.profile = v.trim().to_string();
                } else if let Some(v) = line.strip_prefix("w ") {
                    f.words.extend(v.split_whitespace().map(|h| u32::from_str_radix(h, 16).unwrap()));
                } else if let Some(v) = line.strip_prefix("r ") {
                    f.refs.push(v.to_string());
                }
            }
            assert!(f.profile == "vp" || f.profile == "fp", "{}: profile {:?}", f.name, f.profile);
            assert!(!f.words.is_empty() && !f.refs.is_empty(), "{}: empty fixture", f.name);
            f
        })
        .collect()
}

/// An independent-listing line: `<n> <OP> key=value ... <source>=<text>`.
struct Ref {
    op: String,
    fields: std::collections::HashMap<String, String>,
    srcs: Vec<String>,
}

fn parse_ref(r: &str) -> Ref {
    let mut it = r.split(' ');
    it.next();
    let op = it.next().unwrap().to_string();
    let mut fields = std::collections::HashMap::new();
    let mut srcs = Vec::new();
    for kv in it {
        let (k, v) = kv.split_once('=').unwrap();
        let numbered = k.len() > 1 && k.starts_with('s') && k[1..].chars().all(|c| c.is_ascii_digit());
        if numbered || k.starts_with("src") {
            srcs.push(v.to_string());
        } else {
            fields.insert(k.to_string(), v.to_string());
        }
    }
    Ref { op, fields, srcs }
}

/// The instruction part of a line (modifiers follow two spaces), split into mnemonic and operands.
fn split_ops(text: &str) -> (String, Vec<String>) {
    let main = text.split("  ").next().unwrap();
    match main.split_once(' ') {
        Some((m, ops)) => (m.to_string(), ops.split(", ").map(str::to_string).collect()),
        None => (main.to_string(), Vec::new()),
    }
}

fn strip_pred(d: &str) -> &str {
    d.split(" (").next().unwrap()
}

/// One of our source operands in the independent listing's spelling.
fn norm_src(s: &str, vertex: bool) -> String {
    let (neg, s) = match s.strip_prefix('-') {
        Some(r) => (true, r),
        None => (false, s),
    };
    let (mut abs, s) = match s.strip_prefix('|').and_then(|r| r.strip_suffix('|')) {
        Some(r) => (true, r),
        None => (false, s),
    };
    let (mut base, swz) = match s.rfind('.') {
        Some(i) if s.len() - i == 5 && !s[i..].contains(']') && s[i + 1..].chars().all(|c| "xyzw".contains(c)) => {
            (s[..i].to_string(), s[i..].to_string())
        }
        _ => (s.to_string(), ".xyzw".to_string()),
    };
    if vertex {
        if base.starts_with('v') && base[1..].chars().all(|c| c.is_ascii_digit()) {
            base = format!("IN{}", &base[1..]);
        } else if let Some(r) = base.strip_prefix('c') {
            base = format!("C{r}");
        }
        abs = false; // vp_words.py does not render abs
    } else if let Some(r) = base.strip_prefix("f[").and_then(|r| r.strip_suffix(']')) {
        base = r.to_string();
    }
    let body = format!("{base}{swz}");
    let body = if abs { format!("|{body}|") } else { body };
    if neg {
        format!("-{body}")
    } else {
        body
    }
}

/// Our vertex destination as (register, mask) in vp_words.py's spelling: `o<n>`, RC as R63, all lanes `xyzw`.
fn vp_dst(d: &str) -> (String, String) {
    let (reg, mask) = d.split_once('.').unwrap_or((d, ""));
    let reg = if let Some(n) = reg.strip_prefix("o[").and_then(|r| r.strip_suffix(']')) {
        let idx = match n {
            "HPOS" => 0,
            "COL0" => 1,
            "COL1" => 2,
            "BFC0" => 3,
            "BFC1" => 4,
            "FOGC" => 5,
            "PSZ/TEX9" => 6,
            _ => match n.strip_prefix("TEX") {
                Some(t) => t.parse::<u32>().unwrap() + 7,
                None => n.parse::<u32>().unwrap(),
            },
        };
        format!("o{idx}")
    } else if reg == "RC" {
        "R63".to_string()
    } else {
        reg.to_string()
    };
    (reg, if mask.is_empty() { "xyzw".to_string() } else { mask.to_string() })
}

/// vp_words.py prints all three source fields; which ones a vector opcode reads.
fn vp_positions(op: &str) -> &'static [usize] {
    match op {
        "ADD" => &[0, 2],
        "MAD" => &[0, 1, 2],
        "MOV" | "ARL" | "FRC" | "FLR" | "TXL" | "SSG" | "ARR" | "ARA" => &[0],
        _ => &[0, 1],
    }
}

fn check_fixture(f: &Fixture) -> Vec<String> {
    let mut errs = Vec::new();
    let lines = if f.profile == "vp" {
        disasm_vp(&f.words)
    } else {
        let bytes: Vec<u8> = f.words.iter().flat_map(|w| w.to_be_bytes()).collect();
        disasm_fp(&bytes)
    };
    if lines.iter().any(|l| matches!(l.kind, LineKind::Unknown | LineKind::Note) || l.text.contains(UNKNOWN_MARK)) {
        errs.push("an unknown or note line".to_string());
    }
    let covered: Vec<u32> = lines.iter().flat_map(|l| l.words.iter().copied()).collect();
    if covered != f.words {
        errs.push("the lines do not cover the program's words exactly".to_string());
    }
    let mut offset = 0;
    for l in &lines {
        if l.offset != offset {
            errs.push(format!("line at {:#x}, expected {offset:#x}", l.offset));
        }
        offset += 4 * l.words.len();
    }
    let ins: Vec<&Line> = lines.iter().filter(|l| l.kind == LineKind::Instruction).collect();
    if !ins.last().is_some_and(|l| l.text.ends_with(END_MARK)) {
        errs.push("the last instruction does not carry the end mark".to_string());
    }
    if ins.iter().rev().skip(1).any(|l| l.text.ends_with(END_MARK)) {
        errs.push("an end mark before the last instruction".to_string());
    }
    if ins.len() != f.refs.len() {
        errs.push(format!("{} instructions, the independent listing has {}", ins.len(), f.refs.len()));
        return errs;
    }
    for (l, r) in ins.iter().zip(&f.refs) {
        let r = parse_ref(r);
        let at = l.offset;
        if f.profile == "vp" {
            if (l.words[1] >> 22) & 0x1F == 0 {
                continue; // vp_words.py decodes the vector slot only
            }
            let vec = l.text.split("  ").next().unwrap().split(" + ").next().unwrap();
            let (m, ops) = split_ops(vec);
            if !m.starts_with(&r.op) {
                errs.push(format!("{at:#x}: opcode {m} vs {}", r.op));
                continue;
            }
            let d = strip_pred(&ops[0]);
            let (rdst, rmask) = (&r.fields["dst"], &r.fields["mask"]);
            if d == "-" {
                if !(rdst == "R63" || rmask == "-") {
                    errs.push(format!("{at:#x}: no destination vs {rdst}"));
                }
            } else if vp_dst(d) != (rdst.clone(), rmask.clone()) {
                errs.push(format!("{at:#x}: destination {d} vs {rdst} mask {rmask}"));
            }
            let pos = vp_positions(&r.op);
            let mine: Vec<String> = ops[1..].iter().take(pos.len()).map(|s| norm_src(s, true)).collect();
            let theirs: Vec<String> = pos.iter().map(|&p| r.srcs[p].clone()).collect();
            if mine != theirs {
                errs.push(format!("{at:#x}: sources {mine:?} vs {theirs:?}"));
            }
        } else {
            if (l.words[2].rotate_left(16) >> 31) & 1 != 0 {
                continue; // a branch: fp_sources.py does not decode IS_BRANCH
            }
            let (m, ops) = split_ops(&l.text);
            let no_dest = matches!(m.as_str(), "NOP" | "KIL" | "FENCBR" | "FENCTR")
                || m.starts_with("KIL")
                || m.starts_with("FENC");
            if !r.op.starts_with("op") {
                let want = if r.op == "DIVSQR" { "DIVSQ" } else { r.op.as_str() };
                if !m.starts_with(want) {
                    errs.push(format!("{at:#x}: opcode {m} vs {}", r.op));
                    continue;
                }
                if !no_dest {
                    let prec = ["R", "H", "X"][r.fields["prec"].parse::<usize>().unwrap()];
                    if &m[want.len()..want.len() + 1] != prec {
                        errs.push(format!("{at:#x}: precision of {m} vs prec={}", r.fields["prec"]));
                    }
                }
            }
            if m.contains("_SAT") != (r.fields["sat"] == "1") {
                errs.push(format!("{at:#x}: saturate of {m} vs sat={}", r.fields["sat"]));
            }
            if l.text.ends_with(END_MARK) != (r.fields["end"] == "1") {
                errs.push(format!("{at:#x}: end mark vs end={}", r.fields["end"]));
            }
            if no_dest {
                continue;
            }
            let d = strip_pred(&ops[0]);
            if r.fields["dst"] == "none" {
                if !(d.starts_with("RC") || d == "-") {
                    errs.push(format!("{at:#x}: destination {d} vs none"));
                }
            } else {
                let (reg, mask) = match d.split_once('.') {
                    Some((g, k)) => (g.to_string(), k.to_string()),
                    None if d == "-" => (String::new(), "-".to_string()),
                    None => (d.to_string(), "xyzw".to_string()),
                };
                if (reg.as_str(), mask.as_str()) != (r.fields["dst"].as_str(), r.fields["mask"].as_str()) {
                    errs.push(format!("{at:#x}: destination {d} vs {} mask {}", r.fields["dst"], r.fields["mask"]));
                }
            }
            if !r.srcs.is_empty() && r.srcs != ["UNKNOWN"] {
                let mine: Vec<String> = ops[1..]
                    .iter()
                    .filter(|o| !(o.starts_with("TEX") && o[3..].chars().all(|c| c.is_ascii_digit())))
                    .map(|s| norm_src(s, false))
                    .collect();
                if mine != r.srcs {
                    errs.push(format!("{at:#x}: sources {mine:?} vs {:?}", r.srcs));
                }
            }
        }
    }
    errs
}

#[test]
fn fixtures_agree_with_the_independent_listings() {
    let all = fixtures();
    assert!(all.len() >= 30, "{} fixtures", all.len());
    let failures: Vec<String> = all
        .iter()
        .filter_map(|f| {
            let e = check_fixture(f);
            (!e.is_empty()).then(|| format!("{}: {}", f.name, e.join("; ")))
        })
        .collect();
    assert!(failures.is_empty(), "{}", failures.join("\n"));
}

#[test]
fn fixture_corpus_covers_the_emitted_opcodes() {
    // Every fragment opcode, branch opcode and vertex vector/scalar opcode the corpus carries must decode under its
    // encoder name (a mnemonic matches with its suffixes: SLTC is SLT); the set is pinned so a fixture regeneration
    // that loses coverage is noticed.
    let mut seen = std::collections::BTreeSet::new();
    for f in fixtures() {
        if f.profile == "vp" {
            for l in disasm_vp(&f.words) {
                for part in l.text.split("  ").next().unwrap().split(" + ") {
                    seen.insert(format!("vp:{}", part.split(' ').next().unwrap()));
                }
            }
        } else {
            let bytes: Vec<u8> = f.words.iter().flat_map(|w| w.to_be_bytes()).collect();
            for l in disasm_fp(&bytes).iter().filter(|l| l.kind == LineKind::Instruction) {
                seen.insert(format!("fp:{}", l.text.split(' ').next().unwrap()));
            }
        }
    }
    for want in [
        "vp:MOV", "vp:MUL", "vp:ADD", "vp:MAD", "vp:DP3", "vp:DP4", "vp:MIN", "vp:MAX", "vp:SLT", "vp:SGT", "vp:ARL",
        "vp:SFL", "vp:TXL", "vp:RCP", "vp:RSQ", "vp:LIT", "vp:LG2", "vp:EX2", "vp:SIN", "vp:COS", "vp:BRA",
        "fp:NOP", "fp:FENCBR", "fp:FENCTR", "fp:KIL", "fp:BRK", "fp:LOOP", "fp:TEXR", "fp:TXPR", "fp:TXBR", "fp:TXLR",
        "fp:DIVR", "fp:DIVSQR", "fp:LITEX2R", "fp:DDXR", "fp:DDYR", "fp:PK4BR", "fp:UP4BR", "fp:PK2HR", "fp:UP2HR",
        "fp:PK4UBR", "fp:UP4UBR", "fp:PK2USR", "fp:UP2USR", "fp:DP2R", "fp:DP3R", "fp:DP4R", "fp:FRCR", "fp:FLRR",
        "fp:RCPR", "fp:RSQR", "fp:EX2R", "fp:LG2R", "fp:SINR", "fp:COSR",
    ] {
        assert!(seen.iter().any(|s| s.starts_with(want)), "{want} missing from the decoded corpus: {seen:?}");
    }
}

// ---- whole programs -----------------------------------------------------------------------------------------------

fn fixture(name: &str) -> Fixture {
    fixtures().into_iter().find(|f| f.name == name).unwrap_or_else(|| panic!("fixture {name}"))
}

#[test]
fn vertex_loop_with_branches_and_relative_constants() {
    let f = fixture("vp_loop_sum_v.txt");
    let lines = disasm_vp(&f.words);
    assert_eq!(
        texts(&lines),
        [
            "MOV R0, v0",
            "MOV R1.x, c458.xxxx",
            "SLTC RC.x, R1.xxxx, c467.xxxx",
            "BRA (EQ.xxxx) @10",
            "ADD R2.x, R1.xxxx, c458.yyyy",
            "ARL A0.x, R1.xxxx",
            "ADD R3, R0, c[A0.x+459]",
            "MOV R1.x, R2.xxxx",
            "MOV R0, R3",
            "BRA @2",
            "MOV o[HPOS], R0  [end]",
        ]
    );
    assert!(lines.iter().enumerate().all(|(i, l)| l.offset == 16 * i && l.kind == LineKind::Instruction));
    assert_eq!(lines[3].words, [0x001f8800, 0x4800000d, 0x8106c081, 0x40401ffc]);
}

#[test]
fn fragment_hardware_loop_with_constant_blocks() {
    let f = fixture("fp_loop_tail_break_f.txt");
    let bytes: Vec<u8> = f.words.iter().flat_map(|w| w.to_be_bytes()).collect();
    let lines = disasm_fp(&bytes);
    let got: Vec<(usize, LineKind, &str)> = lines.iter().map(|l| (l.offset, l.kind, l.text.as_str())).collect();
    use LineKind::{Constant as K, Instruction as I};
    let zeros = ".const 0.0, 0.0, 0.0, 0.0";
    assert_eq!(
        got,
        [
            (0x000, I, "MOVR R3.x, c0.xxxx"),
            (0x010, K, zeros),
            (0x020, I, "MOVR R2.x, c0.xxxx"),
            (0x030, K, zeros),
            (0x040, I, "LOOP count=255 init=0 step=1 end=@0x110"),
            (0x050, I, "SLTRC RC.x, R2.xxxx, f[TEX0].xxxx"),
            (0x060, I, "BRK (EQ.xxxx)"),
            (0x070, I, "ADDR R1.x, R3.xxxx, f[TEX0].yyyy"),
            (0x080, I, "MOVR R3.x, R1.xxxx"),
            (0x090, I, "SGTR R1.x, R1.xxxx, f[TEX0].zzzz"),
            (0x0a0, I, "SNER R1.x, R1.xxxx, c0.xxxx"),
            (0x0b0, K, zeros),
            (0x0c0, I, "MOVRC RC.x, R1.xxxx"),
            (0x0d0, I, "BRK (NE.xxxx)"),
            (0x0e0, I, "ADDR R1.x, R2.xxxx, c0.xxxx"),
            (0x0f0, K, ".const 1.0, 0.0, 0.0, 0.0"),
            (0x100, I, "MOVR R2.x, R1.xxxx"),
            (0x110, I, "MOVR R0.x, R3.xxxx"),
            (0x120, I, "MOVR R0.y, R2.xxxx"),
            (0x130, I, "MOVR R0.z, c0.xxxx"),
            (0x140, K, zeros),
            (0x150, I, "MOVR R0.w, c0.xxxx  [end]"),
            (0x160, K, ".const 1.0, 0.0, 0.0, 0.0"),
        ]
    );
    // Line words are the stored (halfword-swapped) form, exactly the compiler's dump.
    assert_eq!(lines[4].words, [0x00004340, 0xc8001c9d, 0x03fc8008, 0x00440000]);
}

#[test]
fn vertex_texture_fetch() {
    // The listing vp-texture-fetch-test.sh records for these words.
    let f = fixture("vp_tex_tex2d_v.txt");
    assert_eq!(
        texts(&disasm_vp(&f.words)),
        ["MOV R0.xy, v8.xyxx", "SFL R0.z, v8.xxxx, v8.xxxx", "TXL R0, R0.xyxz, TEX0", "MOV o[HPOS], R0  [end]"]
    );
}

// ---- fragment opcode families ---------------------------------------------------------------------------------------

#[test]
fn fragment_arithmetic_modifiers() {
    // MAD, fp16, half destination, cc write, sat, 2x scale, NE predicate, negate+abs, an input and a half temp with
    // a reversed swizzle, disable_pc.
    let mad = [
        (4 << 24) | (2 << 1) | (1 << 7) | (7 << 9) | (4 << 13) | (1 << 22) | (1 << 31) | (1 << 8) | 1,
        (1 << 2) | (0xE4 << 9) | (1 << 17) | (1 << 29) | (5 << 18) | (0xE4 << 21),
        1 | (1 << 28),
        (3 << 2) | (1 << 8) | (0x1B << 9) | (0x7FC << 19) | (1 << 31),
    ];
    let lines = disasm_fp(&fp_bytes(&[mad]));
    assert_eq!(texts(&lines), ["MADHC_x2_SAT H2.xyz (NE), -|R1|, f[TEX0].xxxx, H3.wzyx  {disable_pc}  [end]"]);
    assert_eq!(lines[0].words, [0x8f858440, 0xc8043c97, 0x00011000, 0x370cbfe0]);

    let mul = [(2 << 24) | (1 << 1) | (0xF << 9) | 1, (2 << 2) | PAD | TRI, (3 << 2) | PAD | (5 << 28), PAD];
    assert_eq!(texts(&disasm_fp(&fp_bytes(&[mul]))), ["MULR_d2 R1, R2, R3  [end]"]);
}

#[test]
fn fragment_texture_kill_and_fences() {
    let tex = [(0x17 << 24) | (0xF << 9) | (4 << 13) | (3 << 17) | 1, 1 | PAD | TRI, PAD, PAD | (0x7FC << 19)];
    assert_eq!(texts(&disasm_fp(&fp_bytes(&[tex]))), ["TEXR R0, f[TEX0], TEX3  [end]"]);

    let program = [
        [(0x12 << 24) | (1 << 30) | (0x3F << 1) | (3 << 9), (2 << 18) | PAD, PAD, PAD],
        [(0x3E << 24) | (1 << 30) | (0x3F << 1) | (0xF << 9), TRI | PAD, PAD, PAD],
        [(0x3D << 24) | (1 << 30) | (0x3F << 1) | (0xF << 9), TRI | PAD, PAD, PAD],
        [1, 0, 0, 0],
    ];
    assert_eq!(texts(&disasm_fp(&fp_bytes(&program))), ["KIL (EQ.xxxx)", "FENCBR", "FENCTR", "NOP  [end]"]);
}

#[test]
fn fragment_constant_block_is_data() {
    let program = [
        [(1 << 24) | (0xF << 9) | 1, 2 | PAD | TRI, PAD, PAD],
        [0x3f800000, 0x3f000000, 0xc0000000, 0x3e800000],
    ];
    let lines = disasm_fp(&fp_bytes(&program));
    assert_eq!(texts(&lines), ["MOVR R0, c0  [end]", ".const 1.0, 0.5, -2.0, 0.25"]);
    assert_eq!((lines[1].offset, lines[1].kind), (16, LineKind::Constant));
    assert_eq!(lines[1].words, [0x00003f80, 0x00003f00, 0x0000c000, 0x00003e80]);
}

#[test]
fn fragment_branches() {
    let program = [
        [(2 << 24) | (1 << 30), 5 << 18, (1 << 31) | 12, 20],
        [(4 << 24) | (1 << 30), TRI, (1 << 31) | (3 << 2) | (3 << 10) | (3 << 19), 8],
        [(1 << 24) | (1 << 30), TRI, (1 << 31) | 16, 0],
        [(5 << 24) | (1 << 30) | 1, TRI, 1 << 31, 0],
    ];
    assert_eq!(
        texts(&disasm_fp(&fp_bytes(&program))),
        ["IF (NE.xxxx) else=@0x30 end=@0x50", "REP count=3 end=@0x20", "CAL @0x40", "RET  [end]"]
    );
}

#[test]
fn fragment_unknown_encodings() {
    let unknown_op = [(0x30 << 24) | 1, TRI | PAD, PAD, PAD];
    let lines = disasm_fp(&fp_bytes(&[unknown_op]));
    assert_eq!(lines.len(), 1);
    assert_eq!(lines[0].kind, LineKind::Unknown);
    assert_eq!(lines[0].text, "?? 00013000 c8001c9d c8000001 c8000001 ; unknown fragment opcode 0x30  [end]");

    let stray_bit = [(1 << 24) | (0xF << 9) | (1 << 21) | 1, (1 << 2) | PAD | TRI, PAD, PAD];
    assert_eq!(texts(&disasm_fp(&fp_bytes(&[stray_bit]))), ["MOVR R0, R1  ?bits(w0:0x00200000)  [end]"]);
}

#[test]
fn fragment_truncated_and_unterminated() {
    let mov = [(1 << 24) | (0xF << 9), (1 << 2) | PAD | TRI, PAD, PAD];
    let lines = disasm_fp(&fp_bytes(&[mov]));
    assert_eq!(texts(&lines), ["MOVR R0, R1", "no end bit: the program is unterminated"]);
    assert_eq!((lines[1].kind, lines[1].offset, lines[1].words.len()), (LineKind::Note, 16, 0));

    let mut bytes = fp_bytes(&[mov]);
    bytes.extend([1, 2, 3, 4]);
    let lines = disasm_fp(&bytes);
    assert_eq!(lines[1].kind, LineKind::Unknown);
    assert_eq!(lines[1].text, "?? 01020304 ; 4 trailing byte(s), not a whole instruction");
    assert_eq!(lines[2].kind, LineKind::Note);

    let mut bytes = fp_bytes(&[[(1 << 24) | (0xF << 9) | 1, 2 | PAD | TRI, PAD, PAD]]);
    bytes.extend([0u8; 8]);
    let lines = disasm_fp(&bytes);
    assert_eq!(lines.len(), 2);
    assert_eq!(lines[1].kind, LineKind::Unknown);
    assert_eq!(lines[1].text, "?? 00000000 00000000 ; constant block truncated to 8 byte(s)");
}

// ---- vertex opcode families -----------------------------------------------------------------------------------------

#[test]
fn vertex_co_issue() {
    let insn = vp(
        VIN,
        0x1B03,
        0x0002,
        (1 << 15) | CC,
        (4 << 27) | (2 << 22) | (4 << 12) | (3 << 8),
        (1 << 17) | (0xF << 13) | (2 << 7) | (0x1F << 2) | 1,
    );
    assert_eq!(insn, [0x00009c6c, 0x2080430d, 0x8106c0c0, 0x0043e17d]);
    assert_eq!(texts(&disasm_vp(&insn)), ["MUL R1, v3, c4 + RSQ R2.w, v3.xxxx  [end]"]);
}

#[test]
fn vertex_modifiers_and_outputs() {
    let flags = (5 << 10) | (1 << 13) | (1 << 25) | (1 << 26);
    let sat_pred = vp(0x1B15, VIN, VIN, flags, 1 << 22, (8 << 13) | NOSCA | (0x1F << 2) | 1);
    assert_eq!(texts(&disasm_vp(&sat_pred)), ["MOV_SAT R0.x (NE1.xxxx), R5  [end]"]);

    let h1 = (4 << 22) | (2 << 12) | (1 << 8);
    let mad = vp(0x11B02, 0x5503, 0x1B11, (3 << 15) | CC | (1 << 22), h1, (0xF << 13) | NOSCA | (0x1F << 2) | 1);
    assert_eq!(texts(&disasm_vp(&mad)), ["MAD R3, -v1, |c2.yyyy|, R4  [end]"]);

    let vec_out = vp(0x1B05, VIN, VIN, (1 << 30) | (0x3F << 15) | CC, 1 << 22, (0xC << 13) | NOSCA | (7 << 2) | 1);
    assert_eq!(texts(&disasm_vp(&vec_out)), ["MOV o[TEX0].xy, R1  [end]"]);

    let sca_out = vp(VIN, VIN, 0x0002, (0x3F << 15) | CC, 2 << 27, (8 << 17) | (1 << 12) | (0x1F << 7) | (5 << 2) | 1);
    assert_eq!(texts(&disasm_vp(&sca_out)), ["RCP o[FOGC].x, v0.xxxx  [end]"]);
}

#[test]
fn vertex_unknown_encodings_and_unterminated() {
    let unknown_op = vp(VIN, VIN, VIN, CC, 0x1A << 22, 1);
    let lines = disasm_vp(&unknown_op);
    assert_eq!(lines[0].kind, LineKind::Unknown);
    assert_eq!(
        lines[0].text,
        "?? 00001c6c 0680000d 8106c083 60400001 ; unknown vertex opcode (vector 0x1a, scalar 0x00)  [end]"
    );

    let stray_bit = vp(0x1B05, VIN, VIN, (1 << 15) | CC | (1 << 28), 1 << 22, (0xF << 13) | NOSCA | (0x1F << 2) | 1);
    assert_eq!(texts(&disasm_vp(&stray_bit)), ["MOV R1, R1  ?bits(w0:0x10000000)  [end]"]);

    let open = vp(0x1B05, VIN, VIN, (1 << 15) | CC, 1 << 22, (0xF << 13) | NOSCA | (0x1F << 2));
    let mut words = open.to_vec();
    words.push(0xdeadbeef);
    let lines = disasm_vp(&words);
    assert_eq!(lines.len(), 3);
    assert_eq!(lines[0].text, "MOV R1, R1");
    assert_eq!(lines[1].kind, LineKind::Unknown);
    assert_eq!(lines[1].text, "?? deadbeef ; 1 trailing word(s), not a whole instruction");
    assert_eq!((lines[2].kind, lines[2].text.as_str()), (LineKind::Note, "no end bit: the program is unterminated"));
}

// ---- static texture fetch counts ------------------------------------------------------------------------------------

/// The fetches the DISASSEMBLY shows: (opcode index into TEX_OPS, unit) per fetch line.
fn listed_fetches(lines: &[Line]) -> Vec<(usize, u32)> {
    use ps3dk_prof::nv40_disasm::TEX_OPS;
    let mut out = Vec::new();
    for l in lines.iter().filter(|l| l.kind == LineKind::Instruction) {
        let (m, ops) = split_ops(l.text.split(" + ").next().unwrap());
        let Some(k) = TEX_OPS.iter().position(|o| m.starts_with(o)) else { continue };
        let unit = ops.last().and_then(|o| o.strip_prefix("TEX")).and_then(|u| u.parse().ok()).expect("a unit");
        out.push((k, unit));
    }
    out
}

#[test]
fn texture_fetch_counts_agree_with_the_disassembly_and_the_independent_listing() {
    use ps3dk_prof::nv40_disasm::{fp_tex_fetches, vp_tex_fetches, TEX_OPS};
    let mut fetching = 0;
    for f in fixtures() {
        let (lines, t) = if f.profile == "vp" {
            (disasm_vp(&f.words), vp_tex_fetches(&f.words))
        } else {
            let bytes: Vec<u8> = f.words.iter().flat_map(|w| w.to_be_bytes()).collect();
            (disasm_fp(&bytes), fp_tex_fetches(&bytes))
        };
        let listed = listed_fetches(&lines);
        let mut by_op = [0u32; 5];
        let mut by_unit = [0u32; 16];
        for &(k, u) in &listed {
            by_op[k] += 1;
            by_unit[u as usize] += 1;
        }
        assert_eq!((t.by_op, t.by_unit), (by_op, by_unit), "{}: counts differ from the disassembly", f.name);
        let refs: Vec<String> = f.refs.iter().map(|r| parse_ref(r).op).collect();
        let independent: Vec<u32> =
            TEX_OPS.iter().map(|o| refs.iter().filter(|r| r.as_str() == *o).count() as u32).collect();
        assert_eq!(t.by_op.to_vec(), independent, "{}: counts differ from the independent listing", f.name);
        assert!(t.exact(), "{}: every fixture decodes through its end bit", f.name);
        let insns = lines.iter().filter(|l| l.kind == LineKind::Instruction).count() as u32;
        assert_eq!(t.instructions, insns, "{}", f.name);
        fetching += usize::from(t.total() > 0);
    }
    assert!(fetching >= 3, "the corpus has fetching programs of both profiles ({fetching})");
}

#[test]
fn texture_fetch_counts_by_opcode_unit_and_shape() {
    use ps3dk_prof::nv40_disasm::fp_tex_fetches;
    let fetch = |op: u32, unit: u32, end: u32| [(op << 24) | (0xF << 9) | (4 << 13) | (unit << 17) | end, 1 | PAD | TRI, PAD, PAD | (0x7FC << 19)];
    // TEX unit 3, TXP unit 3, TXB unit 0, TXD unit 15, TXL unit 7 (end)
    let program = [fetch(0x17, 3, 0), fetch(0x18, 3, 0), fetch(0x31, 0, 0), fetch(0x19, 15, 0), fetch(0x2F, 7, 1)];
    let t = fp_tex_fetches(&fp_bytes(&program));
    assert_eq!(t.by_op, [1, 1, 1, 1, 1]);
    assert_eq!(t.units(), [0, 3, 7, 15]);
    assert_eq!((t.by_unit[3], t.total(), t.instructions, t.exact()), (2, 5, 5, true));
    assert_eq!(t.ops_text(), "TEX 1 TXP 1 TXL 1 TXB 1 TXD 1");
    assert_eq!(listed_fetches(&disasm_fp(&fp_bytes(&program))).len(), 5);

    // A constant block is data: words shaped like a TEX inside it are not a fetch.
    let reads_const = [(1 << 24) | (0xF << 9), 2 | PAD | TRI, PAD, PAD];
    let mut bytes = fp_bytes(&[reads_const]);
    bytes.extend(fp_bytes(&[fetch(0x17, 1, 0)]));
    bytes.extend(fp_bytes(&[fetch(0x17, 2, 1)]));
    let t = fp_tex_fetches(&bytes);
    assert_eq!((t.total(), t.units(), t.instructions, t.exact()), (1, vec![2], 2, true));

    // Branches fetch nothing; an unknown opcode makes the counts a lower bound; so does a missing end bit.
    let branchy = [[(2 << 24) | (1 << 30), 5 << 18, (1 << 31) | 12, 20], fetch(0x17, 4, 1)];
    let t = fp_tex_fetches(&fp_bytes(&branchy));
    assert_eq!((t.total(), t.exact()), (1, true));
    let unknown = [[(0x30 << 24), TRI | PAD, PAD, PAD], fetch(0x17, 4, 1)];
    let t = fp_tex_fetches(&fp_bytes(&unknown));
    assert_eq!((t.total(), t.unknown, t.terminated, t.exact()), (1, 1, true, false));
    let t = fp_tex_fetches(&fp_bytes(&[fetch(0x17, 4, 0)]));
    assert_eq!((t.total(), t.terminated, t.exact()), (1, false, false));
    let t = fp_tex_fetches(&fp_bytes(&[[(1 << 24) | (0xF << 9) | 1, (1 << 2) | PAD | TRI, PAD, PAD]]));
    assert_eq!((t.total(), t.ops_text(), t.exact()), (0, "none".to_string(), true));
}

#[test]
fn vertex_texture_fetch_count() {
    use ps3dk_prof::nv40_disasm::vp_tex_fetches;
    let f = fixture("vp_tex_tex2d_v.txt");
    let t = vp_tex_fetches(&f.words);
    assert_eq!((t.by_op, t.units(), t.instructions, t.exact()), ([0, 0, 1, 0, 0], vec![0], 4, true));
    // without the instruction carrying the end bit, the walk is unterminated
    let t = vp_tex_fetches(&f.words[..12]);
    assert_eq!((t.total(), t.terminated), (1, false));
}
