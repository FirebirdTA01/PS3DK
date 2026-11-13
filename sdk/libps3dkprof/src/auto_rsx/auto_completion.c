#include "auto_completion.h"
#include <stddef.h>
#include <string.h>
#define ACQ(p) __atomic_load_n((p),__ATOMIC_ACQUIRE)
#define REL(p,v) __atomic_store_n((p),(v),__ATOMIC_RELEASE)
_Static_assert(__atomic_always_lock_free(4,0),"AUTO publication needs inline 4-byte atomics");

int auto_publication_commit(auto_publication *p,uint32_t serial)
{
    if(!serial || (serial&1u) || serial<=ACQ(&p->committed)) return 0;
    REL(&p->committed,serial);
    return 1;
}
uint32_t auto_publication_before(const auto_publication *p) {return ACQ(&p->committed);}
int auto_publication_after(auto_publication *p,uint32_t snapshot)
{
    if((snapshot&1u) || snapshot>ACQ(&p->committed) || snapshot<ACQ(&p->published)) return 0;
    REL(&p->published,snapshot);
    return 1;
}
uint32_t auto_publication_read(const auto_publication *p) {return ACQ(&p->published);}

int auto_completion_read(auto_completion *s,const auto_publication *pub,const auto_completion_port *port,
                         uint32_t begin_slot,uint32_t end_slot,uint32_t begin_serial,uint32_t end_serial,
                         auto_completion_fact *fact)
{
    uint32_t snapshot,first,last;
    uint64_t b0,e0,b1,e1;
    if(!fact) return AUTO_COMPLETION_REFUSED;
    memset(fact,0,sizeof *fact);
    if(!s || s->invalid || !pub || !port || !port->label || !port->timer || !port->sync ||
       begin_slot>=2048 || end_slot>=2048 || begin_slot==end_slot || !begin_serial ||
       !(begin_serial&1u) || begin_serial==UINT32_MAX || end_serial!=begin_serial+1u)
        return AUTO_COMPLETION_REFUSED;
    snapshot=auto_publication_read(pub);
    first=port->label(port->user);
    port->sync(port->user);
    if(first<s->last_label) {s->invalid=1;return AUTO_COMPLETION_REFUSED;}
    if(first>snapshot) return AUTO_COMPLETION_DEFER;
    s->last_label=first;
    if(first<end_serial) return AUTO_COMPLETION_PENDING;
    b0=port->timer(port->user,begin_slot);e0=port->timer(port->user,end_slot);
    port->sync(port->user);
    b1=port->timer(port->user,begin_slot);e1=port->timer(port->user,end_slot);
    port->sync(port->user);
    last=port->label(port->user);
    port->sync(port->user);
    if(last<first) {s->invalid=1;return AUTO_COMPLETION_REFUSED;}
    if(last>snapshot) return AUTO_COMPLETION_DEFER;
    s->last_label=last;
    if(b0!=b1 || e0!=e1 || b0==UINT64_MAX || e0==UINT64_MAX || e0<b0)
        return AUTO_COMPLETION_REFUSED;
    fact->raw_begin=b0;fact->raw_end=e0;fact->published=snapshot;
    return AUTO_COMPLETION_READY;
}
