/* overhead-bench CSV (one row per executed block; tools/ob_verify.py checks it).  Portable C over ob_fw (ob_io.h):
 * every write is checked and its failure returned.
 *   line 1: "# OB seed=<seed> blocks=<planned n> digest=<16 hex> tb_freq=<Hz>"
 *   line 2: the column header OB_CSV_HEADER
 *   one BLK row per EXECUTED block, in schedule order, then "END,<rows actually written>"
 * PPU rows: t0/t1 = the PPU boundary readings (timebase); ppu_t0/ppu_t1/dec_reset/spu_cmd/spu_echo = 0; counted =
 *   0 on the target (the counted ledger comes from the capture), 1 in the host ledger model.
 * SPU rows (Plan A r2 2.3): t0/t1 = the SPU decrementer readings (a down-counter, raw endpoints kept as data);
 *   ppu_t0/ppu_t1 = the identity-linked enclosing PPU interval (timebase right before the command was sent, right
 *   after its reply was observed); dec_reset = 1 when a decrementer write that returned success preceded the block
 *   (reported by the SPU in the reply); spu_cmd = the command word sent, spu_echo = the one the reply carries;
 *   counted = 1 (the writer state the SPU reports). */
#ifndef OB_CSV_H
#define OB_CSV_H

#include "ob_cases.h"
#include "ob_io.h"
#include "ob_sched.h"

#define OB_CSV_HEADER                                                                                                  \
    "kind,index,case,tu,block,rep,t0,t1,seq,dropped,regs,depth,sseq,tail_lost,staged,published,lost,counted,rc,"       \
    "ppu_t0,ppu_t1,dec_reset,spu_cmd,spu_echo"

typedef struct ob_row {
    uint64_t t0, t1;
    ob_ledger led;
    int counted, rc;
    uint64_t ppu_t0, ppu_t1;
    uint32_t dec_reset, spu_cmd, spu_echo;
} ob_row;

int ob_csv_begin(ob_fw *w, uint64_t seed, const ob_block *b, unsigned long n, uint64_t tb_freq);
int ob_csv_row(ob_fw *w, unsigned long index, const ob_block *b, const ob_row *r);
int ob_csv_end(ob_fw *w, unsigned long rows);

#endif
