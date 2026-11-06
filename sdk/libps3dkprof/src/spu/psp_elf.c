/* SPU ELF descriptor parsing (design P3.2 I3b r2): see psp_elf.h.  All arithmetic in u64; every table, scan and
 * note bounded; a failure anywhere is -1 with nothing registered. */
#include "psp_elf.h"

#include <string.h>

enum { PT_LOAD = 1, SHT_PROGBITS = 1, SHT_STRTAB = 3, SHT_RELA = 4, SHT_NOTE = 7, SHT_REL = 9, SHF_ALLOC = 2 };

static uint32_t be16(const uint8_t *p) { return (uint32_t)p[0] << 8 | p[1]; }
static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }

static const char k_desc[] = "ps3dkprof_spudesc";
static const char k_note[] = ".note.gnu.build-id";

int psp_elf_text_ok(const uint8_t *s, const uint8_t *end, uint32_t lo, uint32_t hi, uint32_t *len)
{
    uint32_t n = 0;
    for (;; n++) {
        if (s + n >= end || n > hi)
            return 0; /* no NUL inside the segment, or longer than hi */
        if (!s[n])
            break;
        if (s[n] < 0x20 || s[n] > 0x7E)
            return 0;
    }
    if (n < lo)
        return 0;
    *len = n;
    return 1;
}

/* the PT_LOAD whose FILE-BACKED part holds [va, va + len): its file offset and the end of its file bytes */
static int map(const psp_elf_view *v, uint64_t va, uint64_t len, uint64_t *off, uint64_t *seg_end)
{
    uint32_t i;
    for (i = 0; i < v->phnum; i++) {
        const uint8_t *ph = v->b + v->phoff + 32u * i;
        uint64_t p_off = be32(ph + 4), p_va = be32(ph + 8), p_fsz = be32(ph + 16);
        if (be32(ph) != PT_LOAD)
            continue;
        if (va >= p_va && va - p_va <= p_fsz && len <= p_fsz - (va - p_va)) {
            *off = p_off + (va - p_va);
            *seg_end = p_off + p_fsz;
            return 1;
        }
    }
    return 0;
}

/* the name at shstrtab offset `name` equals `want` (bounded by the target length and the section) */
static int name_is(const uint8_t *strtab, uint64_t strsz, uint32_t name, const char *want)
{
    size_t n = strlen(want) + 1;
    return name < strsz && n <= strsz - name && !memcmp(strtab + name, want, n);
}

static uint32_t pad4(uint32_t x) { return (x + 3u) & ~3u; }

int psp_elf_parse(const void *elf, uint64_t size, psp_elf_view *v)
{
    const uint8_t *b = elf;
    uint32_t phoff, shoff, phnum, shnum, shstrndx, i, j, desc_idx = UINT32_MAX, note_idx = UINT32_MAX;
    uint64_t str_off, str_sz, last_nul;
    const uint8_t *sh, *strtab;
    memset(v, 0, sizeof *v);
    if (!b || size < 52 || size > PSP_ELF_SIZE_MAX || size > (uint64_t)SIZE_MAX)
        return -1;
    if (memcmp(b, "\177ELF", 4) || b[4] != 1 || b[5] != 2 || b[6] != 1 || be16(b + 16) != 2 || be16(b + 18) != 23 ||
        be32(b + 20) != 1 || be16(b + 40) != 52)
        return -1; /* ELF32, MSB, EI_VERSION 1, EXEC, EM_SPU, e_version 1, ehsize 52 */
    phoff = be32(b + 28);
    shoff = be32(b + 32);
    phnum = be16(b + 44);
    shnum = be16(b + 48);
    shstrndx = be16(b + 50);
    if (phnum == 0xffffu || phnum < 1 || phnum > PSP_ELF_PHNUM_MAX || be16(b + 42) != 32 ||
        (uint64_t)phoff + 32u * (uint64_t)phnum > size)
        return -1;
    if (shnum == 0 || shnum > PSP_ELF_SHNUM_MAX || be16(b + 46) != 40 || (uint64_t)shoff + 40u * (uint64_t)shnum > size ||
        shstrndx >= 0xff00u || shstrndx >= shnum)
        return -1; /* extended numbering (shnum 0, SHN_XINDEX, >= SHN_LORESERVE) is rejected */
    v->b = b;
    v->size = size;
    v->phoff = phoff;
    v->phnum = phnum;
    /* segments: in range, congruent, inside the LS, pairwise disjoint */
    for (i = 0; i < phnum; i++) {
        const uint8_t *ph = b + phoff + 32u * i;
        uint64_t off = be32(ph + 4), va = be32(ph + 8), fsz = be32(ph + 16), msz = be32(ph + 20), al = be32(ph + 28);
        if (be32(ph) != PT_LOAD)
            continue;
        if (fsz > msz || off + fsz > size || va + msz > PSP_ELF_LS_END || (al > 1 && ((al & (al - 1)) ||
                                                                                       va % al != off % al)))
            return -1;
        for (j = 0; j < i; j++) {
            const uint8_t *q = b + phoff + 32u * j;
            uint64_t va2 = be32(q + 8), msz2 = be32(q + 20);
            if (be32(q) == PT_LOAD && va < va2 + msz2 && va2 < va + msz)
                return -1;
        }
    }
    /* the section-name table: STRTAB, in range; one bounded pass finds its last NUL, which bounds every name */
    sh = b + shoff;
    if (be32(sh + 40u * shstrndx + 4) != SHT_STRTAB)
        return -1;
    str_off = be32(sh + 40u * shstrndx + 16);
    str_sz = be32(sh + 40u * shstrndx + 20);
    if (str_sz < 1 || str_off + str_sz > size)
        return -1;
    strtab = b + str_off;
    for (last_nul = str_sz; last_nul > 0 && strtab[last_nul - 1]; last_nul--)
        ;
    if (!last_nul)
        return -1;
    last_nul--; /* the index of the last NUL: every name starting at or before it is terminated in the section */
    for (i = 0; i < shnum; i++) {
        const uint8_t *s = sh + 40u * i;
        uint32_t nm = be32(s);
        if (nm > last_nul)
            return -1;
        if (name_is(strtab, str_sz, nm, k_desc)) {
            if (desc_idx != UINT32_MAX)
                return -1; /* two descriptor sections */
            desc_idx = i;
        } else if (name_is(strtab, str_sz, nm, k_note)) {
            if (note_idx != UINT32_MAX)
                return -1; /* two build-id sections */
            note_idx = i;
        }
    }
    if (desc_idx == UINT32_MAX)
        return -1;
    {
        const uint8_t *d = sh + 40u * desc_idx;
        uint64_t addr = be32(d + 12), off = be32(d + 16), dsz = be32(d + 20), m_off, m_end;
        if (be32(d + 4) != SHT_PROGBITS || !(be32(d + 8) & SHF_ALLOC) || dsz % 16 || dsz / 16 < 1 ||
            dsz / 16 > 4096 || addr % 16 || addr + dsz > PSP_ELF_LS_END || off + dsz > size ||
            !map(v, addr, dsz, &m_off, &m_end) || m_off != off)
            return -1; /* inside ONE PT_LOAD's file bytes, and the section header agrees with the segment */
        for (i = 0; i < shnum; i++) {
            const uint8_t *s = sh + 40u * i;
            uint32_t t = be32(s + 4);
            if ((t == SHT_REL || t == SHT_RELA) && be32(s + 28) == desc_idx)
                return -1; /* relocations on the descriptor section are not assumed resolved */
        }
        v->desc_addr = (uint32_t)addr;
        v->desc_off = (uint32_t)off;
        v->n = (uint32_t)(dsz / 16);
    }
    /* the build id: exactly one well-formed GNU note, or the FNV fallback */
    v->build_len = 0;
    if (note_idx != UINT32_MAX) {
        const uint8_t *s = sh + 40u * note_idx, *nb;
        uint64_t off = be32(s + 16), nsz = be32(s + 20), namesz, descsz;
        if (be32(s + 4) != SHT_NOTE || nsz < 12 || nsz > PSP_ELF_NOTE_MAX || off + nsz > size)
            return -1;
        nb = b + off;
        namesz = be32(nb);
        descsz = be32(nb + 4);
        /* the exact size first (bounded operands), so the name and padding reads below stay inside the section */
        if (namesz != 4 || descsz > PSP_ELF_NOTE_MAX || nsz != 12 + pad4((uint32_t)namesz) + pad4((uint32_t)descsz))
            return -1;
        if (be32(nb + 8) != 3 || memcmp(nb + 12, "GNU", 4))
            return -1;
        for (i = (uint32_t)descsz; i < pad4((uint32_t)descsz); i++)
            if (nb[16 + i])
                return -1; /* non-zero padding */
        if (descsz >= 1 && descsz <= 63) {
            v->build_id[0] = 0x01;
            memcpy(v->build_id + 1, nb + 16, (size_t)descsz);
            v->build_len = 1 + (uint32_t)descsz;
        }
    }
    if (!v->build_len) {
        uint64_t h = 0xcbf29ce484222325ull, k;
        for (k = 0; k < size; k++)
            h = (h ^ b[k]) * 0x100000001b3ull;
        v->build_id[0] = 0x02;
        for (i = 0; i < 8; i++)
            v->build_id[1 + i] = (uint8_t)(h >> (56 - 8 * i));
        v->build_len = 9;
    }
    /* every record, through the same getter the copy pass uses */
    for (i = 0; i < v->n; i++) {
        psp_desc_rec r;
        if (psp_elf_get(v, i, &r) != 0)
            return -1;
    }
    return 0;
}

int psp_elf_get(void *ctx, uint32_t i, psp_desc_rec *out)
{
    const psp_elf_view *v = ctx;
    const uint8_t *rec;
    uint64_t off, end;
    uint32_t ln, lf, pn, pf;
    if (i >= v->n)
        return -1;
    rec = v->b + v->desc_off + 16u * i;
    if (be32(rec) != v->desc_addr + 16u * i)
        return -1; /* the record's own LS address */
    pn = be32(rec + 8);
    pf = be32(rec + 12);
    if (!map(v, pn, 1, &off, &end) || !psp_elf_text_ok(v->b + off, v->b + end, 1, 512, &ln))
        return -1;
    out->name = (const char *)(v->b + off);
    if (!map(v, pf, 1, &off, &end) || !psp_elf_text_ok(v->b + off, v->b + end, 0, 512, &lf))
        return -1;
    out->file = (const char *)(v->b + off);
    out->ls_addr = be32(rec);
    out->line = be32(rec + 4);
    out->name_len = ln;
    out->file_len = lf;
    return 0;
}
