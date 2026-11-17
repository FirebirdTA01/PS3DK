/* Initial RED obligations: checked bounded API, canonical initial checkpoint,
 * original-header refusal and immutable accepted state on rejected input. */
#include "roll_ppu.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL ppu-encoder:%d: %s\n", __LINE__, #x); exit(1); } } while (0)
static const uint8_t header[16] = {'P','S','3','D','K','P','R','F',1,0,0,0,0,0,0,0};
static uint32_t r32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1]<<8 | (uint32_t)p[2]<<16 | (uint32_t)p[3]<<24; }
static uint32_t crc(const uint8_t *p, size_t n)
{ uint32_t c=UINT32_MAX; unsigned k; while(n--) { c^=*p++; for(k=0;k<8;++k)c=(c>>1)^((c&1)?UINT32_C(0xedb88320):0); } return ~c; }
int main(void)
{
    pfp_config cfg = {16,16,32,64,32768,65536}; pfp_layout layout;
    void *ctx, *a, *b, *before; uint8_t *cp; uint8_t unsupported[16], input[20]={0}; size_t n;
    pfo_prepared result;
    CHECK(pfp_plan(&cfg,&layout)==PFR_OK && "general PPU arena planner unavailable");
    CHECK(layout.context_bytes && layout.arena_bytes && layout.arena_bytes<=1048576);
    CHECK(layout.arena_alignment && layout.arena_alignment<=_Alignof(max_align_t));
    CHECK(layout.checkpoint_bound==132u+32768u+120u*16u+80u*32u+16u*64u);
    ctx=malloc(layout.context_bytes); a=malloc(layout.arena_bytes); b=malloc(layout.arena_bytes);
    before=malloc(layout.arena_bytes); cp=malloc(cfg.checkpoint_bytes); CHECK(ctx&&a&&b&&before&&cp);
    CHECK(pfp_init(ctx,layout.context_bytes,&cfg,header,a,layout.arena_bytes,cp,cfg.checkpoint_bytes,&n)==PFR_OK);
    CHECK(n==132 && !memcmp(cp,"PFRCKP01",8) && r32(cp+128)==crc(cp,128));
    CHECK(!memcmp(cp+80,header,16)); /* bootstrap section's original header */
    memcpy(before,a,layout.arena_bytes);
    memset(&result,0xa5,sizeof result);
    CHECK(pfp_prepare(ctx,a,b,input,sizeof input,cp,cfg.checkpoint_bytes,&result)==PFR_INVALID);
    CHECK(!memcmp(before,a,layout.arena_bytes));
    CHECK(!result.checkpoint_bytes && !result.terminal && !result.reason);
    memcpy(unsupported,header,16); unsupported[12]=1; /* genuine EXT flag, no masking */
    CHECK(pfp_init(ctx,layout.context_bytes,&cfg,unsupported,b,layout.arena_bytes,cp,cfg.checkpoint_bytes,&n)==PFR_INVALID);
    cfg.checkpoint_bytes=131; CHECK(pfp_plan(&cfg,&layout)==PFR_INVALID);
    free(cp); free(before); free(b); free(a); free(ctx);
    puts("PASS PPU encoder initial API, header and rejected-input obligations"); return 0;
}
