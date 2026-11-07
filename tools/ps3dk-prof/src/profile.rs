//! The PROFILE record (docs/local/prof-viewer/profile-format-spec.md, gate B2p, draft r1): file_flags bit 1, chunk
//! type 17, exactly chunk index 2 of an extension file.  The record is the runtime's DECLARATION of the premise
//! profile it was built for, the digest of the profiler archive it was linked from (ARCHIVE-DIGEST-V1, computed at
//! archive build time) and the hardware/configuration facts it could or could not read at init.  It proves nothing
//! by itself: a premise is supported only by the reviewed evidence named in a table entry, and the binding of a
//! capture to a pinned image is the run evidence's job.
//!
//! [`parse`] applies the structural rules (3.1-3.4; any violation is MALFORMED, rule [`Rule::Profile`]).
//! [`classify`] applies the classification of 5.3 against a table of reviewed profiles.  The table compiled into
//! the reader ([`builtin_table`]) holds one entry, RSX-R1 (RSX units only); every other profile id classifies as not
//! reviewed.  Tests pass their own tables through the library API.

use std::fmt::Write as _;

use crate::model::{malformed, Malformed, Rule};
use crate::wire::Cursor;

pub const CHUNK_PROFILE: u32 = 17;
/// file_flags bit 1 (PROFILE_RECORD); it requires bit 0 (DOMAIN_EXTENSIONS).
pub const FLAG_PROFILE_RECORD: u32 = 2;
pub const PROFILE_VERSION: u16 = 1;
pub const DIGEST_ALG_ARCHIVE_V1: u32 = 1;
pub const FIELD_COUNT_V1: u16 = 7;

/// Premise bit names (spec 3.2), bit i = PREMISES[i].  Bits 9 (C-SPU) and 10 (C-ORD) are the SPU ticks slice's
/// delta (spu-ticks slice design 4.3).
pub const PREMISES: [&str; 11] = ["C-PPU", "C-RSX", "A", "S", "D", "P", "R64", "E", "C-PPU-Z", "C-SPU", "C-ORD"];
pub const PREMISE_MASK: u32 = (1 << PREMISES.len()) - 1;
pub const KIND_RSX: u32 = 1;
pub const KIND_SPU: u32 = 2;

pub const ST_READ: u8 = 1;
pub const ST_DECLARED_BUILD: u8 = 2;
pub const ST_NOT_READABLE: u8 = 3;
pub const ST_READ_FAILED: u8 = 4;

pub const F_SYS_SW_VERSION: u16 = 1;
pub const F_HW_MODEL_CLASS: u16 = 2;
pub const F_TB_FREQ_HZ: u16 = 3;
pub const F_SPU_USABLE: u16 = 4;
pub const F_LOADER_HOOK: u16 = 5;
pub const F_LIVE_LINKED: u16 = 6;
pub const F_EXT_CONFIG: u16 = 7;

/// (id, name, width, permitted statuses) of the version-1 fields (spec 3.4).
pub const FIELDS: [(u16, &str, u8, &[u8]); 7] = [
    (F_SYS_SW_VERSION, "SYS_SW_VERSION", 4, &[ST_READ, ST_NOT_READABLE, ST_READ_FAILED]),
    (F_HW_MODEL_CLASS, "HW_MODEL_CLASS", 4, &[ST_READ, ST_NOT_READABLE]),
    (F_TB_FREQ_HZ, "TB_FREQ_HZ", 8, &[ST_READ]),
    (F_SPU_USABLE, "SPU_USABLE", 4, &[ST_READ, ST_NOT_READABLE, ST_READ_FAILED]),
    (F_LOADER_HOOK, "LOADER_HOOK", 1, &[ST_DECLARED_BUILD]),
    (F_LIVE_LINKED, "LIVE_LINKED", 1, &[ST_DECLARED_BUILD]),
    (F_EXT_CONFIG, "EXT_CONFIG", 40, &[ST_READ]),
];

pub fn field_name(id: u16) -> &'static str {
    FIELDS.iter().find(|f| f.0 == id).map_or("?", |f| f.1)
}

/// One decoded identity field.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Field {
    pub id: u16,
    pub status: u8,
    pub value: Vec<u8>,
}

impl Field {
    /// The value as printed: decimal for widths 1, 4 and 8, hex otherwise; the status for unread fields.
    pub fn shown(&self) -> String {
        match self.status {
            ST_NOT_READABLE => "NOT_READABLE".to_string(),
            ST_READ_FAILED => "READ_FAILED".to_string(),
            _ => match self.value.len() {
                1 => self.value[0].to_string(),
                4 => u32::from_le_bytes(self.value[..].try_into().unwrap()).to_string(),
                8 => u64::from_le_bytes(self.value[..].try_into().unwrap()).to_string(),
                _ => hex(&self.value),
            },
        }
    }
}

/// A decoded PROFILE record (structure valid).
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct ProfileRecord {
    pub profile_id: String,
    pub digest: [u8; 32],
    pub premise_bits: u32,
    pub kinds: u32,
    /// In increasing field id order, each version-1 id exactly once.
    pub fields: Vec<Field>,
}

impl ProfileRecord {
    pub fn field(&self, id: u16) -> Option<&Field> {
        self.fields.iter().find(|f| f.id == id)
    }
}

pub fn hex(b: &[u8]) -> String {
    let mut s = String::with_capacity(b.len() * 2);
    for x in b {
        let _ = write!(s, "{x:02x}");
    }
    s
}

pub fn premise_names(bits: u32) -> String {
    let v: Vec<&str> = (0..PREMISES.len()).filter(|&i| bits & (1 << i) != 0).map(|i| PREMISES[i]).collect();
    if v.is_empty() {
        "none".to_string()
    } else {
        v.join(" ")
    }
}

pub fn kind_names(kinds: u32) -> String {
    let mut v = Vec::new();
    if kinds & KIND_RSX != 0 {
        v.push("RSX");
    }
    if kinds & KIND_SPU != 0 {
        v.push("SPU");
    }
    v.join("+")
}

fn bad<T>(detail: impl Into<String>) -> Result<T, Malformed> {
    malformed(Rule::Profile, detail)
}

/// The structural rules of spec 3.1-3.4.  `clock_freq`: CLOCKS freq_hz (chunk 1 precedes chunk 2).
pub fn parse(payload: &[u8], clock_freq: u64) -> Result<ProfileRecord, Malformed> {
    let mut c = Cursor::new(payload, "PROFILE");
    let version = c.u16()?;
    if version != PROFILE_VERSION {
        return bad(format!("version {version}"));
    }
    let r0 = c.u16()?;
    if r0 != 0 {
        return bad(format!("reserved0 {r0}"));
    }
    let n = c.u8()? as usize;
    if !(1..=32).contains(&n) {
        return bad(format!("id_len {n}"));
    }
    let mut id = Vec::with_capacity(n);
    for _ in 0..n {
        id.push(c.u8()?);
    }
    if !id[0].is_ascii_uppercase() {
        return bad("profile_id does not start with a letter A-Z");
    }
    if let Some(pos) = id.iter().position(|&b| !(b.is_ascii_uppercase() || b.is_ascii_digit() || b == b'-')) {
        return bad(format!("profile_id byte {pos} is outside A-Z 0-9 -"));
    }
    let alg = c.u32()?;
    if alg != DIGEST_ALG_ARCHIVE_V1 {
        return bad(format!("digest_alg {alg}"));
    }
    let mut digest = [0u8; 32];
    for d in digest.iter_mut() {
        *d = c.u8()?;
    }
    let premise_bits = c.u32()?;
    if premise_bits & !PREMISE_MASK != 0 {
        return bad(format!("premise_bits {premise_bits:#x} sets an undefined bit"));
    }
    let kinds = c.u32()?;
    if kinds == 0 || kinds & !(KIND_RSX | KIND_SPU) != 0 {
        return bad(format!("kinds {kinds:#x}"));
    }
    let count = c.u16()?;
    if count != FIELD_COUNT_V1 {
        return bad(format!("field_count {count}"));
    }
    let mut fields = Vec::with_capacity(count as usize);
    for k in 0..count as usize {
        let fid = c.u16()?;
        let status = c.u8()?;
        let width = c.u8()?;
        let Some(&(want_id, name, want_w, permitted)) = FIELDS.get(k) else { return bad("too many fields") };
        if fid != want_id {
            return bad(format!("field {k}: id {fid}, want {want_id} (each id once, in increasing order)"));
        }
        if width != want_w {
            return bad(format!("field {name}: width {width}, want {want_w}"));
        }
        let mut value = Vec::with_capacity(width as usize);
        for _ in 0..width {
            value.push(c.u8()?);
        }
        if !(ST_READ..=ST_READ_FAILED).contains(&status) {
            return bad(format!("field {name}: status {status}"));
        }
        if !permitted.contains(&status) {
            return bad(format!("field {name}: status {status} is not permitted for this field"));
        }
        if (status == ST_NOT_READABLE || status == ST_READ_FAILED) && value.iter().any(|&b| b != 0) {
            return bad(format!("field {name}: a nonzero value with status {status}"));
        }
        if fid == F_TB_FREQ_HZ {
            let v = u64::from_le_bytes(value[..].try_into().unwrap());
            if v != clock_freq {
                return bad(format!("TB_FREQ_HZ {v} differs from CLOCKS freq_hz {clock_freq}"));
            }
        }
        fields.push(Field { id: fid, status, value });
    }
    c.done()?;
    Ok(ProfileRecord { profile_id: String::from_utf8(id).expect("ASCII"), digest, premise_bits, kinds, fields })
}

// ---- the reviewed-profile table and the classification (spec 5.2, 5.3) ----------------------------------------------

/// How a table entry treats one field.
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum FieldRule {
    /// The field must have a READ / DECLARED_BUILD value in this list (exact bytes).
    RequiredMatch(Vec<Vec<u8>>),
    /// The field is printed, never checked; the reason is part of the reviewed entry.
    Any(String),
}

/// One reviewed profile.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct TableEntry {
    pub profile_id: String,
    pub kinds: u32,
    pub premise_bits: u32,
    pub digests: Vec<[u8; 32]>,
    /// One rule per version-1 field id (a missing id is treated as REQUIRED-MATCH with no accepted value).
    pub fields: Vec<(u16, FieldRule)>,
    pub evidence: Vec<String>,
}

/// The runtime digests of the two RSX-R1 archives measured on the console (ILP32 and LP64 builds of the same
/// sources), as printed by the archive's verify step.
const RSX_R1_DIGESTS: [&str; 2] = [
    "39dcb52ae3a1dc034b3974490c886f9935f049dcf854aed254ebae5e4e9e1fca",
    "5b53c967cadf272c4b19746668a7b2deab3418eb06ff93e2bca55183fc9b480d",
];

fn digest_from_hex(s: &str) -> [u8; 32] {
    let mut d = [0u8; 32];
    for (i, b) in d.iter_mut().enumerate() {
        *b = u8::from_str_radix(&s[2 * i..2 * i + 2], 16).expect("table digest is hex");
    }
    d
}

/// The table compiled into the reader.  Adding or changing an entry is a reviewed change carrying the evidence
/// references (Plan B r2 4.3).
///
/// RSX-R1 covers RSX units only.  Evidence: two console captures of the heterogeneous-workload sample, one per data
/// model, each with 17 brackets, every GPU pair interpolated, the held-out bracket test 8 of 8 decisive and none
/// contradicted, and the same slope within the fitted bounds in both data models.
pub fn builtin_table() -> Vec<TableEntry> {
    vec![TableEntry {
        profile_id: "RSX-R1".into(),
        kinds: KIND_RSX,
        premise_bits: 0b1_1100_1111,
        digests: RSX_R1_DIGESTS.iter().map(|h| digest_from_hex(h)).collect(),
        fields: vec![
            (F_SYS_SW_VERSION, FieldRule::Any("not readable by the runtime on the measured console".into())),
            (F_HW_MODEL_CLASS, FieldRule::Any("not readable by the runtime on the measured console".into())),
            (F_TB_FREQ_HZ, FieldRule::RequiredMatch(vec![79_800_000u64.to_le_bytes().to_vec()])),
            (F_SPU_USABLE, FieldRule::Any("RSX placement does not use SPUs".into())),
            (F_LOADER_HOOK, FieldRule::RequiredMatch(vec![vec![0], vec![1]])),
            (F_LIVE_LINKED, FieldRule::RequiredMatch(vec![vec![0], vec![1]])),
            (F_EXT_CONFIG, FieldRule::Any("each capture's held-out bracket test checks its own map".into())),
        ],
        evidence: vec![
            "console capture, ILP32, heterogeneous-workload, 4777 frames, 9554 GPU pairs".into(),
            "console capture, LP64, heterogeneous-workload, 4774 frames, 9548 GPU pairs".into(),
        ],
    }]
}

/// The classification of one capture for one unit kind.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Classification {
    /// Every failing item of spec 5.3, in its order; empty: M-PROF holds for the kind.
    pub failures: Vec<String>,
    /// Fields of the entry marked ANY (printed on every MEASURED line as "not checked").
    pub not_checked: Vec<String>,
}

impl Classification {
    pub fn holds(&self) -> bool {
        self.failures.is_empty()
    }
}

/// Spec 5.3 for `kind` (KIND_RSX or KIND_SPU).  `record` None: no PROFILE (bit 1 clear, or torn before chunk 2).
pub fn classify(record: Option<&ProfileRecord>, table: &[TableEntry], kind: u32) -> Classification {
    let mut out = Classification { failures: Vec::new(), not_checked: Vec::new() };
    let Some(r) = record else {
        out.failures.push("no profile record".to_string());
        return out;
    };
    let Some(e) = table.iter().find(|e| e.profile_id == r.profile_id) else {
        out.failures.push(format!("profile not reviewed: {}", r.profile_id));
        if r.kinds & kind == 0 {
            out.failures.push(format!("{} not covered by {}", kind_names(kind), r.profile_id));
        }
        return out;
    };
    if !e.digests.contains(&r.digest) {
        out.failures.push(format!("runtime not reviewed for {}: {}", r.profile_id, &hex(&r.digest)[..16]));
    }
    if e.premise_bits != r.premise_bits {
        let plus = r.premise_bits & !e.premise_bits;
        let minus = e.premise_bits & !r.premise_bits;
        out.failures.push(format!("premise set differs: +{} -{}", premise_names(plus), premise_names(minus)));
    }
    if r.kinds & kind == 0 || e.kinds & kind == 0 {
        out.failures.push(format!("{} not covered by {}", kind_names(kind), r.profile_id));
    }
    for (fid, name, _, _) in FIELDS {
        let f = r.field(fid).expect("every version-1 field is present after parse");
        let rule = e.fields.iter().find(|(i, _)| *i == fid).map(|(_, r)| r);
        match rule {
            Some(FieldRule::Any(_)) => out.not_checked.push(name.to_string()),
            Some(FieldRule::RequiredMatch(accepted)) => {
                if f.status == ST_NOT_READABLE || f.status == ST_READ_FAILED {
                    out.failures.push(format!("applicability unchecked: {name}"));
                } else if !accepted.contains(&f.value) {
                    out.failures.push(format!("outside profile scope: {name}={}", f.shown()));
                }
            }
            None => out.failures.push(format!("outside profile scope: {name}={} (no accepted value)", f.shown())),
        }
    }
    out
}

/// The report line of a decoded record (printed only for files that carry one).
pub fn record_line(r: &ProfileRecord) -> String {
    let fields: Vec<String> = r.fields.iter().map(|f| format!("{}={}", field_name(f.id), f.shown())).collect();
    format!(
        "profile: {} (declared by the runtime) | runtime digest {} | premises {} | kinds {} | {}",
        r.profile_id,
        hex(&r.digest),
        premise_names(r.premise_bits),
        kind_names(r.kinds),
        fields.join(" ")
    )
}
