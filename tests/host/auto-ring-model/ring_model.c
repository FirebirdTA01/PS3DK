/* Host-only scalar command model. No SDK code or production capture format.
 * Device events are supplied by a scheduler; they are NOT inferred from labels.
 * All storage is allocated at attach. No allocation occurs on a work event. */
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef MUTANT
#define MUTANT 0
#endif
enum { FREE, OPEN, SEALED, EXECUTING, REPORTS, COPIED, READY, READING, QUARANTINE };
static const char *const states[] = {
    "FREE", "OPEN", "SEALED", "EXECUTING", "REPORTS", "COPIED", "READY", "READING", "QUARANTINE"
};
enum { STOPPED, REPLAY, WINDOW_CLOSED, RING_FULL, METADATA_FULL, COMMAND_FULL, SERIAL_FULL, NREASONS };
static const char *const reasons[] = {
    "STOPPED", "REPLAY", "WINDOW_CLOSED", "RING_FULL", "METADATA_FULL", "COMMAND_FULL", "SERIAL_FULL"
};
typedef struct { uint64_t begin, end, op, generation; } Report;
_Static_assert(sizeof(Report) == 32, "private report-pair storage");
typedef struct {
    uint64_t seq, frame, window;
    size_t count;
    int state, local, credit;
} Slot;
typedef struct {
    size_t blocks, pairs, bytes, slots, meta_cap, credit_cap;
    uint64_t limit, issued, executed, next, visits, reserved, last_id;
    uint64_t frame, window, last_window, active, exclusions[NREASONS];
    size_t metadata, commands;
    int stopped, invalid, attached;
    Slot *slot;
    Report *ring, *local_data;
    uint64_t *ids, *local_owner;
    unsigned char *wire;
} Model;
static Model m;
static unsigned alloc_count, alloc_fail;

static void destroy(void) {
    free(m.slot); free(m.ring); free(m.local_data); free(m.ids);
    free(m.local_owner); free(m.wire);
    memset(&m, 0, sizeof m);
}
static void *allocate(size_t n, size_t bytes) {
    ++alloc_count;
    return alloc_count == alloc_fail ? NULL : calloc(n, bytes);
}
static const char *attach(const uint64_t *a, unsigned fail) {
    destroy();
    /* These are host-model bounds, not proposed SDK limits. */
    if (a[0] < 2 || a[0] > 64 || !a[1] || a[1] > 256 ||
        a[2] < a[1]*32 || a[2] > 64*1024*1024 ||
        a[2]/(a[1]*32) > 65536 || a[3] > 2097152 || a[4] > 65536 ||
        !a[5] || a[5] > UINT32_MAX-1u) return "CONFIG";
    m.blocks = (size_t)a[0]; m.pairs = (size_t)a[1]; m.bytes = (size_t)a[2];
    m.slots = m.bytes/(m.pairs*32); m.meta_cap = (size_t)a[3];
    m.credit_cap = (size_t)a[4]; m.limit = a[5];
    alloc_count = 0; alloc_fail = fail;
    m.slot = allocate(m.slots, sizeof *m.slot);
    m.ring = allocate(m.bytes, 1);
    m.local_data = allocate(m.blocks*m.pairs, sizeof *m.local_data);
    m.ids = allocate(m.slots*m.pairs, sizeof *m.ids);
    m.local_owner = allocate(m.blocks, sizeof *m.local_owner);
    m.wire = allocate(48+m.pairs*56, 1);
    if (!m.slot || !m.ring || !m.local_data || !m.ids || !m.local_owner || !m.wire) {
        destroy(); return "ALLOC";
    }
    m.next = m.window = m.last_window = 1;
    m.attached = 1;
    return "OK";
}
static size_t index_of(uint64_t seq) { return (size_t)((seq-1)%m.slots); }
static Slot *find(uint64_t seq) {
    if (!seq) return NULL;
    Slot *s = &m.slot[index_of(seq)];
    return s->state && s->seq == seq ? s : NULL;
}
static void seal(void) {
    Slot *s = find(m.active);
    if (s) s->state = SEALED;
    m.active = 0;
}
static const char *exclude(unsigned why) {
    ++m.exclusions[why];
    return MUTANT == 6 && why == RING_FULL ? "OK" : reasons[why];
}
static const char *draw(uint64_t id, int uncertain) {
    if (!id || id <= m.last_id || id > UINT64_MAX/16) return "INPUT";
    m.last_id = id; ++m.visits;
    if (m.stopped) return exclude(STOPPED);
    if (m.invalid) return exclude(REPLAY);
    if (!m.window) return exclude(WINDOW_CLOSED);
    Slot *s = find(m.active);
    if (!s && m.slot[m.issued%m.slots].state) return exclude(RING_FULL);
    if (m.metadata == m.meta_cap) return exclude(METADATA_FULL);
    if (!s && m.commands == m.credit_cap) return exclude(COMMAND_FULL);
    if (!s && m.issued == m.limit) return exclude(SERIAL_FULL);
    /* All reservations precede the first potentially emitted word. */
    if (!s) {
        ++m.issued;
        s = &m.slot[index_of(m.issued)];
        memset(s, 0, sizeof *s);
        s->seq = m.issued; s->frame = m.frame; s->window = m.window;
        s->state = OPEN; s->credit = 1; ++m.commands;
        m.active = s->seq;
    }
    m.ids[index_of(s->seq)*m.pairs+s->count] = id;
    ++s->count; ++m.metadata; ++m.reserved;
    if (uncertain) {
        s->state = QUARANTINE; m.active = 0;
        return "QUARANTINE";
    }
    if (s->count == m.pairs) seal();
    return "OK";
}
static void put(unsigned char **p, uint64_t value, unsigned width) {
    for (unsigned i = 0; i < width; ++i) { *(*p)++ = (unsigned char)value; value >>= 8; }
}
static size_t encode(Slot *s) {
    unsigned char *p = m.wire;
    memcpy(p, "RGM1", 4); p += 4;
    put(&p, 1, 2); put(&p, 1, 2); put(&p, s->seq, 8);
    put(&p, s->count, 4); put(&p, 56, 4); put(&p, s->count*56, 4); put(&p, 0, 4);
    size_t base = index_of(s->seq)*m.pairs;
    for (size_t i = 0; i < s->count; ++i) {
        Report *r = &m.ring[base+i];
        put(&p, m.ids[base+i], 8); put(&p, 7, 8);
        put(&p, s->frame, 8); put(&p, s->window, 8);
        put(&p, r->generation, 8); put(&p, r->begin, 8); put(&p, r->end, 8);
    }
    put(&p, s->seq, 8); put(&p, ~s->seq, 8);
    return (size_t)(p-m.wire);
}
static void retire(Slot *s) {
    m.metadata -= s->count;
    memset(s, 0, sizeof *s);
    ++m.next;
}
static const char *step(const char *op, const uint64_t *a, size_t *wire_size) {
    if (!m.attached) return "NO_ATTACH";
    if (!strcmp(op, "draw") || !strcmp(op, "uncertain")) return draw(a[0], !strcmp(op, "uncertain"));
    if (!strcmp(op, "flush")) { seal(); return "OK"; }
    if (!strcmp(op, "frame")) {
        if (m.stopped || a[0] <= m.frame) return "BOUNDARY";
        seal(); m.frame = a[0]; return "OK";
    }
    if (!strcmp(op, "window")) {
        if (m.stopped || a[0] == m.window || (a[0] && a[0] <= m.last_window)) return "BOUNDARY";
        seal(); m.window = a[0];
        if (a[0]) m.last_window = a[0];
        return "OK";
    }
    if (!strcmp(op, "stop")) {
        if (MUTANT != 8) seal();
        m.stopped = 1; return "OK";
    }
    if (!strcmp(op, "finish"))
        return m.stopped && m.next == m.issued+1 && !m.invalid ? "OK" : "INCOMPLETE";
    Slot *s = find(a[0]);
    if (!strcmp(op, "replay")) {
        if (MUTANT == 9) return "OK";
        m.invalid = 1;
        if (s) { s->state = QUARANTINE; if (m.active == s->seq) m.active = 0; }
        return "REPLAY";
    }
    if (!s) return "GENERATION";
    size_t local = (size_t)((s->seq-1)%m.blocks), base = index_of(s->seq)*m.pairs;
    if (!strcmp(op, "fault")) {
        s->state = QUARANTINE; if (m.active == s->seq) m.active = 0;
        return "QUARANTINE";
    }
    if (!strcmp(op, "execute")) {
        if (s->state != SEALED || s->seq != m.executed+1) return "ORDER";
        if (m.local_owner[local] && MUTANT != 3) return "LOCAL_BUSY";
        m.local_owner[local] = s->seq; s->local = 1;
        s->state = EXECUTING; ++m.executed; return "OK";
    }
    if (!strcmp(op, "reports")) {
        if (s->state != EXECUTING) return "ORDER";
        for (size_t i = 0; i < s->count; ++i) {
            uint64_t id = m.ids[base+i];
            m.local_data[local*m.pairs+i] = (Report){id*16+1, id*16+6, id, s->seq};
        }
        s->state = REPORTS; return "OK";
    }
    if (!strcmp(op, "copy")) {
        if (s->state != REPORTS && !(MUTANT == 1 && s->state == EXECUTING)) return "ORDER";
        memcpy(&m.ring[base], &m.local_data[local*m.pairs], s->count*sizeof(Report));
        m.local_owner[local] = 0; s->local = 0;
        s->state = COPIED; return "OK";
    }
    if (!strcmp(op, "publish")) {
        if (a[1] != s->seq && !(MUTANT == 5 && a[1] < s->seq)) return "LABEL";
        if (s->state != COPIED && !(MUTANT == 2 && s->state == EXECUTING)) return "ORDER";
        s->state = READY; s->credit = 0; --m.commands; return "OK";
    }
    if (!strcmp(op, "read")) {
        if (s->seq != m.next || (s->state != READY && s->state != READING)) return "ORDER";
        s->state = READING; *wire_size = encode(s);
        if (MUTANT == 4) retire(s);
        return "OK";
    }
    if (!strcmp(op, "ack")) {
        if (s->state != READING || s->seq != m.next) return "ORDER";
        if (a[1] != 48+s->count*56 && MUTANT != 7) return "SHORT";
        retire(s); return "OK";
    }
    return "INPUT";
}
static void snapshot(const char *rc, size_t wire_size) {
    printf("{\"rc\":\"%s\"", rc);
    if (!m.attached) { puts("}"); return; }
    printf(",\"issued\":%"PRIu64",\"executed\":%"PRIu64",\"next\":%"PRIu64
           ",\"visits\":%"PRIu64",\"reserved\":%"PRIu64",\"frame\":%"PRIu64
           ",\"window\":%"PRIu64",\"stopped\":%d,\"invalid\":%d"
           ",\"metadata\":%zu,\"commands\":%zu,\"ring_bytes\":%zu,\"ring_slots\":%zu",
           m.issued, m.executed, m.next, m.visits, m.reserved, m.frame, m.window,
           m.stopped, m.invalid, m.metadata, m.commands, m.bytes, m.slots);
    printf(",\"exclusions\":{");
    int comma = 0;
    for (unsigned i = 0; i < NREASONS; ++i) if (m.exclusions[i]) {
        printf("%s\"%s\":%"PRIu64, comma ? "," : "", reasons[i], m.exclusions[i]); comma = 1;
    }
    printf("},\"locals\":[");
    for (size_t i = 0; i < m.blocks; ++i) printf("%s%"PRIu64, i ? "," : "", m.local_owner[i]);
    printf("],\"slots\":["); comma = 0;
    for (uint64_t n = m.next; n <= m.issued; ++n) {
        Slot *s = find(n);
        if (!s) continue;
        printf("%s{\"seq\":%"PRIu64",\"state\":\"%s\",\"frame\":%"PRIu64
               ",\"window\":%"PRIu64",\"credit\":%d,\"local\":%d,\"ids\":[",
               comma ? "," : "", s->seq, states[s->state], s->frame, s->window, s->credit, s->local);
        for (size_t i = 0; i < s->count; ++i)
            printf("%s%"PRIu64, i ? "," : "", m.ids[index_of(s->seq)*m.pairs+i]);
        printf("]}"); comma = 1;
    }
    printf("]");
    if (wire_size) {
        printf(",\"wire\":\"");
        for (size_t i = 0; i < wire_size; ++i) printf("%02x", m.wire[i]);
        printf("\"");
    }
    puts("}"); fflush(stdout);
}
int main(void) {
    char line[512], op[32];
    while (fgets(line, sizeof line, stdin)) {
        uint64_t a[7] = {0};
        int n = sscanf(line, "%31s %"SCNu64" %"SCNu64" %"SCNu64" %"SCNu64
                       " %"SCNu64" %"SCNu64" %"SCNu64, op, &a[0], &a[1], &a[2], &a[3], &a[4], &a[5], &a[6]);
        if (n < 1) { destroy(); return 2; }
        size_t wire_size = 0;
        const char *rc;
        if (!strcmp(op, "reset") || !strcmp(op, "resetfail")) {
            if (n < 7) { destroy(); return 2; }
            rc = attach(a, !strcmp(op, "resetfail") ? (unsigned)a[6] : 0);
        } else rc = step(op, a, &wire_size);
        snapshot(rc, wire_size);
        fflush(stdout);
    }
    destroy(); return ferror(stdin) ? 2 : 0;
}
