//! Reader-backed AUTO frame capture presentation; never decodes wire records.
use ps3dk_prof::{
    auto_rsx::{self, MapState},
    rsx_frame::{self as rf, FrameView, Profile},
    Analysis,
};
use std::{
    collections::BTreeMap,
    sync::{Arc, OnceLock},
};
pub type Names = BTreeMap<(Profile, u64), String>;
/// How far a program identity can be trusted, with the reason, when it is not exact.
pub fn provenance(p: &rf::Program) -> Option<(&'static str, &'static str)> {
    if p.unreadable {
        Some(("identity unavailable", "the container could not be read, or it was rewritten in place outside a capture; no hash is claimed"))
    } else if p.not_revalidated {
        Some(("bound before capture, not revalidated", "the identity comes from a bind made before the capture was armed; the container may have been rewritten in place since"))
    } else {
        None
    }
}
/// The display name with its provenance wherever a program identity is shown.
pub fn program_label(p: &rf::Program, names: &Names) -> String {
    let name = rf::program_name(p, names);
    match provenance(p) {
        Some((what, _)) => format!("{name} ({what})"),
        None => name,
    }
}
pub struct RsxFrameView {
    pub generation: u64,
    pub frames: Vec<FrameView>,
    pub map: Option<MapState>,
    pub hz: u64,
    pub available: bool,
}
impl RsxFrameView {
    pub fn build(a: &Analysis) -> Self {
        static NEXT: std::sync::atomic::AtomicU64 = std::sync::atomic::AtomicU64::new(1);
        Self {
            generation: NEXT.fetch_add(1, std::sync::atomic::Ordering::Relaxed),
            frames: rf::frames(&a.capture),
            map: a
                .capture
                .auto
                .as_ref()
                .filter(|_| a.capture.rsx_frame.is_some())
                .map(auto_rsx::qualify),
            hz: a.capture.clock.as_ref().map_or(0, |c| c.freq_hz),
            available: a.capture.rsx_frame.is_some(),
        }
    }
    pub fn json(&self, a: &Analysis) -> serde_json::Value {
        rf::to_json(&a.capture, &self.frames)
    }
    pub fn duration(&self, raw: Option<u64>) -> DurationText {
        duration_text(self.map.as_ref(), self.hz, raw)
    }
}
#[derive(Default)]
pub struct Slot(OnceLock<Arc<RsxFrameView>>);
impl Slot {
    pub fn get(&self, a: &Analysis) -> Arc<RsxFrameView> {
        self.0
            .get_or_init(|| Arc::new(RsxFrameView::build(a)))
            .clone()
    }
}
pub struct DurationText {
    pub text: String,
    pub hover: String,
}
pub fn duration_text(map: Option<&MapState>, hz: u64, raw: Option<u64>) -> DurationText {
    let Some(raw) = raw else {
        return DurationText {
            text: "unavailable".into(),
            hover: "No valid timed interval was recorded".into(),
        };
    };
    if let Some((lo, hi)) = map
        .and_then(|m| auto_rsx::raw_duration_ppu_bounds(m, raw))
        .filter(|_| hz > 0)
    {
        let ms = |t: u64| t as f64 * 1000.0 / hz as f64;
        DurationText {
            text: if lo == hi {
                format!("{:.3} ms", ms(lo))
            } else {
                format!("{:.3}..{:.3} ms", ms(lo), ms(hi))
            },
            hover: format!("{raw} raw RSX ticks; {lo}..{hi} PPU ticks from the qualified map"),
        }
    } else {
        let why = match map {
            Some(MapState::Unqualified(why)) => format!("clock map unqualified: {why}"),
            None => "clock map unqualified: no AUTO clock records".into(),
            _ => "PPU frequency unavailable".into(),
        };
        DurationText {
            text: format!("{raw} ticks"),
            hover: format!("{raw} raw RSX ticks; {why}; no millisecond conversion"),
        }
    }
}
#[derive(Debug, Clone, Copy, PartialEq, Eq, Default)]
pub enum DrawColumn {
    #[default]
    Time,
    Ordinal,
    Kind,
    Count,
    Gap,
}
pub fn sorted_draws(
    f: &FrameView,
    filter: &str,
    column: DrawColumn,
    descending: bool,
    names: &Names,
) -> Vec<usize> {
    let filter = filter.to_lowercase();
    let mut ids: Vec<_> = f
        .draws
        .iter()
        .enumerate()
        .filter(|(_, d)| {
            if filter.is_empty() {
                return true;
            }
            let prog = |id: Option<usize>| {
                id.and_then(|i| f.programs.get(i))
                    .map(|p| program_label(p, names))
                    .unwrap_or_default()
            };
            format!(
                "{} {} {} {} {} {} {}",
                d.raw.ordinal,
                d.raw.kind.name(),
                rf::primitive_name(d.raw.mode),
                d.label.as_deref().unwrap_or(""),
                prog(d.vp),
                prog(d.fp),
                d.raw.tag
            )
            .to_lowercase()
            .contains(&filter)
        })
        .map(|(i, _)| i)
        .collect();
    ids.sort_by(|&i, &j| {
        let (a, b) = (&f.draws[i], &f.draws[j]);
        let numeric = |a: Option<u64>, b: Option<u64>| match (a, b) {
            (None, None) => std::cmp::Ordering::Equal,
            (None, _) => std::cmp::Ordering::Greater,
            (_, None) => std::cmp::Ordering::Less,
            (Some(a), Some(b)) => {
                if descending {
                    b.cmp(&a)
                } else {
                    a.cmp(&b)
                }
            }
        };
        let cmp = match column {
            DrawColumn::Time => numeric(a.timing.interval(), b.timing.interval()),
            DrawColumn::Ordinal => numeric(Some(a.raw.ordinal as u64), Some(b.raw.ordinal as u64)),
            DrawColumn::Kind => {
                if descending {
                    b.raw.kind.name().cmp(a.raw.kind.name())
                } else {
                    a.raw.kind.name().cmp(b.raw.kind.name())
                }
            }
            DrawColumn::Count => numeric(Some(a.raw.count as u64), Some(b.raw.count as u64)),
            DrawColumn::Gap => numeric(a.gap, b.gap),
        };
        cmp.then(a.raw.ordinal.cmp(&b.raw.ordinal))
    });
    ids
}
