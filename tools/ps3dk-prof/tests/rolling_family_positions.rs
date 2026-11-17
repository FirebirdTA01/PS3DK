//! Independent tiny original-slot assignments, including FIFO window order.
#[path = "../src/rolling_family_positions.rs"]
mod positions;
use positions::{verify_family_positions, Error, FifoWindowGroup as G, IntervalDemand as D,
    Limits, SpuBlock as S, WindowedFifo as F};

fn check(n:u64,m:&[u64],d:&[D],s:&[S],f:Option<F<'_>>)->Result<(),Error>{
    verify_family_positions(n,m,d,s,f,Limits::default())
}
fn d(start:u64,end:u64,chunks:u64)->D{D{start,end,chunks}}

#[test]
fn generic_families_share_free_slots_inside_one_spu_block(){
    assert_eq!(check(7,&[0,3],&[d(1,6,1),d(2,7,1)],&[S{first:1,last:6,chunks:3}],None),Ok(()));
    assert_eq!(check(7,&[0,3],&[d(1,2,1),d(2,7,1)],&[S{first:1,last:6,chunks:3}],None),Err(Error::Infeasible));
}

#[test]
fn closed_window_rows_can_occupy_later_source_slots(){
    let groups=[G{window:1,begin:1,closed:Some(2),rows:2}];
    let f=F{config:0,final_index:Some(5),groups:&groups};
    assert_eq!(check(6,&[0,1,2,5],&[],&[],Some(f)),Ok(()));
    // Both accounting slots3/4 are after WINDOW_END2, before the family FINAL5.
    let blocked=F{final_index:Some(3),..f};
    assert_eq!(check(6,&[0,1,2,3],&[],&[],Some(blocked)),Err(Error::Infeasible));
}

#[test]
fn later_window_release_prevents_borrowing_an_earlier_hole(){
    let groups=[G{window:2,begin:4,closed:None,rows:2}];
    assert_eq!(check(7,&[0,1,4],&[d(2,4,2)],&[],Some(F{config:0,final_index:None,groups:&groups})),Ok(()));
    assert_eq!(check(7,&[0,4,6],&[d(1,4,2)],&[],Some(F{config:0,final_index:None,groups:&groups})),Err(Error::Infeasible));
}

#[test]
fn common_deadline_allows_window_order_without_per_row_positions(){
    let groups=[G{window:1,begin:1,closed:Some(2),rows:1},G{window:4,begin:3,closed:None,rows:1}];
    assert_eq!(check(7,&[0,1,2,3,6],&[],&[],Some(F{config:0,final_index:Some(6),groups:&groups})),Ok(()));
    let swapped=[groups[1],groups[0]];
    assert!(matches!(check(7,&[0,1,2,3,6],&[],&[],Some(F{config:0,final_index:Some(6),groups:&swapped})),Err(Error::Invalid(_))));
}

#[test]
fn status_split_demands_compete_jointly_with_metadata_and_other_families(){
    // CONFIG0, STATUS3, other metadata6. Before/after demands cannot cross STATUS.
    assert_eq!(check(7,&[0,3,6],&[d(1,3,2),d(4,7,2)],&[],None),Ok(()));
    assert_eq!(check(7,&[0,2,3,6],&[d(1,3,2),d(4,7,1)],&[],None),Err(Error::Infeasible));
    assert_eq!(check(7,&[0,3,6],&[d(1,3,1),d(4,7,2)],&[],None),Err(Error::Infeasible));
}

#[test]
fn real_window_config_and_final_anchors_are_required(){
    let g=[G{window:1,begin:1,closed:Some(2),rows:1}];
    let f=F{config:0,final_index:Some(4),groups:&g};
    for metadata in [&[1,2,4][..],&[0,2,4][..],&[0,1,4][..],&[0,1,2][..]]{
        assert!(matches!(check(5,metadata,&[d(0,5,1)],&[],Some(f)),Err(Error::Invalid(_))));
    }
    assert_eq!(check(5,&[0,1,2,4],&[],&[],Some(f)),Ok(()));
}

#[test]
fn empty_and_invalid_groups_remain_distinct_from_infeasible_counts(){
    assert_eq!(check(0,&[],&[],&[],None),Ok(()));
    assert_eq!(check(1,&[0],&[],&[],Some(F{config:0,final_index:None,groups:&[]})),Ok(()));
    for g in [G{window:0,begin:1,closed:None,rows:1},G{window:1,begin:1,closed:Some(1),rows:1},
        G{window:1,begin:1,closed:None,rows:0},G{window:1,begin:4,closed:None,rows:1}]{
        assert!(matches!(check(4,&[0,1],&[],&[],Some(F{config:0,final_index:None,groups:&[g]})),Err(Error::Invalid(_))));
    }
}

#[test]
fn huge_multiplicity_is_bounded_by_structure_and_u64_endpoints(){
    let n=u64::MAX;let groups=[G{window:1,begin:1,closed:Some(2),rows:n-3}];
    let f=F{config:0,final_index:None,groups:&groups};
    let l=Limits{max_temporary_items:32,max_work:4096};
    assert_eq!(verify_family_positions(n,&[0,1,2],&[],&[],Some(f),l),Ok(()));
    assert!(matches!(verify_family_positions(n,&[0,1,2],&[d(0,n,1)],&[],Some(f),l),Err(Error::Invalid(_))));
    assert!(matches!(verify_family_positions(n,&[0,1,2],&[],&[],Some(f),Limits{max_temporary_items:0,..l}),Err(Error::Limit(_))));
    assert!(matches!(verify_family_positions(n,&[0,1,2],&[],&[],Some(f),Limits{max_work:0,..l}),Err(Error::Limit(_))));
}

// Exhaustive original-position labeling: no free-rank compression, greedy
// deadlines, production helper calls or expansion outside a tiny finite bound.
fn enumerate(n:u64,m:&[u64],d:&[D],s:&[S],f:Option<F<'_>>)->bool{
    assert!(n<=7);let mut fixed=vec![false;n as usize];let mut previous=None;
    for &i in m {if i>=n || previous.is_some_and(|p|p>=i){return false;}fixed[i as usize]=true;previous=Some(i);}
    let mut needs=Vec::new();for x in d {if x.start>x.end || x.end>n{return false;}needs.push((x.start,x.end,x.chunks));}
    previous=None;for b in s {
        if b.first>b.last || b.last>=n || previous.is_some_and(|p|p>=b.first) || b.chunks==0
            || b.chunks>b.last-b.first+1 || (b.chunks==1)!=(b.first==b.last){return false;}
        if fixed[b.first as usize] || fixed[b.last as usize]{return false;}
        fixed[b.first as usize]=true;fixed[b.last as usize]=true;
        needs.push((b.first,b.last+1,b.chunks-if b.first==b.last{1}else{2}));previous=Some(b.last);
    }
    let mut groups=Vec::new();if let Some(f)=f {
        if !m.contains(&f.config) || f.final_index.is_some_and(|i|i<=f.config || !m.contains(&i)){return false;}
        let mut prior:Option<G>=None;
        for &g in f.groups {
            if g.window==0 || g.rows==0 || !m.contains(&g.begin)
                || g.begin==f.config || g.closed.is_some_and(|i|i<=g.begin || !m.contains(&i) || i==f.config)
                || prior.is_some_and(|p|p.window>=g.window || p.closed.is_none_or(|i|i>=g.begin)){return false;}
            groups.push((f.config,g.begin,f.final_index.unwrap_or(n),g.rows));prior=Some(g);
        }
    }
    fn assign(at:usize,fixed:&[bool],needs:&mut[(u64,u64,u64)],groups:&mut[(u64,u64,u64,u64)],last:usize)->bool{
        if at==fixed.len(){return needs.iter().all(|x|x.2==0)&&groups.iter().all(|x|x.3==0);}
        if fixed[at]{return assign(at+1,fixed,needs,groups,last);}
        for i in 0..needs.len(){if needs[i].0<=at as u64 && (at as u64)<needs[i].1 && needs[i].2>0{
            needs[i].2-=1;let yes=assign(at+1,fixed,needs,groups,last);needs[i].2+=1;if yes{return true;}
        }}
        for i in last..groups.len(){if groups[i].0<(at as u64) && groups[i].1<(at as u64) && (at as u64)<groups[i].2 && groups[i].3>0{
            groups[i].3-=1;let yes=assign(at+1,fixed,needs,groups,i);groups[i].3+=1;if yes{return true;}
        }}false
    }
    assign(0,&fixed,&mut needs,&mut groups,0)
}

#[test]
fn exhaustive_tiny_generic_demands_match_independent_slot_assignments(){
    let mut cases=0;let mut positive=0;
    for n in 0..=5 {for mask in 0..1u64<<n {
        let m:Vec<_>=(0..n).filter(|i|mask&(1<<i)!=0).collect();
        for start in 0..=n {for end in start..=n {for a in 0..=2 {for b in 0..=2 {
            let ds=[d(start,end,a),d(0,n,b)];let expected=enumerate(n,&m,&ds,&[],None);
            let actual=check(n,&m,&ds,&[],None);assert_eq!(actual.is_ok(),expected,"n={n} m={m:?} d={ds:?}, got {actual:?}");
            cases+=1;positive+=usize::from(expected);
        }}}}
    }}assert!(cases>5000 && positive>100);
}

#[test]
fn exhaustive_fifo_group_order_and_closed_windows_match_independent_labels(){
    let n=7;let mut cases=0;let mut positive=0;
    for extra in 0..8u64 {
        let mut m=vec![0,1,2,3];for (bit,index) in [4,5,6].into_iter().enumerate(){if extra&(1<<bit)!=0{m.push(index);}}
        for first in 1..=2 {for second in 1..=2 {
            let groups=[G{window:2,begin:1,closed:Some(2),rows:first},G{window:9,begin:3,closed:None,rows:second}];
            let f=F{config:0,final_index:None,groups:&groups};
            for start in 0..=n {for end in start..=n {for count in 0..=2 {
                let ds=[d(start,end,count)];let expected=enumerate(n,&m,&ds,&[],Some(f));
                let got=check(n,&m,&ds,&[],Some(f));assert_eq!(got.is_ok(),expected,"m={m:?} groups={groups:?} ds={ds:?}, got {got:?}");
                cases+=1;positive+=usize::from(expected);
            }}}
        }}
    }assert!(cases>3000 && positive>10);
}
