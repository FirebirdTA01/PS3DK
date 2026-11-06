#include "ob_csv.h"

static const char *const tu_name[OB_TUS] = {"on", "off", "off_b"};
static const char *const kind_name[4] = {"?", "warm", "timed", "cold"};

int ob_csv_begin(ob_fw *w, uint64_t seed, const ob_block *b, unsigned long n, uint64_t tb_freq)
{
    return ob_fw_printf(w, "# OB seed=%llu blocks=%lu digest=%016llx tb_freq=%llu\n" OB_CSV_HEADER "\n",
                        (unsigned long long)seed, n, (unsigned long long)ob_sched_digest(b, n),
                        (unsigned long long)tb_freq);
}

int ob_csv_row(ob_fw *w, unsigned long index, const ob_block *b, const ob_row *r)
{
    const ob_ledger *l = &r->led;
    return ob_fw_printf(w,
                        "BLK,%lu,%s,%s,%s,%u,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%lld,%llu,%llu,%d,%d,%llu,%llu,%u,"
                        "%u,%u\n",
                        index, ob_case_table[b->c].name, tu_name[b->tu % OB_TUS], kind_name[b->kind & 3u],
                        (unsigned)b->rep, (unsigned long long)r->t0, (unsigned long long)r->t1,
                        (unsigned long long)l->seq, (unsigned long long)l->dropped, (unsigned long long)l->regs,
                        (unsigned long long)l->depth, (unsigned long long)l->sseq, (unsigned long long)l->tail_lost,
                        (long long)l->staged, (unsigned long long)l->published, (unsigned long long)l->lost,
                        r->counted, r->rc, (unsigned long long)r->ppu_t0, (unsigned long long)r->ppu_t1, r->dec_reset,
                        r->spu_cmd, r->spu_echo);
}

int ob_csv_end(ob_fw *w, unsigned long rows) { return ob_fw_printf(w, "END,%lu\n", rows); }
