/* I3a checker controls (codex 09:24): small SPU entry points the ordering checker must judge exactly.  Linked only.
 * Every entry takes the control struct first (the checker's P(arg,0)), as the API entries take the writer.
 *   ctl_good              PUT tag 5, mask 1<<5, update ALL, RdTagStat, PUTLLUC + status: PASS (the model clears)
 *   ctl_tail              a wrapper TAIL-BRANCHES to a PUT helper, then PUTLLUC: VIOLATION C
 *   ctl_indirect          the PUT helper is reached through a function pointer: UNMET
 *   ctl_update_any        PUT tag 5, mask 1<<5, update ANY, RdTagStat, PUTLLUC: VIOLATION C
 *   ctl_wrong_mask        PUT tag 5, mask 1<<6, update ALL, RdTagStat, PUTLLUC: VIOLATION C
 *   ctl_early_read        GETLLAR into the line, a load from it, then the status: VIOLATION D
 *   ctl_store_during_put  PUT from the staging pointer, a store through it, then the wait: D-UNRESOLVED (exit 4)
 *   ctl_helper_store      PUT, then a channel-free helper that stores through the staging pointer: D-UNRESOLVED
 *   ctl_unknown_alias     GETLLAR, a load through an unrelated pointer before the status: D-UNRESOLVED */
#include <spu_mfcio.h>
#include <stdint.h>

typedef struct ctl_s {
    uint8_t line[128] __attribute__((aligned(128)));
    uint8_t *stage;
} ctl_s;

void ctl_good(ctl_s *p, uint64_t ea);
void ctl_tail(ctl_s *p, uint64_t ea);
void ctl_indirect(ctl_s *p, uint64_t ea);
void ctl_update_any(ctl_s *p, uint64_t ea);
void ctl_wrong_mask(ctl_s *p, uint64_t ea);
void ctl_early_read(ctl_s *p, uint64_t ea, volatile uint32_t *sink);
void ctl_store_during_put(ctl_s *p, uint64_t ea);
void ctl_helper_store(ctl_s *p, uint64_t ea);
void ctl_unknown_alias(ctl_s *p, uint64_t ea, const volatile uint32_t *q, volatile uint32_t *sink);

static void publish(ctl_s *p, uint64_t ea)
{
    mfc_putlluc(p->line, ea + 128, 0, 0);
    (void)mfc_read_atomic_status();
}

__attribute__((noinline)) void ctl_put_helper(ctl_s *p, uint64_t ea) { mfc_put(p->stage, ea, 128, 5, 0, 0); }
__attribute__((noinline)) void ctl_wrapper(ctl_s *p, uint64_t ea) { ctl_put_helper(p, ea); } /* -O2: a tail branch */
void (*volatile ctl_fp)(ctl_s *, uint64_t) = ctl_put_helper;
__attribute__((noinline)) void ctl_wrapper_indirect(ctl_s *p, uint64_t ea) { ctl_fp(p, ea); }

void ctl_good(ctl_s *p, uint64_t ea)
{
    mfc_put(p->stage, ea, 128, 5, 0, 0);
    mfc_write_tag_mask(1u << 5);
    (void)mfc_read_tag_status_all();
    publish(p, ea);
}

void ctl_tail(ctl_s *p, uint64_t ea)
{
    ctl_wrapper(p, ea);
    publish(p, ea);
}

void ctl_indirect(ctl_s *p, uint64_t ea)
{
    ctl_wrapper_indirect(p, ea);
    publish(p, ea);
}

void ctl_update_any(ctl_s *p, uint64_t ea)
{
    mfc_put(p->stage, ea, 128, 5, 0, 0);
    mfc_write_tag_mask(1u << 5);
    (void)mfc_read_tag_status_any();
    publish(p, ea);
}

void ctl_wrong_mask(ctl_s *p, uint64_t ea)
{
    mfc_put(p->stage, ea, 128, 5, 0, 0);
    mfc_write_tag_mask(1u << 6);
    (void)mfc_read_tag_status_all();
    publish(p, ea);
}

void ctl_early_read(ctl_s *p, uint64_t ea, volatile uint32_t *sink)
{
    mfc_getllar(p->line, ea, 0, 0);
    *sink = p->line[4]; /* read before the GETLLAR status */
    (void)mfc_read_atomic_status();
}

void ctl_store_during_put(ctl_s *p, uint64_t ea)
{
    mfc_put(p->stage, ea, 128, 5, 0, 0);
    p->stage[0] = 1; /* writes the PUT source before the wait */
    mfc_write_tag_mask(1u << 5);
    (void)mfc_read_tag_status_all();
}

__attribute__((noinline)) void ctl_touch_stage(ctl_s *p) { p->stage[1] = 2; } /* no channel op */

void ctl_helper_store(ctl_s *p, uint64_t ea)
{
    mfc_put(p->stage, ea, 128, 5, 0, 0);
    ctl_touch_stage(p); /* writes the PUT source before the wait, inside a channel-free helper */
    mfc_write_tag_mask(1u << 5);
    (void)mfc_read_tag_status_all();
}

void ctl_unknown_alias(ctl_s *p, uint64_t ea, const volatile uint32_t *q, volatile uint32_t *sink)
{
    mfc_getllar(p->line, ea, 0, 0);
    *sink = *q; /* q may alias the line: not decidable */
    (void)mfc_read_atomic_status();
}

static ctl_s g_ctl;
static volatile uint32_t g_sink;

int main(unsigned long long spe, unsigned long long argp, unsigned long long envp)
{
    (void)spe;
    (void)envp;
    ctl_good(&g_ctl, argp);
    ctl_tail(&g_ctl, argp);
    ctl_indirect(&g_ctl, argp);
    ctl_update_any(&g_ctl, argp);
    ctl_wrong_mask(&g_ctl, argp);
    ctl_early_read(&g_ctl, argp, &g_sink);
    ctl_store_during_put(&g_ctl, argp);
    ctl_helper_store(&g_ctl, argp);
    ctl_unknown_alias(&g_ctl, argp, &g_sink, &g_sink);
    return 0;
}
