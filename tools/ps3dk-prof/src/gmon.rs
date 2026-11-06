//! Lossless inputs, explicitly quantized instrumented-time gmon compatibility output.
//! Stock gprof symbolication still needs the compact-OPD toolchain fix.
use crate::{companions::VerifiedPair, model::Verdict};
use object::{Object, ObjectSection, ObjectSymbol};
use std::collections::BTreeMap;

#[derive(Debug, Clone)]
pub struct GmonImage {
    build: Vec<u8>,
    abi: u8,
    opd_address: u64,
    opd: Vec<u8>,
    ranges: Vec<(u64, u64)>,
    functions: Vec<(u64, u64)>,
}
impl GmonImage {
    /// Both PPU data models use ELF64 BE with e_flags=0. ABI is bound through the
    /// capture/build identity, never guessed from ELF class or compact OPD width.
    pub fn read(bytes: &[u8], pair: &VerifiedPair) -> Result<Self, String> {
        let h = pair.arcs.header().ok_or("missing header")?;
        let f = object::File::parse(bytes).map_err(|e| e.to_string())?;
        if !f.is_64()
            || f.is_little_endian()
            || f.architecture() != object::Architecture::PowerPc64
            || f.kind() != object::ObjectKind::Executable
        {
            return Err("expected PPU ELF64 BE executable".into());
        }
        if h.build_id.is_empty()
            || f.build_id().map_err(|e| e.to_string())? != Some(h.build_id.as_slice())
        {
            return Err("ELF build identity mismatch or absent".into());
        }
        let opd = f.section_by_name(".opd").ok_or("missing compact OPD")?;
        let ranges = f
            .sections()
            .filter(|s| matches!(s.flags(),object::SectionFlags::Elf {sh_flags} if sh_flags&4!=0))
            .map(|s| {
                s.address()
                    .checked_add(s.size())
                    .map(|end| (s.address(), end))
                    .ok_or("ELF section overflow")
            })
            .collect::<Result<Vec<_>, _>>()?;
        let mut image = Self {
            build: h.build_id.clone(),
            abi: h.abi,
            opd_address: opd.address(),
            opd: opd.data().map_err(|e| e.to_string())?.to_vec(),
            ranges,
            functions: vec![],
        };
        let mut starts = BTreeMap::<u64, Option<u64>>::new();
        for s in f
            .symbols()
            .filter(|s| s.kind() == object::SymbolKind::Text && !s.is_undefined())
        {
            let address = s.address();
            if image.executable(address) {
                let end = if s.size() == 0 {
                    None
                } else {
                    Some(
                        address
                            .checked_add(s.size())
                            .ok_or("symbol extent overflow")?,
                    )
                };
                let old = starts.entry(address).or_insert(end);
                if *old != end {
                    *old = None;
                }
            } else if let Ok(entry) = image.descriptor(address) {
                starts.entry(entry).or_insert(None);
            }
        }
        let keys = starts.keys().copied().collect::<Vec<_>>();
        for (i, start) in keys.iter().copied().enumerate() {
            let section_end = image
                .ranges
                .iter()
                .find(|&&(lo, hi)| start >= lo && start < hi)
                .unwrap()
                .1;
            let next = keys
                .get(i + 1)
                .copied()
                .unwrap_or(section_end)
                .min(section_end);
            let end = starts[&start].unwrap_or(next);
            if start % 4 != 0 || end > next || end <= start || end - start < 4 {
                return Err("ambiguous or invalid function range".into());
            }
            image.functions.push((start, end));
        }
        if image.functions.is_empty() {
            return Err("no executable function symbols".into());
        }
        Ok(image)
    }
    fn executable(&self, pc: u64) -> bool {
        pc.is_multiple_of(4)
            && self
                .ranges
                .iter()
                .any(|&(a, b)| pc >= a && pc.checked_add(4).is_some_and(|end| end <= b))
    }
    fn descriptor(&self, address: u64) -> Result<u64, String> {
        let relative = address
            .checked_sub(self.opd_address)
            .ok_or("descriptor before OPD")?;
        if relative % 8 != 0 {
            return Err("descriptor not at compact OPD entry boundary".into());
        }
        let off = usize::try_from(relative).map_err(|_| "descriptor offset overflow")?;
        let entry = self
            .opd
            .get(off..off.checked_add(8).ok_or("OPD offset overflow")?)
            .ok_or("descriptor outside OPD")?;
        let pc = u64::from(u32::from_be_bytes(entry[..4].try_into().unwrap()));
        if !self.executable(pc) {
            return Err("descriptor entry is not executable".into());
        }
        Ok(pc)
    }
    fn return_pc(&self, pc: u64) -> Result<u64, String> {
        let instruction = pc.checked_sub(4).ok_or("invalid return PC")?;
        self.functions
            .iter()
            .find(|&&(lo, hi)| instruction >= lo && instruction < hi)
            .map(|&(lo, _)| lo)
            .ok_or_else(|| "unmapped code return PC".into())
    }
}

#[derive(Debug, Clone)]
pub struct FunctionTime {
    pub incarnation: u32,
    pub entry: u64,
    pub inclusive_ticks: u128,
    pub exclusive_ticks: u128,
    pub logical_spans: u64,
}
#[derive(Debug, Clone)]
pub struct FunctionArc {
    pub incarnation: u32,
    pub caller: u64,
    pub callee: u64,
    pub count: u128,
}
#[derive(Debug, Clone)]
pub struct FunctionProjection {
    pair: VerifiedPair,
    functions: BTreeMap<(u32, u64), FunctionTime>,
    arcs: Vec<FunctionArc>,
}
impl FunctionProjection {
    pub fn function(&self, inc: u32, pc: u64) -> Option<&FunctionTime> {
        self.functions.get(&(inc, pc))
    }
    pub fn functions(&self) -> &BTreeMap<(u32, u64), FunctionTime> {
        &self.functions
    }
    pub fn arcs(&self) -> &[FunctionArc] {
        &self.arcs
    }
    pub fn new(pair: VerifiedPair, image: &GmonImage) -> Result<Self, String> {
        if !pair.is_final() || pair.timing.verdict != Verdict::Complete {
            return Err("exact function projection requires final complete companions".into());
        }
        let h = pair.arcs.header().ok_or("missing header")?;
        if h.build_id != image.build || h.abi != image.abi {
            return Err("image/capture identity mismatch".into());
        }
        let c = &pair.timing.capture;
        let mut automatic = BTreeMap::new();
        for (id, d) in &c.descriptors {
            if d.logical_tag.starts_with("ps3prof:auto:") || d.name.starts_with("ppu-fn-opd:") {
                let suffix = d
                    .name
                    .strip_prefix("ppu-fn-opd:")
                    .ok_or("invalid automatic descriptor name")?;
                if d.logical_tag != "ps3prof:auto:cyg:v1"
                    || d.scope != 1
                    || !d.file.is_empty()
                    || d.line != 0
                    || suffix.len() != 16
                    || !suffix
                        .bytes()
                        .all(|b| b.is_ascii_digit() || (b'a'..=b'f').contains(&b))
                {
                    return Err("invalid automatic descriptor namespace".into());
                }
                let address =
                    u64::from_str_radix(suffix, 16).map_err(|_| "invalid descriptor hex")?;
                if address == 0 {
                    return Err("zero function descriptor".into());
                }
                let entry = image.descriptor(address)?;
                if !image.functions.iter().any(|&(lo, _)| lo == entry) {
                    return Err("automatic descriptor has no canonical function".into());
                }
                automatic.insert(*id, entry);
            }
        }
        let by_id = c
            .spans
            .iter()
            .map(|s| ((s.incarnation, s.span_id), s))
            .collect::<BTreeMap<_, _>>();
        let mut child_ticks = BTreeMap::<(u32, u64), u128>::new();
        for s in c.spans.iter().filter(|s| automatic.contains_key(&s.desc)) {
            if s.is_censored() || s.affected || !pair.timing.lane_valid(s.incarnation) {
                return Err("automatic span is censored/affected or lane invalid".into());
            }
            let ticks = s.inclusive().ok_or("automatic span clock anomaly")?;
            let mut parent = s.parent;
            let mut hops = 0;
            while parent != 0 {
                hops += 1;
                if hops > c.spans.len() {
                    return Err("span ancestor cycle".into());
                }
                let p = by_id
                    .get(&(s.incarnation, parent))
                    .ok_or("missing span ancestor")?;
                if p.begin_tb > s.begin_tb || p.end_tb.zip(s.end_tb).is_none_or(|(pe, se)| pe < se)
                {
                    return Err("span clock containment anomaly".into());
                }
                if automatic.contains_key(&p.desc) {
                    *child_ticks.entry((s.incarnation, parent)).or_default() += u128::from(ticks);
                    break;
                }
                parent = p.parent;
            }
        }
        let mut functions = BTreeMap::new();
        for s in &c.spans {
            if let Some(&entry) = automatic.get(&s.desc) {
                let inc = u128::from(s.inclusive().ok_or("automatic timing unavailable")?);
                let exclusive = inc
                    .checked_sub(
                        child_ticks
                            .get(&(s.incarnation, s.span_id))
                            .copied()
                            .unwrap_or(0),
                    )
                    .ok_or("automatic children exceed parent")?;
                let row = functions
                    .entry((s.incarnation, entry))
                    .or_insert(FunctionTime {
                        incarnation: s.incarnation,
                        entry,
                        inclusive_ticks: 0,
                        exclusive_ticks: 0,
                        logical_spans: 0,
                    });
                row.inclusive_ticks = row
                    .inclusive_ticks
                    .checked_add(inc)
                    .ok_or("inclusive overflow")?;
                row.exclusive_ticks = row
                    .exclusive_ticks
                    .checked_add(exclusive)
                    .ok_or("exclusive overflow")?;
                row.logical_spans = row
                    .logical_spans
                    .checked_add(1)
                    .ok_or("logical count overflow")?;
            }
        }
        let mut arcs = BTreeMap::<(u32, u64, u64), u128>::new();
        for a in pair.arcs.entries() {
            if a.unknown_caller {
                return Err("unknown caller prevents exact graph export".into());
            }
            let key = (
                a.incarnation,
                image.return_pc(a.caller_return_pc)?,
                image.return_pc(a.callee_hook_pc)?,
            );
            let v = arcs.entry(key).or_default();
            *v = v
                .checked_add(u128::from(a.count))
                .ok_or("canonical arc overflow")?;
        }
        Ok(Self {
            pair,
            functions,
            arcs: arcs
                .into_iter()
                .map(|((incarnation, caller, callee), count)| FunctionArc {
                    incarnation,
                    caller,
                    callee,
                    count,
                })
                .collect(),
        })
    }
}
pub struct GmonOptions {
    pub incarnation: u32,
    pub rate: u32,
}
#[derive(Debug)]
pub struct GmonExport {
    pub bytes: Vec<u8>,
    pub sidecar: String,
}
pub fn export_gmon(p: &FunctionProjection, o: &GmonOptions) -> Result<GmonExport, String> {
    if !(1..=1_000_000).contains(&o.rate) {
        return Err("rate outside 1..1000000".into());
    }
    if !p.pair.arcs.threads().contains_key(&o.incarnation) {
        return Err("unknown thread incarnation".into());
    }
    // A physical callee without an automatic span has unavailable timing, not a
    // zero-duration bin. An independently known zero-call lane remains valid.
    if p.arcs.iter().any(|a| {
        a.incarnation == o.incarnation && !p.functions.contains_key(&(o.incarnation, a.callee))
    }) {
        return Err("physical callee has no automatic timing in selected incarnation".into());
    }
    let clock = p.pair.arcs.clock().ok_or("missing clock")?;
    let mut bytes = b"gmon\0\0\0\x01\0\0\0\0\0\0\0\0\0\0\0\0".to_vec();
    for row in p
        .functions
        .values()
        .filter(|f| f.incarnation == o.incarnation)
    {
        let scaled = row
            .exclusive_ticks
            .checked_mul(u128::from(o.rate))
            .ok_or("timing product overflow")?;
        let freq = u128::from(clock.freq_hz);
        let units = (scaled / freq) + (u128::from(scaled % freq >= freq.div_ceil(2)));
        let bin = u16::try_from(units)
            .map_err(|_| "instrumented time exceeds u16 bin; choose explicit lower rate")?;
        bytes.push(0);
        bytes.extend(row.entry.to_be_bytes());
        bytes.extend(
            row.entry
                .checked_add(4)
                .ok_or("histogram address overflow")?
                .to_be_bytes(),
        );
        bytes.extend(1u32.to_be_bytes());
        bytes.extend(o.rate.to_be_bytes());
        let mut dimension = [0u8; 15];
        dimension[..7].copy_from_slice(b"seconds");
        bytes.extend(dimension);
        bytes.push(b's');
        bytes.extend(bin.to_be_bytes());
    }
    for arc in p.arcs.iter().filter(|a| a.incarnation == o.incarnation) {
        let count = u32::try_from(arc.count)
            .map_err(|_| "summed canonical arc count exceeds u32; exact originals retained")?;
        bytes.push(1);
        bytes.extend(arc.caller.to_be_bytes());
        bytes.extend(arc.callee.to_be_bytes());
        bytes.extend(count.to_be_bytes());
    }
    let h = p.pair.arcs.header().unwrap();
    let build = h
        .build_id
        .iter()
        .map(|b| format!("{b:02x}"))
        .collect::<String>();
    let sidecar=format!("instrumented elapsed timing (not PC sampling)\nmode=dual-hooks abi={} session={} snapshot={} build={} thread={} frequency={} rate={}\nrounding=nearest ties-up; error <= 0.5/rate seconds per function\nlogical cyg spans and physical mcount calls are distinct; no invented arcs\nelapsed includes instrumentation/preemption and compiler hook-boundary effects\ngprof propagated graph time is not exact per-edge time; stock sampling wording does not apply\npreserve original .prof and .arcs; no full symbolication compatibility claim pending compact-OPD fix\n",h.abi,h.session_id,h.snapshot_id,build,o.incarnation,clock.freq_hz,o.rate);
    Ok(GmonExport { bytes, sidecar })
}
