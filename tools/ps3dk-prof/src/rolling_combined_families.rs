//! Private continuous AUTO/FIFO integration; actual original support only.
use super::{Budget,Error,flags};
use crate::{auto_rsx::rolling as auto,fifo::rolling as fifo,reader::PpuState,ext::rolling as spu};
use super::family_positions as positions;
use std::collections::{BTreeMap,BTreeSet};

fn ck<T>(s:&'static str)->Result<T,Error>{Err(Error::Checkpoint(s))}
fn aerr(e:auto::Error)->Error{match e{auto::Error::Limit(s)=>Error::Limit(s),auto::Error::Malformed(m)=>Error::Malformed(m),
    auto::Error::Checkpoint(s)|auto::Error::Context(s)=>Error::Checkpoint(s),e=>Error::Auto(e)}}
fn ferr(e:fifo::Error)->Error{match e{fifo::Error::Limit(s)=>Error::Limit(s),fifo::Error::Malformed(m)=>Error::Malformed(m),
    fifo::Error::Checkpoint(s)|fifo::Error::Context(s)=>Error::Checkpoint(s),e=>Error::Fifo(e)}}
fn al(b:&Budget)->auto::Limits{let l=b.limits;auto::Limits{checkpoint_bytes:l.checkpoint_bytes,support_bytes:l.metadata_bytes,
    anchors:l.frame_runs,build_entries:l.frame_runs,membership_runs:l.frame_runs,retained_records:l.retained_rows,
    input_bytes:l.input_bytes,temporary_bytes:l.temporary_bytes,work_items:l.work_items}}
fn fl(b:&Budget)->fifo::Limits{let l=b.limits;fifo::Limits{checkpoint_bytes:l.checkpoint_bytes,membership_runs:l.frame_runs,
    retained_records:l.retained_rows,work_records:l.work_items}}
fn arun<T>(b:&mut Budget,f:impl FnOnce(auto::Limits,&mut auto::WorkBudget)->Result<T,auto::Error>)->Result<T,Error>{
    let l=al(b);let mut w=auto::WorkBudget::new(b.limits.work_items-b.work);let r=f(l,&mut w);b.charge(u128::from(w.used()))?;r.map_err(aerr)
}
fn frun<T>(b:&mut Budget,f:impl FnOnce(fifo::Limits,&mut fifo::WorkBudget)->Result<T,fifo::Error>)->Result<T,Error>{
    let l=fl(b);let mut w=fifo::WorkBudget::new(b.limits.work_items-b.work);let r=f(l,&mut w);b.charge(u128::from(w.used()))?;r.map_err(ferr)
}
fn arefs<'a>(header:&'a[u8;16],boot:&'a[auto::Input<'a>],p:&'a PpuState,rsx:&'a BTreeSet<u32>)->auto::Refs<'a>{
    auto::Refs{original_header:header,bootstrap:boot,capture:p.capture(),rsx_domains:rsx,declared:p.v1_refs().declared_incarnations}
}
fn frefs<'a>(p:&'a PpuState,rsx:&'a BTreeSet<u32>)->fifo::Refs<'a>{
    fifo::Refs{capture:p.capture(),rsx_domains:rsx,declared:p.v1_refs().declared_incarnations}
}
struct Window {begin:u64,closed:Option<u64>,first:u64,last:Option<u64>}
pub(super) struct Families {
    auto:Option<auto::CombinedAdapter>,fifo:Option<fifo::CombinedAdapter>,bootstrap:Vec<auto::Original>,
    rsx:BTreeSet<u32>,windows:BTreeMap<u64,Window>,closed:u64,pub restoring:bool,
}
pub(super) struct Saved {pub auto:Option<(Vec<u8>,u64)>,pub fifo:Option<(Vec<u8>,fifo::CombinedPosition)>}
pub(super) struct Proof {
    pub chunks:u128,pub bytes:u128,pub demands:Vec<positions::IntervalDemand>,
    pub groups:Vec<positions::FifoWindowGroup>,pub fifo_config:Option<u64>,pub fifo_final:Option<u64>,
}
impl Families {
    pub(super) fn new(header:&[u8;16],p:&PpuState,b:&mut Budget)->Result<Self,Error>{
        let rsx=BTreeSet::new();let boot=[];
        let auto=if flags(header)&128!=0{Some(arun(b,|l,w|auto::CombinedAdapter::new(arefs(header,&boot,p,&rsx),l,w))?)}else{None};
        let fifo=if flags(header)&64!=0{Some(fifo::CombinedAdapter::new(frefs(p,&rsx)).map_err(ferr)?)}else{None};
        Ok(Self{auto,fifo,bootstrap:Vec::new(),rsx,windows:BTreeMap::new(),closed:0,restoring:false})
    }
    pub(super) fn accept(&mut self,c:&crate::rolling::Chunk<'_>,header:&[u8;16],p:&PpuState,s:&mut spu::Adapter,b:&mut Budget)->Result<(),Error>{
        if matches!(c.ty,1|2|17){
            if self.bootstrap.len()>=3{return ck("combined bootstrap count");}
            b.bytes(c.payload.len())?;self.bootstrap.push(auto::Original{index:c.index,kind:c.ty,payload:c.payload.to_vec()});
        }
        if c.ty==8 {b.charge(s.combined_counts().0 as u128+1)?;self.rsx.extend(s.rsx_domains());}
        if c.ty==41 {
            let w=p.windows().and_then(|w|w.open.as_ref()).ok_or(Error::Checkpoint("accepted WINDOW_BEGIN"))?;
            if self.windows.len()>=b.limits.frame_runs{return Err(Error::Limit("window identities"));}
            self.windows.insert(u64::from(w.id),Window{begin:c.index,closed:None,first:w.first_frame,last:None});
        }
        if c.ty==42 {
            let w=p.windows().and_then(|w|w.windows.last()).ok_or(Error::Checkpoint("accepted WINDOW_END"))?;
            let v=self.windows.get_mut(&u64::from(w.id)).ok_or(Error::Checkpoint("WINDOW_END identity"))?;v.closed=Some(c.index);v.last=w.last_frame;
            self.closed=self.closed.checked_add(1).ok_or(Error::Limit("window count"))?;
            if let Some(f)=&mut self.fifo{frun(b,|l,m|f.accept(frefs(p,&self.rsx),fifo::Input::WindowClosed{index:c.index,window:w},l,m))?;}
        }
        let boot:Vec<_>=self.bootstrap.iter().map(|x|auto::Input{index:x.index,kind:x.kind,payload:&x.payload}).collect();
        if (34..=38).contains(&c.ty){
            let a=self.auto.as_mut().ok_or(Error::Unsupported{flags:flags(header),chunk:Some(c.ty)})?;
            if !(self.restoring&&c.ty==37){arun(b,|l,m|a.accept(arefs(header,&boot,p,&self.rsx),auto::Input{index:c.index,kind:c.ty,payload:c.payload},l,m))?;}
            if c.ty==34{if let Some(id)=a.summary().domain{s.bind_combined_domain(id)?;}}
        }
        if matches!(c.ty,31..=33|43){
            let f=self.fifo.as_mut().ok_or(Error::Unsupported{flags:flags(header),chunk:Some(c.ty)})?;
            if !(self.restoring&&c.ty==33){frun(b,|l,m|f.accept(frefs(p,&self.rsx),fifo::Input::Fifo{index:c.index,kind:c.ty,payload:c.payload,windows:p.windows()},l,m))?;}
            if c.ty==31{if let Some(id)=f.domain(){s.bind_combined_domain(id)?;}}
        }Ok(())
    }
    pub(super) fn sync(&mut self,header:&[u8;16],p:&PpuState,next:u64,b:&mut Budget)->Result<(),Error>{
        let boot:Vec<_>=self.bootstrap.iter().map(|x|auto::Input{index:x.index,kind:x.kind,payload:&x.payload}).collect();
        if let Some(a)=&mut self.auto{arun(b,|l,w|a.sync(arefs(header,&boot,p,&self.rsx),next,l,w))?;}
        if let Some(f)=&mut self.fifo{f.sync(frefs(p,&self.rsx),next).map_err(ferr)?;}Ok(())
    }
    pub(super) fn restore(&mut self,saved:Saved,header:&[u8;16],p:&PpuState,next:u64,metadata:&[Vec<u8>],b:&mut Budget)->Result<(),Error>{
        let boot:Vec<_>=self.bootstrap.iter().map(|x|auto::Input{index:x.index,kind:x.kind,payload:&x.payload}).collect();
        if self.auto.is_some()!=saved.auto.is_some() || self.fifo.is_some()!=saved.fifo.is_some(){return ck("combined family presence");}
        if let Some((bytes,chunks))=saved.auto{
            let a=arun(b,|l,w|auto::CombinedAdapter::import(arefs(header,&boot,p,&self.rsx),&bytes,next,chunks,l,w))?;
            let mut count=0usize;for x in a.support(){check_support(metadata,x.index,x.kind,&x.payload,b)?;count+=1;}
            if count!=metadata.iter().filter(|raw|matches!(u32_at(raw,0),34|36|37|38)).count(){return ck("AUTO shared support census");}
            self.auto=Some(a);
        }
        if let Some((bytes,position))=saved.fifo{
            let f=frun(b,|l,w|fifo::CombinedAdapter::import(frefs(p,&self.rsx),&bytes,next,position,self.closed,l,w))?;
            let support=frun(b,|l,w|f.support(l,w))?;
            for (i,t,payload) in &support{check_support(metadata,*i,*t,payload,b)?;}
            if support.len()!=metadata.iter().filter(|raw|matches!(u32_at(raw,0),31|33)).count(){return ck("FIFO shared support census");}
            self.fifo=Some(f);
        }
        self.restoring=false;self.validate_groups(b)?;Ok(())
    }
    fn validate_groups(&self,b:&mut Budget)->Result<(),Error>{
        if let Some(f)=&self.fifo{for g in &f.position().groups{
            b.charge(u128::from(usize::BITS-self.windows.len().leading_zeros())+1)?;
            if g.window!=0 {
                let w=self.windows.get(&g.window).ok_or(Error::Checkpoint("accounting undeclared window"))?;
                if g.first<w.first || w.last.is_some_and(|last|g.last>last){return ck("accounting window frame bounds");}
            }
        }}Ok(())
    }
    pub(super) fn mark(&mut self,b:&mut Budget)->Result<(),Error>{
        if let Some(a)=&mut self.auto{arun(b,|l,w|a.mark_retained(l,w))?;}
        if let Some(f)=&mut self.fifo{frun(b,|l,w|f.mark_retained(l,w))?;}Ok(())
    }
    pub(super) fn finish(self,b:&mut Budget)->Result<(Option<auto::Retained>,Option<fifo::Retained>,Option<auto::Report>),Error>{
        let a=self.auto.map(|a|arun(b,|l,w|a.finish(l,w))).transpose()?;
        let f=self.fifo.map(|f|frun(b,|l,w|f.finish(l,w))).transpose()?;
        let report=a.as_ref().map(|a|arun(b,|l,w|auto::analyze(a,f.as_ref(),l,w))).transpose()?;Ok((a,f,report))
    }
    pub(super) fn encode(&self,b:&mut Budget)->Result<(Vec<u8>,Vec<u8>),Error>{
        self.validate_groups(b)?;
        let mut a=Vec::new();if let Some(state)=&self.auto{
            let inner=arun(b,|l,w|state.encode(l,w))?;a=vec![0;32];put32(&mut a,0,u32::try_from(inner.len()).map_err(|_|Error::Limit("AUTO inner bytes"))?);
            put64(&mut a,8,state.summary().status_chunks);a.extend_from_slice(&inner);
        }
        let mut f=Vec::new();if let Some(state)=&self.fifo{
            let inner=frun(b,|l,w|state.encode(self.closed,l,w))?;let p=state.position();f=vec![0;64];
            put32(&mut f,0,u32::try_from(inner.len()).map_err(|_|Error::Limit("FIFO inner bytes"))?);
            put32(&mut f,4,u32::try_from(p.groups.len()).map_err(|_|Error::Limit("FIFO groups"))?);
            for (at,n) in [(8,p.config),(16,p.status),(24,p.event_chunks),(32,p.status_chunks),(40,p.chunks),(48,p.bytes)]{put64(&mut f,at,n);}
            f.extend_from_slice(&inner);for g in &p.groups{for n in [g.window,g.rows,g.first,g.last]{f.extend_from_slice(&n.to_le_bytes());}}
        }
        if a.len().checked_add(f.len()).is_none_or(|n|n>b.limits.checkpoint_bytes){return Err(Error::Limit("checkpoint bytes"));}
        b.bytes(a.len())?;b.bytes(f.len())?;Ok((a,f))
    }
    pub(super) fn storage(&self)->u128{
        self.auto.as_ref().map_or(0,|a|a.storage())+self.fifo.as_ref().map_or(0,|f|f.storage())
            +self.bootstrap.iter().map(|x|x.payload.len() as u128+64).sum::<u128>()+128*(self.windows.len()+self.rsx.len()) as u128
    }
    pub(super) fn storage_work(&self)->u128{
        self.auto.as_ref().map_or(0,|a|a.storage_work())+self.bootstrap.len() as u128+1
    }
    pub(super) fn analysis_scratch(&self)->Result<u128,Error>{
        let fifo=self.fifo.as_ref().map_or(0,|f|f.analysis_items());
        self.auto.as_ref().map_or(Ok(0),|a|a.analysis_scratch(fifo).map_err(aerr))
    }
    pub(super) fn retained_rows(&self)->u128{
        self.auto.as_ref().map_or(0,|a|a.retained_rows() as u128)+self.fifo.as_ref().map_or(0,|f|f.retained_rows() as u128)
    }
    pub(super) fn proof(&self,next:u64,b:&mut Budget)->Result<Proof,Error>{
        self.validate_groups(b)?;let mut out=Proof{chunks:0,bytes:0,demands:Vec::new(),groups:Vec::new(),fifo_config:None,fifo_final:None};
        let groups=self.fifo.as_ref().map_or(0,|f|f.position().groups.len());
        if groups.checked_add(7).is_none_or(|n|n>b.limits.position_temporary_items){return Err(Error::Limit("family position temporary entries"));}
        out.demands.try_reserve_exact(7).map_err(|_|Error::Limit("family demand allocation"))?;
        out.groups.try_reserve_exact(groups).map_err(|_|Error::Limit("family group allocation"))?;
        if let Some(a)=&self.auto{
            let s=a.summary();b.charge(16)?;
            if let Some(config)=s.config{lists(&mut out.demands,config,s.status,s.counts.op_chunks,s.status_chunks,next)?;statuses(&mut out.demands,config,s.status,s.counts.statuses)?;}
            let old=s.counts.statuses.saturating_sub(1);out.chunks+=u128::from(s.counts.op_chunks)+u128::from(old);
            out.bytes+=36*u128::from(s.counts.op_chunks)+u128::from(s.row_width)*u128::from(s.counts.ops)+(20+u128::from(s.status_width))*u128::from(old);
            if s.final_seen && (s.status_ops!=s.counts.ops || s.status_chunks!=s.counts.op_chunks){return ck("AUTO post-final demand");}
        }
        if let Some(f)=&self.fifo{
            let p=f.position();let n=f.counts();b.charge(16+p.groups.len() as u128)?;
            let status=f.status().map(|_|p.status);let config=f.domain().map(|_|p.config);
            if let Some(config)=config {
                lists(&mut out.demands,config,status,p.event_chunks,p.status_chunks,next)?;statuses(&mut out.demands,config,status,n.statuses)?;
                let final_index=f.status().filter(|s|s.is_final()).map(|_|p.status);
                if final_index.is_some() && p.status_chunks!=p.event_chunks{return ck("FIFO post-final demand");}
                for g in &p.groups {
                    if g.window==0{out.demands.push(positions::IntervalDemand{start:config.checked_add(1).ok_or(Error::Checkpoint("FIFO release overflow"))?,end:final_index.unwrap_or(next),chunks:g.rows});}
                    else{let w=self.windows.get(&g.window).ok_or(Error::Checkpoint("FIFO window position"))?;
                        out.groups.push(positions::FifoWindowGroup{window:g.window,begin:w.begin,closed:w.closed,rows:g.rows});}
                }
                if !out.groups.is_empty(){out.fifo_config=Some(config);out.fifo_final=final_index;}
            }
            let old=n.statuses.saturating_sub(1);out.chunks+=u128::from(p.event_chunks)+u128::from(old)+u128::from(n.accounting_rows);
            out.bytes+=36*u128::from(p.event_chunks)+96*u128::from(n.events)+100*u128::from(old)+100*u128::from(n.accounting_rows);
        }Ok(out)
    }
}
fn lists(out:&mut Vec<positions::IntervalDemand>,config:u64,status:Option<u64>,chunks:u64,before:u64,next:u64)->Result<(),Error>{
    let start=config.checked_add(1).ok_or(Error::Checkpoint("CONFIG release overflow"))?;
    if let Some(index)=status{
        let after=chunks.checked_sub(before).ok_or(Error::Checkpoint("STATUS list snapshot"))?;
        out.push(positions::IntervalDemand{start,end:index,chunks:before});
        out.push(positions::IntervalDemand{start:index.checked_add(1).ok_or(Error::Checkpoint("STATUS release overflow"))?,end:next,chunks:after});
    }else{if before!=0{return ck("absent STATUS snapshot");}out.push(positions::IntervalDemand{start,end:next,chunks});}Ok(())
}
fn statuses(out:&mut Vec<positions::IntervalDemand>,config:u64,status:Option<u64>,count:u64)->Result<(),Error>{
    if let Some(end)=status{let chunks=count.checked_sub(1).ok_or(Error::Checkpoint("STATUS count"))?;
        out.push(positions::IntervalDemand{start:config.checked_add(1).ok_or(Error::Checkpoint("CONFIG release overflow"))?,end,chunks});
    }else if count!=0{return ck("absent STATUS count");}Ok(())
}
fn check_support(metadata:&[Vec<u8>],index:u64,kind:u32,payload:&[u8],b:&mut Budget)->Result<(),Error>{
    b.charge(u128::from(usize::BITS-metadata.len().leading_zeros())+payload.len() as u128+1)?;
    let at=metadata.binary_search_by_key(&index,|raw|u64_at(raw,8)).map_err(|_|Error::Checkpoint("missing shared family support"))?;
    let raw=&metadata[at];if u32_at(raw,0)!=kind || &raw[16..raw.len()-4]!=payload{return ck("family/shared original support mismatch");}Ok(())
}
fn u32_at(b:&[u8],at:usize)->u32{u32::from_le_bytes(b[at..at+4].try_into().unwrap())}
fn u64_at(b:&[u8],at:usize)->u64{u64::from_le_bytes(b[at..at+8].try_into().unwrap())}
fn put32(b:&mut[u8],at:usize,n:u32){b[at..at+4].copy_from_slice(&n.to_le_bytes());}
fn put64(b:&mut[u8],at:usize,n:u64){b[at..at+8].copy_from_slice(&n.to_le_bytes());}

pub(super) fn decode(auto_section:(usize,&[u8]),fifo_section:(usize,&[u8]),header:&[u8;16],b:&mut Budget)->Result<Saved,Error>{
    let mut out=Saved{auto:None,fifo:None};
    for (bit,(count,bytes)) in [(128,auto_section),(64,fifo_section)]{
        if flags(header)&bit==0 {if count!=0 || !bytes.is_empty(){return ck("unadvertised family section");}continue;}
        if count!=1{return ck("family section count");}
        let fixed=if bit==128{32}else{64};if bytes.len()<fixed{return ck("short family wrapper");}
        let inner=u32_at(bytes,0) as usize;
        if bit==128{
            if u32_at(bytes,4)!=0 || bytes[16..32].iter().any(|x|*x!=0) || inner!=bytes.len()-32{return ck("AUTO wrapper fields");}
            b.bytes(inner)?;out.auto=Some((bytes[32..].to_vec(),u64_at(bytes,8)));
        }else{
            let count=u32_at(bytes,4) as usize;if count>b.limits.frame_runs{return Err(Error::Limit("accounting window groups"));}
            if u64_at(bytes,56)!=0 || 64u128+inner as u128+32*count as u128!=bytes.len() as u128{return ck("FIFO wrapper fields");}
            b.charge(bytes.len() as u128+count as u128)?;
            let mut position=fifo::CombinedPosition{config:u64_at(bytes,8),status:u64_at(bytes,16),event_chunks:u64_at(bytes,24),
                status_chunks:u64_at(bytes,32),chunks:u64_at(bytes,40),bytes:u64_at(bytes,48),groups:Vec::new()};
            position.groups.try_reserve_exact(count).map_err(|_|Error::Limit("accounting group allocation"))?;
            for raw in bytes[64+inner..].chunks_exact(32){position.groups.push(fifo::AccountingGroup{window:u64_at(raw,0),rows:u64_at(raw,8),first:u64_at(raw,16),last:u64_at(raw,24)});}
            out.fifo=Some((bytes[64..64+inner].to_vec(),position));
        }
    }Ok(out)
}
