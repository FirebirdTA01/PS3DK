//! Pure generic source-position interface for combined AUTO/FIFO restart.
//! Bounded scheduling preserves original source positions without a drain ledger.
#[path = "rolling_combined_positions.rs"]
mod core;
pub use core::{Limits, SpuBlock};

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct IntervalDemand { pub start: u64, pub end: u64, pub chunks: u64 }

/// Actual declared window support. Closing a window constrains frame identities,
/// not the source position of a later accounting row referring to that window.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct FifoWindowGroup {
    pub window: u64,
    pub begin: u64,
    pub closed: Option<u64>,
    pub rows: u64,
}

#[derive(Clone, Copy, Debug)]
pub struct WindowedFifo<'a> {
    pub config: u64,
    pub final_index: Option<u64>,
    pub groups: &'a [FifoWindowGroup],
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Error {
    Invalid(&'static str),
    Infeasible,
    Limit(&'static str),
}

struct Work { left:u64 }
impl Work {
    fn charge(&mut self,n:u64)->Result<(),Error>{
        self.left=self.left.checked_sub(n).ok_or(Error::Limit("family position work"))?;Ok(())
    }
    fn anchor(&mut self,metadata:&[u64],index:u64)->Result<(),Error>{
        self.charge(u64::from(usize::BITS-metadata.len().leading_zeros())+1)?;
        if metadata.binary_search(&index).is_err(){return Err(Error::Invalid("missing family metadata anchor"));}Ok(())
    }
}

/// Full disjoint occupancy of [0, frontier), with generic interval demands and
/// FIFO accounting groups in window order. Caller checks frame/value predicates.
/// CONFIG/window/FINAL positions must name actual distinct pinned metadata.
pub fn verify_family_positions(
    frontier: u64,
    metadata: &[u64],
    demands: &[IntervalDemand],
    spu: &[SpuBlock],
    fifo: Option<WindowedFifo<'_>>,
    limits: Limits,
) -> Result<(), Error> {
    let groups=fifo.map_or(0,|f|f.groups.len());
    let copied=demands.len().checked_add(groups).ok_or(Error::Limit("family position demands"))?;
    let anchors=spu.len().checked_mul(2).and_then(|n|n.checked_add(metadata.len())).ok_or(Error::Limit("family position anchors"))?;
    let inner=spu.len().checked_add(copied).and_then(|n|n.checked_mul(2)).and_then(|n|n.checked_add(anchors))
        .ok_or(Error::Limit("family position temporary entries"))?;
    if inner.checked_add(copied).is_none_or(|n|n>limits.max_temporary_items){return Err(Error::Limit("family position temporary entries"));}
    let mut work=Work{left:limits.max_work};work.charge(1)?;
    let mut previous=None;
    for &i in metadata {work.charge(1)?;if i>=frontier || previous.is_some_and(|p|p>=i){return Err(Error::Invalid("metadata index/order"));}previous=Some(i);}
    // Bound storage before allocation; row/chunk multiplicities never allocate.
    let mut all=Vec::new();all.try_reserve_exact(copied).map_err(|_|Error::Limit("family demand allocation"))?;
    for d in demands {work.charge(1)?;all.push(core::PpuDemand{start:d.start,end:d.end,chunks:d.chunks});}
    if let Some(f)=fifo {
        work.anchor(metadata,f.config)?;
        let end=f.final_index.unwrap_or(frontier);
        if let Some(final_index)=f.final_index {
            work.anchor(metadata,final_index)?;
            if final_index<=f.config{return Err(Error::Invalid("FIFO FINAL chronology"));}
        }
        let mut previous:Option<FifoWindowGroup>=None;
        for &g in f.groups {
            work.charge(1)?;work.anchor(metadata,g.begin)?;
            if g.window==0 || g.rows==0 || g.begin==f.config || Some(g.begin)==f.final_index
                || previous.is_some_and(|p|p.window>=g.window || p.closed.is_none_or(|i|i>=g.begin)){
                return Err(Error::Invalid("FIFO window order/identity"));
            }
            if let Some(closed)=g.closed {
                work.anchor(metadata,closed)?;
                if closed<=g.begin || closed==f.config || Some(closed)==f.final_index{return Err(Error::Invalid("FIFO window close anchor"));}
            }
            let start=f.config.max(g.begin).checked_add(1).ok_or(Error::Invalid("FIFO release overflow"))?;
            if start>end{return Err(Error::Infeasible);}
            // All groups have this common deadline and nondecreasing releases.
            // Any inverted pair of assigned group slots can be exchanged:
            // earlier group moves earlier, later group later, both stay legal.
            // Thus EDF existence also admits original window order. WINDOW_END
            // constrains frame values in the caller, never this source deadline.
            all.push(core::PpuDemand{start,end,chunks:g.rows});previous=Some(g);
        }
    }
    core::verify_positions(frontier,metadata,&all,spu,Limits{
        max_temporary_items:limits.max_temporary_items-copied,max_work:work.left,
    }).map_err(|e|match e {core::Error::Invalid(s)=>Error::Invalid(s),core::Error::Infeasible=>Error::Infeasible,core::Error::Limit(s)=>Error::Limit(s)})
}
