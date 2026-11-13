#include "auto_config.h"
#include <string.h>

#define ACQ(p) __atomic_load_n((p),__ATOMIC_ACQUIRE)
#define REL(p,v) __atomic_store_n((p),(v),__ATOMIC_RELEASE)
#define CAS(p,e,v) __atomic_compare_exchange_n((p),(e),(v),0,__ATOMIC_ACQ_REL,__ATOMIC_ACQUIRE)
enum { EMPTY=0, WRITING=1, READY=2, FROZEN=3, PHASE_MASK=3, STOP=4 };
_Static_assert(__atomic_always_lock_free(4,0), "AUTO config needs inline 4-byte atomics");

/* Refused CONFIG cannot invent an owner, domain, resource range or allocation.
 * Preserve only its capture identity and the original application result. */
static void refusal(auto_wire_config *c, uint32_t reason, int32_t application_rc)
{
    auto_wire_config prior=*c;
    memset(c,0,sizeof *c);
    c->instance=1; c->frequency=prior.frequency;
    memcpy(c->mechanism,prior.mechanism,sizeof c->mechanism);
    memcpy(c->binding,prior.binding,sizeof c->binding);
    c->reason=reason; c->application_rc=application_rc;
}

void auto_config_init(auto_config_store *s, uint64_t frequency, const uint8_t mechanism[32],
                      const uint8_t binding[32])
{
    memset(s,0,sizeof *s);
    s->config.instance=1; s->config.frequency=frequency;
    memcpy(s->config.mechanism,mechanism,32); memcpy(s->config.binding,binding,32);
    s->config.reason=AUTO_NO_GCM_INIT;
}

uint32_t auto_config_begin(auto_config_store *s)
{
    uint32_t state=ACQ(&s->state);
    for(;;) {
        uint32_t phase=state&PHASE_MASK, ticket=phase+1u;
        if((state&STOP) || (phase!=EMPTY && phase!=READY)) return 0;
        if(CAS(&s->state,&state,WRITING)) return ticket;
    }
}

void auto_config_abort(auto_config_store *s, uint32_t ticket)
{
    uint32_t expected=WRITING;
    if(ticket!=EMPTY+1u && ticket!=READY+1u) return;
    if(CAS(&s->state,&expected,ticket-1u)) return;
    if(expected==(WRITING|STOP)) REL(&s->state,FROZEN|STOP);
}

int auto_config_commit(auto_config_store *s, uint32_t ticket, const auto_wire_config *candidate)
{
    uint32_t expected=WRITING;
    auto_wire_config next;
    if(ticket!=EMPTY+1u && ticket!=READY+1u) return 0;
    if((ACQ(&s->state)&PHASE_MASK)!=WRITING) return 0;
    if(!candidate || candidate->instance!=1 || candidate->reason>AUTO_NO_GCM_INIT) {
        auto_config_abort(s,ticket);
        return 0;
    }
    next=*candidate;
    /* The runtime may override resources, never the capture's identity. */
    next.frequency=s->config.frequency;
    memcpy(next.mechanism,s->config.mechanism,32); memcpy(next.binding,s->config.binding,32);
    if(next.reason) refusal(&next,next.reason,next.application_rc);
    s->config=next;
    if(CAS(&s->state,&expected,READY)) return 1;
    /* The seal won while the candidate was being prepared. It cannot read
     * these bytes until this release, and no admitted device work exists. */
    refusal(&s->config,AUTO_STOPPED,next.application_rc);
    REL(&s->state,FROZEN|STOP);
    return 0;
}

int auto_config_freeze(auto_config_store *s)
{
    uint32_t state=ACQ(&s->state);
    for(;;) {
        if((state&PHASE_MASK)==WRITING) return 0;
        if((state&PHASE_MASK)==FROZEN) return 1;
        if(CAS(&s->state,&state,FROZEN|(state&STOP))) return 1;
    }
}

void auto_config_stop(auto_config_store *s)
{
    uint32_t state=ACQ(&s->state);
    for(;;) {
        uint32_t next=((state&PHASE_MASK)==WRITING ? WRITING:FROZEN)|STOP;
        if(CAS(&s->state,&state,next)) return;
    }
}

void auto_config_invalidate(auto_config_store *s, uint32_t flags)
{
    flags&=AUTO_STATUS_CONFLICT|AUTO_STATUS_RESET;
    __atomic_fetch_or(&s->flags,flags,__ATOMIC_RELEASE);
    auto_config_stop(s);
}
uint32_t auto_config_flags(const auto_config_store *s) { return ACQ(&s->flags); }
const auto_wire_config *auto_config_read(const auto_config_store *s)
{
    return (ACQ(&s->state)&PHASE_MASK)==FROZEN ? &s->config:NULL;
}
int auto_config_can_admit(const auto_config_store *s)
{
    return ACQ(&s->state)==FROZEN && !s->config.reason;
}
