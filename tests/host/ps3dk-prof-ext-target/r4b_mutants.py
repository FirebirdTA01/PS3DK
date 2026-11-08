#!/usr/bin/env python3
"""P3.3 R4b REAL checker mutants (target step): exact, single-match source edits of spu/ext_runtime.c, each with the
image-checker row it must trip and the named reason (the verdict alone does not count).  Every edit must match its
anchor exactly once, or the generator refuses (a mutant that silently fails to apply is never "killed").

usage: r4b_mutants.py EXT_RUNTIME_C OUTDIR     writes OUTDIR/<name>/ext_runtime.c and OUTDIR/MUTANTS.tsv
       (name, check, function, expected verdict, reason substring)"""
import sys
from pathlib import Path

ST64 = '    *(volatile uint64_t *)(void *)at(ea) = v;\n'
ST16 = '    *(volatile uint16_t *)(void *)at(ea) = v;\n'
LD64 = '    return *(const volatile uint64_t *)(const void *)at(ea);\n'
LDLINE = '        uint64_t v = src[i];\n'
LWSYNC = '    __asm__ volatile("lwsync" ::: "memory");\n'
MEMSET = '        memset(slot, 0, slot_bytes(p));\n'
ZSYNC = ('        __asm__ volatile("sync" ::: "memory"); /* the zeroed ring + control block are performed before the lane\'s EA\n'
         '                                                * can leave this thread (r4b D2) */\n')
STORE_EA = '        *ea = (uint64_t)(uintptr_t)slot;\n'
ADAPTER = '#else /* PS3DK_PROF_PAL_LV2 */\n'

MUTANTS = [
    ('c1_st64_bytes', 'G4b', 'm_st64', 'FAIL', '8 stores to the EA',
     [(ST64, '    volatile uint8_t *d = at(ea);\n' +  # eight explicit byte stores to the shared line (no loop)
             ''.join(f'    d[{k}] = (uint8_t)(v >> {56 - 8 * k});\n' for k in range(8)))]),
    ('c2_lwsync_noop', 'G4e', 'm_lwsync', 'FAIL', 'no lwsync', [(LWSYNC, '')]),
    ('c3_sync_before_memset', 'G4a', 'm_alloc', 'FAIL', 'no sync after the zeroing memset',
     [(ZSYNC, '        /* moved */\n'), (MEMSET, '        __asm__ volatile("sync" ::: "memory");\n' + MEMSET)]),
    ('c4_bypass', 'G4a', 'm_alloc', 'FAIL', 'no sync after the zeroing memset',
     [(MEMSET, '        if (m_bypass) {\n    ' + MEMSET + '    ' + STORE_EA + '            return 0;\n        }\n'
               + MEMSET),
      (ADAPTER, ADAPTER + 'static volatile int m_bypass;\n')]),
    ('c5_untaken_sync', 'G4a', 'm_alloc', 'FAIL', 'no sync after the zeroing memset',
     [(ZSYNC, '        if (m_never)\n' + ZSYNC), (ADAPTER, ADAPTER + 'static volatile int m_never;\n')]),
    ('c6_ldline_byte', 'G4d', 'm_ld_line', 'FAIL', 'not a doubleword ld',
     [(LDLINE, '        uint64_t v = i == 5 ? (uint64_t)((const volatile uint8_t *)src)[40] : src[i];\n')]),
    ('c7_icall_in_window', 'G4a', 'm_alloc', 'UNMET', 'indirect call',
     [(MEMSET, MEMSET + '        m_hook();\n'),
      (ADAPTER, ADAPTER + 'static void m_nop_hook(void) {}\nstatic void (*volatile m_hook)(void) = m_nop_hook;\n')]),
    ('c8_nonzero_fill', 'G4a', 'm_alloc', 'FAIL', 'memset fill is not zero',
     [(MEMSET, MEMSET.replace(', 0, slot_bytes(p))', ', 1, slot_bytes(p))'))]),
    ('c9_wrong_dest', 'G4a', 'm_alloc', 'FAIL', 'stored EA is not the zeroed region address',
     [(MEMSET, '        memset(p->spu_slots, 0, slot_bytes(p));\n')]),
    ('c10_ld64_lost', 'G4c', 'm_ld64', 'FAIL', 'returned value is not the loaded doubleword',
     [(LD64, '    uint64_t x = *(const volatile uint64_t *)(const void *)at(ea);\n    (void)x;\n    return 0;\n')]),
    ('c11_st16_word', 'G4b', 'm_st16', 'FAIL', 'not one 2-byte store',
     [(ST16, '    *(volatile uint32_t *)(void *)at(ea) = v;\n')]),
]


def main():
    if len(sys.argv) != 3:
        print(__doc__)
        return 2
    src = Path(sys.argv[1]).read_text(encoding='ascii')
    out = Path(sys.argv[2])
    out.mkdir()
    rows = []
    for name, check, fn, verdict, why, edits in MUTANTS:
        s = src
        for old, new in edits:
            n = s.count(old)
            if n != 1:
                print(f'REFUSED {name}: anchor occurs {n} times: {old.strip()[:60]!r}')
                return 1
            s = s.replace(old, new)
        (out / name).mkdir()
        (out / name / 'ext_runtime.c').write_text(s, encoding='ascii', newline='\n')
        rows.append('\t'.join((name, check, fn, verdict, why)))
    (out / 'MUTANTS.tsv').write_text('\n'.join(rows) + '\n', encoding='ascii', newline='\n')
    print(f'{len(rows)} mutants written')
    return 0


if __name__ == '__main__':
    sys.exit(main())
