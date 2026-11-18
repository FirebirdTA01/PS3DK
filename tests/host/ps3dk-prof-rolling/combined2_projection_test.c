/* Exercise the private projection state directly, including wire flag values.
 * The integration tests separately read real runtime output with Rust. */
#include "roll_combined2.c"
#include <stdio.h>
#include <stdlib.h>
#include "combined2_encoder_config.h"
#define CHECK(x) do { if (!(x)) { fprintf(stderr,"projection assertion: %s\n",#x); return 1; } } while (0)
static int own_frontier(void)
{
    pfm2_config cfg=test_config();pfm2_layout layout;pfo_prepared prepared;
    uint8_t header[16]={0},chunk[56]={0};void *ctx,*a,*b,*saved,*cp,*swap;size_t written;unsigned i;
    cfg.family_flags=128;cfg.auto_build_entries=4;cfg.auto_clock_anchors=9;
    cfg.fifo_membership_runs=0;cfg.accounting_groups=0;
    cfg.auto_final_runs=4;cfg.auto_touched_runs=4;
    cfg.auto_bootstrap_bytes=512;cfg.auto_support_bytes=16384;
    cfg.shared.checkpoint_bytes=262144;cfg.shared.prepare_work=UINT64_C(100000000);
    memcpy(header,"PS3DKPRF",8);w16(header+8,1);w32(header+12,641);
    CHECK(pfm2_plan(&cfg,&layout)==PFR_OK);
    ctx=malloc(layout.context_bytes);a=malloc(layout.arena_bytes);b=malloc(layout.arena_bytes);
    saved=malloc(layout.arena_bytes);cp=malloc(cfg.shared.checkpoint_bytes);
    CHECK(ctx&&a&&b&&saved&&cp);
    CHECK(pfm2_init(ctx,layout.context_bytes,&cfg,header,a,layout.arena_bytes,cp,cfg.shared.checkpoint_bytes,&written)==PFR_OK);
    CHECK(pfm2_retention(ctx,a,6,cp,cfg.shared.checkpoint_bytes,&written)==PFR_OK);
    memcpy(saved,a,layout.arena_bytes);
    /* Six empty strings, ILP32/thread mode, zero start fields, two limits. */
    w32(chunk,1);w32(chunk+4,36);chunk[28]=1;chunk[29]=2;
    w32(chunk+44,2);w32(chunk+48,8);w32(chunk+52,crc(chunk,52));
    CHECK(pfm2_prepare(ctx,a,b,chunk,sizeof chunk,cp,cfg.shared.checkpoint_bytes,&prepared)==PFR_OK);
    CHECK(!memcmp(a,saved,layout.arena_bytes));
    CHECK(r64((uint8_t *)cp+32)==1);
    CHECK(r64((uint8_t *)cp+80)==1); /* No physical commit or projection call. */
    memcpy(saved,b,layout.arena_bytes);
    CHECK(pfm2_project(ctx,b,a,0,cp,cfg.shared.checkpoint_bytes,&written)==PFR_OK);
    CHECK(!memcmp(b,saved,layout.arena_bytes)&&!memcmp(a,b,layout.arena_bytes));
    CHECK(r64((uint8_t *)cp+80)==1); /* Oldest physical slot cannot move support backwards. */
    /* More closed windows than the bounded pool, without a single physical
     * commit. Open/current declarations must survive; older ones must retire. */
    for(i=0;i<4001;++i){size_t bytes=i?52:48;unsigned id=(i+1)/2;
        memset(chunk,0,sizeof chunk);w64(chunk+8,(uint64_t)i+1);
        if(!i){w32(chunk,2);w32(chunk+4,28);w32(chunk+16,1);w32(chunk+20,1);w64(chunk+28,1000000);}
        else if(i&1){w32(chunk,41);w32(chunk+4,32);w16(chunk+16,1);w32(chunk+20,id);
            w32(chunk+24,UINT32_MAX);w64(chunk+28,id);chunk[36]=1;chunk[37]=1;}
        else {w32(chunk,42);w32(chunk+4,32);w16(chunk+16,1);w32(chunk+20,id);chunk[24]=1;w64(chunk+28,id);}
        w32(chunk+bytes-4,crc(chunk,bytes-4));memcpy(saved,a,layout.arena_bytes);
        CHECK(pfm2_prepare(ctx,a,b,chunk,bytes,cp,cfg.shared.checkpoint_bytes,&prepared)==PFR_OK);
        CHECK(!memcmp(a,saved,layout.arena_bytes));
        CHECK(r64((uint8_t *)cp+32)==(uint64_t)i+2&&r64((uint8_t *)cp+80)==(uint64_t)i+2);
        CHECK(state(ctx,b)->windows<=2);
        swap=a;a=b;b=swap;
    }
    CHECK(state(ctx,a)->retired_windows==1999&&state(ctx,a)->retired_window_id==1999);
    state(ctx,a)->automatic.new_builds=cfg.auto_build_entries;
    state(ctx,a)->automatic.new_clocks=cfg.auto_clock_anchors;
    state(ctx,a)->new_windows=cfg.windows;
    CHECK(pfm2_pressure(ctx,a)==0); /* History alone cannot force full-slot IO. */
    free(cp);free(saved);free(b);free(a);free(ctx);return 0;
}
int main(void)
{
    static const uint16_t flags[] = {2,4,8}; /* TIMEOUT, REGRESSION, READ_REFUSED */
    unsigned bracket,j;
    for (bracket=0;bracket<2;++bracket) for(j=0;j<3;++j) {
        au_state s; au_clock clocks[2]; au_pools pools; pfm2_config cfg; uint8_t raw[96];
        memset(&s,0,sizeof s); memset(&pools,0,sizeof pools); memset(&cfg,0,sizeof cfg); memset(raw,0,sizeof raw);
        pools.clocks=clocks; cfg.auto_clock_anchors=2;
        w32(s.config+4,1); w32(s.config+12,2); w32(s.config+16,3); w32(s.config+32,100);
        /* Only CLOCK_BRACKET changes these rules. */
        w32(s.config+24,bracket?127:63);
        w16(raw,1);w16(raw+2,flags[j]);w32(raw+4,1);w32(raw+8,2);w32(raw+12,1);
        w32(raw+20,100);w32(raw+24,1);w32(raw+80,3);
        CHECK(au_clock_accept(&s,pools,&cfg,raw,sizeof raw,10)==PFR_OK);
        CHECK(s.sticky_clock==(!bracket||flags[j]==4));
    }
    CHECK(own_frontier()==0);
    puts("projection clock flags and own frontier: PASS");return 0;
}
