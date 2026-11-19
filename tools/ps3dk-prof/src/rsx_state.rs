//! RSX frame captures, phase 2: the 3D register file at each draw and its interpretation into typed fields.
//!
//! The runtime stores raw register values (BASELINE at arming, then per-draw STATE deltas; format section 12.3); this
//! module turns a [`Registers`] snapshot into the field tables the CLI and the viewer show: render state, the 16
//! fragment texture slots, the 16 vertex attributes, the index stream and the surfaces. The tables are written from
//! the public NV40 method names in `sdk/include/rsx/nv40.h`, the value names in `sdk/include/cell/gcm/gcm_enum.h` and
//! `sdk/include/rsx/gcm_sys.h`, and the encodings this SDK's own command writers use (measured behaviour); a field
//! whose register was never written since the last decoder reset is [`Value::Unknown`], never guessed.
use serde_json::{json, Map, Value as Json};

/// Registers in the 3D register file (methods 0x0000..0x1ffc).
pub const REGS: usize = 2048;

/// The register file at one draw: the value last written and whether it is known.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Registers {
    value: Vec<u32>,
    known: Vec<u64>,
}
impl Default for Registers {
    fn default() -> Self {
        Self::new()
    }
}
impl Registers {
    pub fn new() -> Self {
        Registers { value: vec![0; REGS], known: vec![0; REGS / 64] }
    }
    /// The value of the register at byte address `method` (0..0x1ffc), None when unknown.
    pub fn get(&self, method: u32) -> Option<u32> {
        let i = (method >> 2) as usize;
        (method & 3 == 0 && i < REGS && self.known[i / 64] >> (i % 64) & 1 != 0).then(|| self.value[i])
    }
    /// Sets register `index` (method >> 2).
    pub fn set(&mut self, index: u16, value: u32) {
        let i = index as usize;
        self.value[i] = value;
        self.known[i / 64] |= 1 << (i % 64);
    }
    /// Every register unknown (a decoder reset).
    pub fn clear(&mut self) {
        self.known.iter_mut().for_each(|w| *w = 0);
    }
    pub fn known_count(&self) -> usize {
        self.known.iter().map(|w| w.count_ones() as usize).sum()
    }
}

/// VP constant slots (the VP constant file).
pub const VP_CONSTS: usize = 468;

/// The VP constant file at one draw: four 32-bit words per slot and a known-component mask (bit k: word k).
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct VpConstants {
    value: Vec<[u32; 4]>,
    known: Vec<u8>,
}
impl Default for VpConstants {
    fn default() -> Self {
        Self::new()
    }
}
impl VpConstants {
    pub fn new() -> Self {
        VpConstants { value: vec![[0; 4]; VP_CONSTS], known: vec![0; VP_CONSTS] }
    }
    /// The words of `slot` with their known mask (None: no component known).
    pub fn get(&self, slot: usize) -> Option<([u32; 4], u8)> {
        (slot < VP_CONSTS && self.known[slot] != 0).then(|| (self.value[slot], self.known[slot]))
    }
    /// The slot as floats, a component None when unknown.
    pub fn floats(&self, slot: usize) -> [Option<f32>; 4] {
        let mut out = [None; 4];
        if let Some((v, m)) = self.get(slot) {
            for k in 0..4 {
                if m >> k & 1 != 0 {
                    out[k] = Some(f32::from_bits(v[k]));
                }
            }
        }
        out
    }
    pub fn set(&mut self, slot: u16, mask: u8, value: [u32; 4]) {
        let i = slot as usize;
        self.value[i] = value;
        self.known[i] = mask;
    }
    pub fn clear(&mut self) {
        self.known.iter_mut().for_each(|m| *m = 0);
    }
    pub fn known_count(&self) -> usize {
        self.known.iter().filter(|&&m| m != 0).count()
    }
}

/// An FP embedded-constant word as it lies in memory (16-bit halves swapped) as a float.
pub fn fp_word_float(w: u32) -> f32 {
    f32::from_bits(w.rotate_left(16))
}

/// One interpreted field value.
#[derive(Debug, Clone, PartialEq)]
pub enum Value {
    /// The register was not written since the last decoder reset.
    Unknown,
    Bool(bool),
    Uint(u32),
    Float(f32),
    /// A named value (an enumerant from the SDK headers).
    Name(&'static str),
    /// A raw value shown in hex (a value with no name, or a register with no field table).
    Hex(u32),
}
impl Value {
    pub fn text(&self) -> String {
        match self {
            Value::Unknown => "?".into(),
            Value::Bool(b) => b.to_string(),
            Value::Uint(v) => v.to_string(),
            Value::Float(f) => format!("{f}"),
            Value::Name(n) => (*n).into(),
            Value::Hex(v) => format!("{v:#x}"),
        }
    }
    pub fn json(&self) -> Json {
        match self {
            Value::Unknown => Json::Null,
            Value::Bool(b) => json!(b),
            Value::Uint(v) => json!(v),
            Value::Float(f) => json!(f),
            Value::Name(n) => json!(n),
            Value::Hex(v) => json!(format!("{v:#x}")),
        }
    }
}

/// One field of a table: its name (dotted path), value and the register (byte address) it comes from.
#[derive(Debug, Clone, PartialEq)]
pub struct Field {
    pub name: String,
    pub value: Value,
    pub register: u32,
}

/// Every table of one draw.
#[derive(Debug, Clone, PartialEq)]
pub struct StateView {
    pub render: Vec<Field>,
    /// Fragment texture slots 0..15.
    pub textures: Vec<Vec<Field>>,
    /// Vertex attributes 0..15.
    pub attributes: Vec<Vec<Field>>,
    pub index: Vec<Field>,
    pub surfaces: Vec<Field>,
}

// ---- value names --------------------------------------------------------------------------------------------------

type Names = &'static [(u32, &'static str)];

const FUNC: Names = &[(0x200, "NEVER"), (0x201, "LESS"), (0x202, "EQUAL"), (0x203, "LEQUAL"), (0x204, "GREATER"),
    (0x205, "NOTEQUAL"), (0x206, "GEQUAL"), (0x207, "ALWAYS")];
const STENCIL_OP: Names = &[(0, "ZERO"), (0x150a, "INVERT"), (0x1e00, "KEEP"), (0x1e01, "REPLACE"), (0x1e02, "INCR"),
    (0x1e03, "DECR"), (0x8507, "INCR_WRAP"), (0x8508, "DECR_WRAP")];
const FACTOR: Names = &[(0, "ZERO"), (1, "ONE"), (0x300, "SRC_COLOR"), (0x301, "ONE_MINUS_SRC_COLOR"),
    (0x302, "SRC_ALPHA"), (0x303, "ONE_MINUS_SRC_ALPHA"), (0x304, "DST_ALPHA"), (0x305, "ONE_MINUS_DST_ALPHA"),
    (0x306, "DST_COLOR"), (0x307, "ONE_MINUS_DST_COLOR"), (0x308, "SRC_ALPHA_SATURATE"), (0x8001, "CONSTANT_COLOR"),
    (0x8002, "ONE_MINUS_CONSTANT_COLOR"), (0x8003, "CONSTANT_ALPHA"), (0x8004, "ONE_MINUS_CONSTANT_ALPHA")];
const EQUATION: Names = &[(0x8006, "ADD"), (0x8007, "MIN"), (0x8008, "MAX"), (0x800a, "SUBTRACT"),
    (0x800b, "REVERSE_SUBTRACT")];
const SHADE: Names = &[(0x1d00, "FLAT"), (0x1d01, "SMOOTH")];
const LOGIC: Names = &[(0x1500, "CLEAR"), (0x1501, "AND"), (0x1502, "AND_REVERSE"), (0x1503, "COPY"),
    (0x1504, "AND_INVERTED"), (0x1505, "NOOP"), (0x1506, "XOR"), (0x1507, "OR"), (0x1508, "NOR"), (0x1509, "EQUIV"),
    (0x150a, "INVERT"), (0x150b, "OR_REVERSE"), (0x150c, "COPY_INVERTED"), (0x150d, "OR_INVERTED"), (0x150e, "NAND"),
    (0x150f, "SET")];
const POLYGON_MODE: Names = &[(0x1b00, "POINT"), (0x1b01, "LINE"), (0x1b02, "FILL")];
const CULL_FACE: Names = &[(0x404, "FRONT"), (0x405, "BACK"), (0x408, "FRONT_AND_BACK")];
const FRONT_FACE: Names = &[(0x900, "CW"), (0x901, "CCW")];
const TEX_FORMAT: Names = &[(0x81, "B8"), (0x82, "A1R5G5B5"), (0x83, "A4R4G4B4"), (0x84, "R5G6B5"), (0x85, "A8R8G8B8"),
    (0x86, "COMPRESSED_DXT1"), (0x87, "COMPRESSED_DXT23"), (0x88, "COMPRESSED_DXT45"), (0x8b, "G8B8"),
    (0x8f, "R6G5B5"), (0x90, "DEPTH24_D8"), (0x91, "DEPTH24_D8_FLOAT"), (0x92, "DEPTH16"), (0x93, "DEPTH16_FLOAT"),
    (0x94, "X16"), (0x95, "Y16_X16"), (0x97, "R5G5B5A1"), (0x98, "COMPRESSED_HILO8"), (0x99, "COMPRESSED_HILO_S8"),
    (0x9a, "W16_Z16_Y16_X16_FLOAT"), (0x9b, "W32_Z32_Y32_X32_FLOAT"), (0x9c, "X32_FLOAT"), (0x9d, "D1R5G5B5"),
    (0x9e, "D8R8G8B8"), (0x9f, "Y16_X16_FLOAT")];
const TEX_LOCATION: Names = &[(1, "local"), (2, "main")];
const TEX_DIMENSION: Names = &[(1, "1D"), (2, "2D"), (3, "3D")];
const WRAP: Names = &[(1, "WRAP"), (2, "MIRROR"), (3, "CLAMP_TO_EDGE"), (4, "BORDER"), (5, "CLAMP"),
    (6, "MIRROR_ONCE_CLAMP_TO_EDGE"), (7, "MIRROR_ONCE_BORDER"), (8, "MIRROR_ONCE_CLAMP")];
const FILTER_MIN: Names = &[(1, "NEAREST"), (2, "LINEAR"), (3, "NEAREST_NEAREST"), (4, "LINEAR_NEAREST"),
    (5, "NEAREST_LINEAR"), (6, "LINEAR_LINEAR"), (7, "CONVOLUTION_MIN")];
const FILTER_MAG: Names = &[(1, "NEAREST"), (2, "LINEAR"), (4, "CONVOLUTION_MAG")];
const VERTEX_TYPE: Names = &[(1, "S1"), (2, "F"), (3, "SF"), (4, "UB"), (5, "S32K"), (6, "CMP"), (7, "UB256")];
const LOCATION_BIT: Names = &[(0, "local"), (1, "main")];
const DMA_LOCATION: Names = &[(0xfeed_0000, "local"), (0xfeed_0001, "main")];
const INDEX_TYPE: Names = &[(0, "u32"), (1, "u16")];
const SURFACE_COLOR: Names = &[(1, "X1R5G5B5_Z1R5G5B5"), (2, "X1R5G5B5_O1R5G5B5"), (3, "R5G6B5"),
    (4, "X8R8G8B8_Z8R8G8B8"), (5, "X8R8G8B8"), (8, "A8R8G8B8"), (9, "B8"), (10, "G8B8"), (11, "F_W16Z16Y16X16"),
    (12, "F_W32Z32Y32X32"), (13, "F_X32"), (14, "X8B8G8R8_Z8B8G8R8"), (15, "X8B8G8R8_O8B8G8R8"), (16, "A8B8G8R8")];
const SURFACE_ZETA: Names = &[(1, "Z16"), (2, "Z24S8")];
const SURFACE_TYPE: Names = &[(1, "linear"), (2, "swizzled")];
const ANTIALIAS: Names = &[(0, "CENTER_1"), (3, "DIAGONAL_CENTERED_2"), (4, "SQUARE_CENTERED_4"),
    (5, "SQUARE_ROTATED_4")];
const FREQUENCY_OP: Names = &[(0, "divide"), (1, "modulo")];

/// How one field is read from its register.
#[derive(Clone, Copy)]
enum Kind {
    Bool,
    Uint,
    Float,
    Hex,
    Named(Names),
}

struct Table<'a> {
    regs: &'a Registers,
    out: Vec<Field>,
}
impl Table<'_> {
    /// Field `name` = bits `shift`.. of `mask` (after the shift) of register `method`.
    fn field(&mut self, name: &str, method: u32, shift: u32, mask: u32, kind: Kind) -> &mut Self {
        let value = match self.regs.get(method) {
            None => Value::Unknown,
            Some(raw) => {
                let v = (raw >> shift) & mask;
                match kind {
                    Kind::Bool => Value::Bool(v != 0),
                    Kind::Uint => Value::Uint(v),
                    Kind::Float => Value::Float(f32::from_bits(v)),
                    Kind::Hex => Value::Hex(v),
                    Kind::Named(names) => names.iter().find(|(k, _)| *k == v).map_or(Value::Hex(v), |&(_, n)| Value::Name(n)),
                }
            }
        };
        self.out.push(Field { name: name.into(), value, register: method });
        self
    }
    fn whole(&mut self, name: &str, method: u32, kind: Kind) -> &mut Self {
        self.field(name, method, 0, u32::MAX, kind)
    }
}

fn table<'a>(regs: &'a Registers) -> Table<'a> {
    Table { regs, out: Vec::new() }
}

const LO16: u32 = 0xffff;

fn render(r: &Registers) -> Vec<Field> {
    let mut t = table(r);
    t.whole("dither", 0x300, Kind::Bool)
        .whole("alpha_test.enable", 0x304, Kind::Bool)
        .whole("alpha_test.func", 0x308, Kind::Named(FUNC))
        .whole("alpha_test.ref", 0x30c, Kind::Uint)
        .whole("blend.enable", 0x310, Kind::Bool)
        .whole("blend.enable_mrt", 0x36c, Kind::Hex)
        .field("blend.src_rgb", 0x314, 0, LO16, Kind::Named(FACTOR))
        .field("blend.src_alpha", 0x314, 16, LO16, Kind::Named(FACTOR))
        .field("blend.dst_rgb", 0x318, 0, LO16, Kind::Named(FACTOR))
        .field("blend.dst_alpha", 0x318, 16, LO16, Kind::Named(FACTOR))
        .field("blend.equation_rgb", 0x320, 0, LO16, Kind::Named(EQUATION))
        .field("blend.equation_alpha", 0x320, 16, LO16, Kind::Named(EQUATION))
        .whole("blend.color", 0x31c, Kind::Hex)
        .whole("blend.color2", 0x37c, Kind::Hex)
        .field("color_mask.b", 0x324, 0, 0xff, Kind::Bool)
        .field("color_mask.g", 0x324, 8, 0xff, Kind::Bool)
        .field("color_mask.r", 0x324, 16, 0xff, Kind::Bool)
        .field("color_mask.a", 0x324, 24, 0xff, Kind::Bool)
        .whole("color_mask.mrt", 0x370, Kind::Hex);
    // 0x328.. is the stencil test SetStencilTestEnable/Func/Mask/Op write (the front face, or both when one-sided);
    // 0x348.. is SetTwoSidedStencilTestEnable and the back face's SetBackStencil* (the reverse of nouveau's names)
    for (side, base) in [("front", 0x328u32), ("back", 0x348)] {
        t.whole(&format!("stencil.{side}.enable"), base, Kind::Bool)
            .whole(&format!("stencil.{side}.write_mask"), base + 4, Kind::Hex)
            .whole(&format!("stencil.{side}.func"), base + 8, Kind::Named(FUNC))
            .whole(&format!("stencil.{side}.ref"), base + 12, Kind::Uint)
            .whole(&format!("stencil.{side}.func_mask"), base + 16, Kind::Hex)
            .whole(&format!("stencil.{side}.fail"), base + 20, Kind::Named(STENCIL_OP))
            .whole(&format!("stencil.{side}.zfail"), base + 24, Kind::Named(STENCIL_OP))
            .whole(&format!("stencil.{side}.zpass"), base + 28, Kind::Named(STENCIL_OP));
    }
    t.whole("shade_model", 0x368, Kind::Named(SHADE))
        .whole("logic_op.enable", 0x374, Kind::Bool)
        .whole("logic_op.op", 0x378, Kind::Named(LOGIC))
        .whole("depth.test", 0xa74, Kind::Bool)
        .whole("depth.func", 0xa6c, Kind::Named(FUNC))
        .whole("depth.write", 0xa70, Kind::Bool)
        .whole("depth.bounds_test", 0x380, Kind::Bool)
        .whole("depth.bounds_min", 0x384, Kind::Float)
        .whole("depth.bounds_max", 0x388, Kind::Float)
        .whole("depth.range_near", 0x394, Kind::Float)
        .whole("depth.range_far", 0x398, Kind::Float)
        .whole("cull.enable", 0x183c, Kind::Bool)
        .whole("cull.face", 0x1830, Kind::Named(CULL_FACE))
        .whole("cull.front_face", 0x1834, Kind::Named(FRONT_FACE))
        .whole("polygon.mode_front", 0x1828, Kind::Named(POLYGON_MODE))
        .whole("polygon.mode_back", 0x182c, Kind::Named(POLYGON_MODE))
        .whole("polygon.smooth", 0x1838, Kind::Bool)
        .whole("polygon.offset_point", 0xa60, Kind::Bool)
        .whole("polygon.offset_line", 0xa64, Kind::Bool)
        .whole("polygon.offset_fill", 0xa68, Kind::Bool)
        .whole("polygon.offset_factor", 0xa78, Kind::Float)
        .whole("polygon.offset_units", 0xa7c, Kind::Float)
        .whole("line.width", 0x3b8, Kind::Uint)
        .whole("line.smooth", 0x3bc, Kind::Bool)
        .whole("point.size", 0x1ee0, Kind::Float)
        .whole("point.sprite", 0x1ee8, Kind::Hex)
        .whole("two_side_light", 0x142c, Kind::Bool)
        .field("viewport.x", 0xa00, 0, LO16, Kind::Uint)
        .field("viewport.width", 0xa00, 16, LO16, Kind::Uint)
        .field("viewport.y", 0xa04, 0, LO16, Kind::Uint)
        .field("viewport.height", 0xa04, 16, LO16, Kind::Uint);
    for (i, c) in ["x", "y", "z", "w"].iter().enumerate() {
        t.whole(&format!("viewport.offset_{c}"), 0xa20 + 4 * i as u32, Kind::Float);
    }
    for (i, c) in ["x", "y", "z", "w"].iter().enumerate() {
        t.whole(&format!("viewport.scale_{c}"), 0xa30 + 4 * i as u32, Kind::Float);
    }
    t.field("scissor.x", 0x8c0, 0, LO16, Kind::Uint)
        .field("scissor.width", 0x8c0, 16, LO16, Kind::Uint)
        .field("scissor.y", 0x8c4, 0, LO16, Kind::Uint)
        .field("scissor.height", 0x8c4, 16, LO16, Kind::Uint)
        .whole("clip.horizontal", 0x2c0, Kind::Hex)
        .whole("clip.vertical", 0x2c4, Kind::Hex)
        .whole("zcull.enable", 0x1d84, Kind::Hex)
        .whole("zcull.stats", 0x1804, Kind::Hex)
        .whole("clear.color", 0x1d90, Kind::Hex)
        .whole("clear.depth_stencil", 0x1d8c, Kind::Hex)
        .whole("anti_aliasing", 0x1d7c, Kind::Hex)
        .whole("depth_control", 0x1d78, Kind::Hex)
        .whole("fp_control", 0x1d60, Kind::Hex)
        .whole("vp.attribute_inputs", 0x1ff0, Kind::Hex)
        .whole("vp.outputs", 0x1ff4, Kind::Hex);
    t.out
}

fn texture(r: &Registers, i: u32) -> Vec<Field> {
    let b = 0x1a00 + 32 * i;
    let mut t = table(r);
    t.whole("offset", b, Kind::Hex)
        .field("location", b + 4, 0, 3, Kind::Named(TEX_LOCATION))
        .field("cubemap", b + 4, 2, 1, Kind::Bool)
        .field("dimension", b + 4, 4, 0xf, Kind::Named(TEX_DIMENSION))
        .field("format", b + 4, 8, 0x9f, Kind::Named(TEX_FORMAT))
        .field("linear", b + 4, 13, 1, Kind::Bool)
        .field("unnormalized", b + 4, 14, 1, Kind::Bool)
        .field("mipmaps", b + 4, 16, 0xf, Kind::Uint)
        .field("width", b + 0x18, 16, LO16, Kind::Uint)
        .field("height", b + 0x18, 0, LO16, Kind::Uint)
        .field("depth", 0x1840 + 4 * i, 20, 0xfff, Kind::Uint)
        .field("pitch", 0x1840 + 4 * i, 0, 0xf_ffff, Kind::Uint)
        .field("enable", b + 0xc, 31, 1, Kind::Bool)
        .field("min_lod", b + 0xc, 19, 0xfff, Kind::Uint)
        .field("max_lod", b + 0xc, 7, 0xfff, Kind::Uint)
        .field("max_aniso", b + 0xc, 4, 7, Kind::Uint)
        .field("wrap.s", b + 8, 0, 0xf, Kind::Named(WRAP))
        .field("wrap.t", b + 8, 8, 0xf, Kind::Named(WRAP))
        .field("wrap.r", b + 8, 16, 0xf, Kind::Named(WRAP))
        .field("wrap.unsigned_remap", b + 8, 12, 0xf, Kind::Uint)
        .field("wrap.gamma", b + 8, 20, 0xf, Kind::Hex)
        .field("wrap.zfunc", b + 8, 28, 0xf, Kind::Uint)
        .field("filter.min", b + 0x14, 16, 0xf, Kind::Named(FILTER_MIN))
        .field("filter.mag", b + 0x14, 24, 0xf, Kind::Named(FILTER_MAG))
        .field("filter.convolution", b + 0x14, 13, 3, Kind::Uint)
        .field("filter.bias", b + 0x14, 0, 0x1fff, Kind::Hex)
        .whole("remap", b + 0x10, Kind::Hex)
        .whole("border_color", b + 0x1c, Kind::Hex);
    t.out
}

fn attribute(r: &Registers, i: u32) -> Vec<Field> {
    let fmt = 0x1740 + 4 * i;
    let mut t = table(r);
    let enabled = r.get(fmt).map_or(Value::Unknown, |v| Value::Bool((v >> 4) & 0xf != 0));
    t.out.push(Field { name: "enabled".into(), value: enabled, register: fmt });
    t.field("type", fmt, 0, 0xf, Kind::Named(VERTEX_TYPE))
        .field("size", fmt, 4, 0xf, Kind::Uint)
        .field("stride", fmt, 8, 0xff, Kind::Uint)
        .field("frequency", fmt, 16, LO16, Kind::Uint)
        .field("location", 0x1680 + 4 * i, 31, 1, Kind::Named(LOCATION_BIT))
        .field("offset", 0x1680 + 4 * i, 0, 0x7fff_ffff, Kind::Hex)
        .field("frequency_op", 0x1fc0, i, 1, Kind::Named(FREQUENCY_OP));
    t.out
}

fn index(r: &Registers) -> Vec<Field> {
    let mut t = table(r);
    t.whole("offset", 0x181c, Kind::Hex)
        .field("location", 0x1820, 0, 1, Kind::Named(LOCATION_BIT))
        .field("type", 0x1820, 4, 1, Kind::Named(INDEX_TYPE))
        .whole("restart.enable", 0x1dac, Kind::Bool)
        .whole("restart.index", 0x1db0, Kind::Hex);
    t.out
}

fn surfaces(r: &Registers) -> Vec<Field> {
    let mut t = table(r);
    t.field("color_format", 0x208, 0, 0x1f, Kind::Named(SURFACE_COLOR))
        .field("zeta_format", 0x208, 5, 7, Kind::Named(SURFACE_ZETA))
        .field("type", 0x208, 8, 0xf, Kind::Named(SURFACE_TYPE))
        .field("antialias", 0x208, 12, 0xf, Kind::Named(ANTIALIAS))
        .field("log2_width", 0x208, 16, 0xff, Kind::Uint)
        .field("log2_height", 0x208, 24, 0xff, Kind::Uint)
        .field("targets.color0", 0x220, 0, 1, Kind::Bool)
        .field("targets.color1", 0x220, 1, 1, Kind::Bool)
        .field("targets.color2", 0x220, 2, 1, Kind::Bool)
        .field("targets.color3", 0x220, 3, 1, Kind::Bool)
        .field("targets.mrt", 0x220, 4, 1, Kind::Bool);
    for (n, off, pitch, dma) in [(0, 0x210u32, 0x20cu32, 0x194u32), (1, 0x218, 0x21c, 0x18c), (2, 0x288, 0x280, 0x1b4),
                                 (3, 0x28c, 0x284, 0x1b8)] {
        t.whole(&format!("color{n}.offset"), off, Kind::Hex)
            .whole(&format!("color{n}.pitch"), pitch, Kind::Uint)
            .whole(&format!("color{n}.location"), dma, Kind::Named(DMA_LOCATION));
    }
    t.whole("zeta.offset", 0x214, Kind::Hex)
        .whole("zeta.pitch", 0x22c, Kind::Uint)
        .whole("zeta.location", 0x198, Kind::Named(DMA_LOCATION))
        .field("clip.x", 0x200, 0, LO16, Kind::Uint)
        .field("clip.width", 0x200, 16, LO16, Kind::Uint)
        .field("clip.y", 0x204, 0, LO16, Kind::Uint)
        .field("clip.height", 0x204, 16, LO16, Kind::Uint)
        .field("window.x", 0x2b8, 0, LO16, Kind::Uint)
        .field("window.y", 0x2b8, 16, LO16, Kind::Uint);
    t.out
}

/// Every field table of `r`.
pub fn interpret(r: &Registers) -> StateView {
    StateView {
        render: render(r),
        textures: (0..16).map(|i| texture(r, i)).collect(),
        attributes: (0..16).map(|i| attribute(r, i)).collect(),
        index: index(r),
        surfaces: surfaces(r),
    }
}

/// The NV40 method name of a register (byte address), or its address.
pub fn register_name(method: u32) -> String {
    let indexed = |base: u32, stride: u32, count: u32, name: &str| {
        (method >= base && method < base + stride * count && (method - base) % stride == 0)
            .then(|| format!("{name}({})", (method - base) / stride))
    };
    let fixed: &[(u32, &str)] = &[
        (0x194, "DMA_COLOR0"), (0x18c, "DMA_COLOR1"), (0x1b4, "DMA_COLOR2"), (0x1b8, "DMA_COLOR3"),
        (0x198, "DMA_ZETA"), (0x200, "RT_HORIZ"), (0x204, "RT_VERT"), (0x208, "RT_FORMAT"), (0x20c, "COLOR0_PITCH"),
        (0x210, "COLOR0_OFFSET"), (0x214, "ZETA_OFFSET"), (0x218, "COLOR1_OFFSET"), (0x21c, "COLOR1_PITCH"),
        (0x220, "RT_ENABLE"), (0x22c, "ZETA_PITCH"), (0x280, "COLOR2_PITCH"), (0x284, "COLOR3_PITCH"),
        (0x288, "COLOR2_OFFSET"), (0x28c, "COLOR3_OFFSET"), (0x2b8, "WINDOW_OFFSET"), (0x300, "DITHER_ENABLE"),
        (0x304, "ALPHA_TEST_ENABLE"), (0x308, "ALPHA_TEST_FUNC"), (0x30c, "ALPHA_TEST_REF"), (0x310, "BLEND_ENABLE"),
        (0x314, "BLEND_FUNC_SRC"), (0x318, "BLEND_FUNC_DST"), (0x31c, "BLEND_COLOR"), (0x320, "BLEND_EQUATION"),
        (0x324, "COLOR_MASK"), (0x328, "STENCIL_TEST_ENABLE"), (0x348, "TWO_SIDED_STENCIL_TEST_ENABLE"),
        (0x368, "SHADE_MODEL"), (0x36c, "BLEND_ENABLE_MRT"), (0x370, "MRT_COLOR_MASK"),
        (0x374, "COLOR_LOGIC_OP_ENABLE"), (0x378, "COLOR_LOGIC_OP"), (0x37c, "BLEND_COLOR2"),
        (0x380, "DEPTH_BOUNDS_TEST_ENABLE"), (0x394, "DEPTH_RANGE_NEAR"), (0x398, "DEPTH_RANGE_FAR"),
        (0x3b8, "LINE_WIDTH"), (0x8c0, "SCISSOR_HORIZ"), (0x8c4, "SCISSOR_VERT"), (0x8e4, "FP_ADDRESS"),
        (0xa00, "VIEWPORT_HORIZ"), (0xa04, "VIEWPORT_VERT"), (0xa6c, "DEPTH_FUNC"), (0xa70, "DEPTH_WRITE_ENABLE"),
        (0xa74, "DEPTH_TEST_ENABLE"), (0x181c, "VB_INDEX_BATCH_OFFSET"), (0x1820, "VB_INDEX_BATCH_DMA"),
        (0x1828, "POLYGON_MODE_FRONT"), (0x182c, "POLYGON_MODE_BACK"), (0x1830, "CULL_FACE"), (0x1834, "FRONT_FACE"),
        (0x183c, "CULL_FACE_ENABLE"), (0x1d60, "FP_CONTROL"), (0x1d84, "ZCULL_ENABLE"), (0x1d8c, "CLEAR_VALUE_DEPTH"),
        (0x1d90, "CLEAR_VALUE_COLOR"), (0x1dac, "RESTART_INDEX_ENABLE"), (0x1db0, "RESTART_INDEX"),
        (0x1e9c, "VP_UPLOAD_FROM_ID"), (0x1ea0, "VP_START_FROM_ID"), (0x1ee0, "POINT_SIZE"),
        (0x1efc, "VP_UPLOAD_CONST_ID"), (0x1fc0, "DIVIDER_FREQUENCY_OP"), (0x1ff0, "VP_ATTRIB_EN"),
        (0x1ff4, "VP_RESULT_EN"),
    ];
    if let Some((_, n)) = fixed.iter().find(|(m, _)| *m == method) {
        return (*n).into();
    }
    const TEX: [&str; 8] = ["TEX_OFFSET", "TEX_FORMAT", "TEX_WRAP", "TEX_ENABLE", "TEX_SWIZZLE", "TEX_FILTER",
        "TEX_SIZE0", "TEX_BORDER_COLOR"];
    if (0x1a00..0x1c00).contains(&method) {
        let k = ((method - 0x1a00) % 32 / 4) as usize;
        return format!("{}({})", TEX[k], (method - 0x1a00) / 32);
    }
    indexed(0x1840, 4, 16, "TEX_SIZE1")
        .or_else(|| indexed(0x1740, 4, 16, "VTXFMT"))
        .or_else(|| indexed(0x1680, 4, 16, "VTXBUF_ADDRESS"))
        .or_else(|| indexed(0xa20, 4, 4, "VIEWPORT_TRANSLATE"))
        .or_else(|| indexed(0xa30, 4, 4, "VIEWPORT_SCALE"))
        .unwrap_or_else(|| format!("{method:#06x}"))
}

/// The JSON of a state view: each table an object of field name to value (null: unknown).
pub fn view_json(v: &StateView) -> Json {
    let obj = |fields: &[Field]| {
        let mut m = Map::new();
        for f in fields {
            m.insert(f.name.clone(), f.value.json());
        }
        Json::Object(m)
    };
    json!({
        "render": obj(&v.render),
        "textures": v.textures.iter().map(|t| obj(t)).collect::<Vec<_>>(),
        "attributes": v.attributes.iter().map(|t| obj(t)).collect::<Vec<_>>(),
        "index": obj(&v.index),
        "surfaces": obj(&v.surfaces),
    })
}
