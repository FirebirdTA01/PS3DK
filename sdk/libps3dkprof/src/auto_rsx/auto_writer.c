#include "auto_writer.h"
#include <string.h>

int auto_writer_init(auto_writer *w, const auto_config_store *c, uint8_t *scratch, size_t capacity,
                     auto_chunk_sink sink, void *user)
{
    if(!w || !c || !scratch || capacity<AUTO_FRAME_BYTES || !sink) return 0;
    memset(w,0,sizeof *w);
    w->config=c; w->scratch=scratch; w->capacity=capacity; w->sink=sink; w->user=user;
    return 1;
}

static int send(auto_writer *w, uint32_t type, size_t n)
{
    if(!n || w->failed || w->closed) return 0;
    if(w->sink(w->user,type,w->scratch,n)!=0) {
        w->failed=1;
        return 0;
    }
    return 1;
}

int auto_writer_begin(auto_writer *w)
{
    const auto_wire_config *c;
    uint64_t zero[15]={0};
    size_t n;
    if(w->closed || w->failed) return 0;
    c=auto_config_read(w->config);
    if(!c) return 0; /* background drain does not close the override window */
    if(!w->config_sent) {
        n=auto_encode_config(w->scratch,w->capacity,c);
        if(!send(w,C_AUTO_CONFIG,n)) return 0;
        w->config_sent=1;
    }
    if(c->reason) {
        n=auto_encode_status(w->scratch,w->capacity,c->instance,AUTO_FINAL,zero);
        if(send(w,C_AUTO_STATUS,n)) w->closed=1;
        return 0;
    }
    return 1;
}

int auto_writer_ops(auto_writer *w, const auto_wire_op *ops, uint32_t count)
{
    const auto_wire_config *c;
    size_t n;
    uint64_t publication,ring;
    uint32_t i;
    if(!auto_writer_begin(w) || UINT64_MAX-w->operations_written<count) return 0;
    c=auto_config_read(w->config);
    n=c->ring_bytes ? auto_encode_ops_ring(w->scratch,w->capacity,c->instance,ops,count):
                      auto_encode_ops(w->scratch,w->capacity,c->instance,ops,count);
    if(!n) return 0;
    publication=w->publication_max;ring=w->ring_published_max;
    for(i=0;i<count;i++) {
        if(ops[i].published>UINT32_MAX) return 0;
        if(ops[i].flags&AUTO_OP_RING) {
            /* a ring record only exists in the ring CONFIG and carries the ring label, never the operation label */
            if(!c->ring_bytes) return 0;
            if(ops[i].published>ring) ring=ops[i].published;
        } else if(ops[i].published>publication) publication=ops[i].published;
    }
    if(!send(w,C_AUTO_OPS,n)) return 0;
    w->operations_written+=count;
    w->publication_max=publication;w->ring_published_max=ring;
    return 1; /* caller may acknowledge these exact queue entries now */
}

int auto_writer_frame(auto_writer *w, const auto_wire_frame *frame)
{
    size_t n;
    if(!auto_writer_begin(w) || !frame || w->frames_written==UINT64_MAX ||
       frame->instance!=auto_config_read(w->config)->instance) return 0;
    n=auto_config_read(w->config)->ring_bytes ? auto_encode_frame_ring(w->scratch,w->capacity,frame):
                                                auto_encode_frame(w->scratch,w->capacity,frame);
    if(!send(w,C_AUTO_COVERAGE,n)) return 0;
    w->frames_written++;
    return 1;
}

int auto_writer_clock(auto_writer *w, const auto_wire_clock *clock)
{
    size_t n;
    if(!auto_writer_begin(w) || !clock || w->clocks_written==UINT64_MAX ||
       clock->instance!=auto_config_read(w->config)->instance) return 0;
    n=auto_encode_clock(w->scratch,w->capacity,clock);
    if(!send(w,C_AUTO_CLOCK,n)) return 0;
    w->clocks_written++;
    return 1;
}

static int writer_status(auto_writer *w, uint16_t flags, const uint64_t values[15], const uint64_t *ring,
                         const uint32_t *reject);
int auto_writer_status(auto_writer *w, uint16_t flags, const uint64_t values[15])
{
    return writer_status(w,flags,values,NULL,NULL);
}
int auto_writer_status_ring(auto_writer *w, uint16_t flags, const uint64_t values[15], const uint64_t ring[8],
                            const uint32_t reject[3])
{
    return ring && reject ? writer_status(w,flags,values,ring,reject):0;
}
static int writer_status(auto_writer *w, uint16_t flags, const uint64_t values[15], const uint64_t *ring,
                         const uint32_t *reject)
{
    const auto_wire_config *c;
    uint64_t disposed;
    unsigned i;
    size_t n;
    if(!auto_writer_begin(w) || !values || (flags&~4095u)) return 0;
    c=auto_config_read(w->config);
    flags|=(uint16_t)auto_config_flags(w->config);
    flags|=w->sticky;
    if((flags&AUTO_SATURATED) && !(flags&AUTO_LOWER_BOUND)) return 0;
    if((flags&AUTO_OWNER_UNACK) && (flags&(AUTO_FINAL|AUTO_LOWER_BOUND))!=(AUTO_FINAL|AUTO_LOWER_BOUND)) return 0;
    if(values[2]!=w->operations_written || values[5]!=w->frames_written ||
       values[10]!=w->clocks_written || values[14]>c->pairs ||
       values[13]<w->publication_max || values[13]>UINT32_MAX) return 0;
    if(w->status_seen) for(i=0;i<15;i++) {
        if(i!=4 && i!=14 && values[i]<w->prior[i]) return 0;
    }
    if(!(flags&AUTO_SATURATED)) {
        if(UINT64_MAX-values[2]<values[3]) return 0;
        disposed=values[2]+values[3];
        if(UINT64_MAX-disposed<values[4]) return 0;
        disposed+=values[4];
        if(values[1]<disposed || values[0]<values[1]) return 0;
        if(!(flags&AUTO_LOWER_BOUND) && values[1]!=disposed) return 0;
    }
    if((flags&AUTO_FINAL) && !(flags&AUTO_LOWER_BOUND) && (values[4] || values[14])) return 0;
    if(!c->ring_bytes!=!ring) return 0;
    if(ring) {
        uint64_t extent=(uint64_t)c->ring_pairs*32u;
        if(ring[1]>ring[0] || ring[3]!=ring[0]-ring[1] || (ring[2]&1u) || ring[2]>2u*ring[0] ||
           ring[2]<w->ring_published_max || ring[0]>UINT32_MAX || ring[4]>ring[0]*extent ||
           ring[5]>ring[0] || ring[6]>ring[0]-ring[5]) return 0;
        if((flags&AUTO_FINAL) && !(flags&AUTO_LOWER_BOUND) && ring[3]) return 0;
        if(reject[2]>AUTO_STATUS_RING_REJECT_MAX || (!reject[2] && (reject[0] || reject[1]))) return 0;
    }
    n=ring ? auto_encode_status_ring(w->scratch,w->capacity,c->instance,flags,values,ring,reject):
             auto_encode_status(w->scratch,w->capacity,c->instance,flags,values);
    if(!send(w,C_AUTO_STATUS,n)) return 0;
    memcpy(w->prior,values,sizeof w->prior); w->status_seen=1;
    w->sticky=flags&(AUTO_SATURATED|AUTO_STATUS_CONFLICT|AUTO_STATUS_RESET|AUTO_STATUS_SEGMENT_QUARANTINE|
                                AUTO_STATUS_FRAME_MARK_MISSED|AUTO_STATUS_FRAMES_FLIP|AUTO_STATUS_FRAMES_EXPLICIT|
                                AUTO_STATUS_SEGMENT_REPLAY|AUTO_STATUS_PREINIT_FOREIGN);
    if(flags&AUTO_FINAL) w->closed=1;
    return 1;
}
