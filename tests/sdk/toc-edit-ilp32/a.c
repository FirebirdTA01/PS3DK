/* Minimal repro of the v0.26.0 ILP32 TOC-optimiser mis-remap (independent test code). */
typedef struct { unsigned int v[3]; unsigned int d[9]; } Desc;   /* 48 bytes */
typedef struct { Desc frag; unsigned int pad[6]; } Cascade;
struct Flat { unsigned int hdr[3]; Desc fragment; unsigned int tail[3]; } g_flat;
struct Pass { unsigned int w[23]; } g_passes;
Cascade g_cascades[4];
extern void use(const Desc *d, int unit);
extern int stored[4];
/* Unreferenced: --gc-sections removes it, so its 4-byte TOC entry has no kept user. */
int first(void) { return stored[0]; }
/* Uses the 8-byte g_flat+12 entry through its low word (lwz entry+4). */
void draw(const int *st)
{
    for (int i = 1; i < 4; ++i)
        use(st[i] ? &g_cascades[i].frag : &g_flat.fragment, i);
}
/* Uses the same entry with addis/ld, which the TOC optimiser can rewrite to addis/addi. */
void sky(void) { use(&g_flat.fragment, 4); use(&g_flat.fragment, 5); }
const unsigned int *passes(void) { return &g_passes.w[7]; }
