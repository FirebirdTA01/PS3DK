#include "auto_wire.h"
#include <string.h>

static void le16(uint8_t *b, uint16_t v) { b[0]=(uint8_t)v; b[1]=(uint8_t)(v>>8); }
static void le32(uint8_t *b, uint32_t v) {
    unsigned i;
    for(i=0;i<4;i++) b[i]=(uint8_t)(v>>(8*i));
}
static void le64(uint8_t *b, uint64_t v) {
    unsigned i;
    for(i=0;i<8;i++) b[i]=(uint8_t)(v>>(8*i));
}

size_t auto_encode_config(uint8_t *b, size_t cap, const auto_wire_config *c)
{
    if (!b || !c || cap < AUTO_CONFIG_BYTES || c->reason > AUTO_NO_GCM_INIT) return 0;
    memset(b,0,AUTO_CONFIG_BYTES);
    le16(b,1); le16(b+2,c->reason ? AUTO_REFUSED:0); le32(b+4,c->instance);
    le64(b+64,c->frequency);
    memcpy(b+96,c->mechanism,32); memcpy(b+128,c->binding,32);
    if(c->reason) {
        le32(b+24,c->reason); le32(b+28,(uint32_t)c->application_rc);
    } else {
        le32(b+8,c->domain); le32(b+12,c->epoch); le32(b+16,c->owner); le32(b+20,c->generation);
        le32(b+24,7); le32(b+28,15);
        le32(b+32,c->report_first); le32(b+36,c->report_count);
        le32(b+40,c->op_label); le32(b+44,c->clock_label);
        le32(b+48,c->pairs); le32(b+52,c->metadata); le32(b+56,c->frames); le32(b+60,AUTO_OP_BYTES);
        le64(b+72,c->allocated); le64(b+80,c->ceiling); le32(b+88,3); le32(b+92,1);
    }
    return AUTO_CONFIG_BYTES;
}

size_t auto_encode_ops(uint8_t *b, size_t cap, uint32_t instance, const auto_wire_op *ops, uint32_t count)
{
    uint32_t i,j;
    size_t bytes;
    if(!b || !ops || !count || count > (AUTO_CHUNK_MAX-16u)/AUTO_OP_BYTES) return 0;
    bytes=16u+(size_t)count*AUTO_OP_BYTES; /* <=1 MiB, representable in both target ABIs */
    if(cap<bytes) return 0;
    memset(b,0,bytes);
    le16(b,1); le32(b+4,instance); le32(b+8,count); le32(b+12,AUTO_OP_BYTES);
    for(i=0;i<count;i++) {
        const auto_wire_op *o=&ops[i]; uint8_t *q=b+16u+(size_t)i*AUTO_OP_BYTES;
        le64(q,o->id); le32(q+8,o->frame_owner); le16(q+12,o->kind); le16(q+14,o->flags);
        le64(q+16,o->frame); le64(q+24,o->tb_before); le64(q+32,o->tb_after);
        le64(q+40,o->raw_begin); le64(q+48,o->raw_end); le64(q+56,o->caller);
        for(j=0;j<7;j++) le32(q+64+j*4,o->args[j]);
        le32(q+96,o->report_begin); le32(q+100,o->report_end);
        le32(q+104,o->serial_begin); le32(q+108,o->serial_end); le32(q+112,o->generation);
        le64(q+120,o->published);
    }
    return bytes;
}

size_t auto_encode_frame(uint8_t *b, size_t cap, const auto_wire_frame *f)
{
    unsigned i,j;
    if(!b || !f || cap<AUTO_FRAME_BYTES) return 0;
    memset(b,0,AUTO_FRAME_BYTES);
    le16(b,1); le16(b+2,f->flags); le32(b+4,f->instance); le32(b+8,f->owner);
    le64(b+16,f->frame); le64(b+24,f->tb_begin); le64(b+32,f->tb_end);
    le64(b+40,f->unsupported); le64(b+48,f->lost);
    for(i=0;i<3;i++) for(j=0;j<12;j++) le64(b+64+i*96+j*8,f->counts[i][j]);
    return AUTO_FRAME_BYTES;
}

size_t auto_encode_status(uint8_t *b, size_t cap, uint32_t instance, uint16_t flags, const uint64_t counters[15])
{
    unsigned i;
    if(!b || !counters || cap<AUTO_STATUS_BYTES) return 0;
    memset(b,0,AUTO_STATUS_BYTES);
    le16(b,1); le16(b+2,flags); le32(b+4,instance);
    for(i=0;i<15;i++) le64(b+8+i*8,counters[i]);
    return AUTO_STATUS_BYTES;
}

size_t auto_encode_clock(uint8_t *b, size_t cap, const auto_wire_clock *a)
{
    if(!b || !a || cap<AUTO_CLOCK_BYTES) return 0;
    memset(b,0,AUTO_CLOCK_BYTES);
    le16(b,1); le16(b+2,a->flags); le32(b+4,a->instance); le32(b+8,a->epoch);
    le32(b+12,a->id); le32(b+16,a->role); le32(b+20,a->slot); le32(b+24,a->serial); le32(b+28,a->polls);
    le64(b+32,a->tb_before); le64(b+40,a->tb_after); le64(b+48,a->raw); le64(b+56,a->repeated);
    le32(b+64,a->label_first); le32(b+68,a->label_second); le64(b+72,a->frame); le32(b+80,a->owner);
    le64(b+88,a->elapsed);
    return AUTO_CLOCK_BYTES;
}
