/* Reuse the unchanged actual pfo/pfr fault and acknowledgement harness. The
 * test adapters select pfm; no production encoder implementation is copied. */
#include "roll_combined.h"
#include "roll_ppu.h"
#undef pfp_plan
#undef pfp_init
#undef pfp_prepare
static int combined_test_plan(pfm_config *c,pfm_layout *l)
{
    c->spu_domains=8;c->spu_lanes=8;c->spu_images=8;c->epochs=32;c->brackets=128;c->image_descriptors=64;
    c->journal_entries=128;c->event_runs=128;c->live_spu_stack_rows=32;c->saved_spu_witnesses=256;c->prepare_work=4194304;
    return pfm_plan(c,l);
}
#define pfp_config pfm_config
#define pfp_layout pfm_layout
#define pfp_plan combined_test_plan
#define pfp_init pfm_init
#define pfp_prepare pfm_prepare
#define stack_rows ppu_stack_rows
#include "ppu_output_fixture.c"
