/* Actual pfm2 + unchanged pfo/pfr fault harness. Only test capacity conversion;
 * no production encoder is copied and no runtime selector is enabled. */
#include "roll_combined2.h"
#include "roll_ppu.h"
typedef struct {
    uint32_t incarnations,descriptors,stack_rows,frame_identities,metadata_bytes,checkpoint_bytes;
} auto_output_config;
static pfm2_config auto_output_settings(const auto_output_config *o)
{
    pfm2_config c={{16,16,32,64,8,8,8,32,64,64,32,32,32,64,131072,262144,UINT64_C(1)<<28},
        8,8,32,8,192,64,64,64,64,4096,131072};
    c.shared.incarnations=o->incarnations;c.shared.descriptors=o->descriptors;
    c.shared.ppu_stack_rows=o->stack_rows;c.shared.frame_identities=o->frame_identities;
    c.shared.metadata_bytes=o->metadata_bytes;c.shared.checkpoint_bytes=o->checkpoint_bytes;
    return c;
}
static int auto_output_plan(const auto_output_config *o,pfm2_layout *l)
{pfm2_config c=auto_output_settings(o);return pfm2_plan(&c,l);}
static int auto_output_init(void *ctx,size_t context_bytes,const auto_output_config *o,const uint8_t header[16],
    void *arena,size_t arena_bytes,void *checkpoint,size_t capacity,size_t *written)
{pfm2_config c=auto_output_settings(o);return pfm2_init(ctx,context_bytes,&c,header,arena,arena_bytes,checkpoint,capacity,written);}
#undef pfp_plan
#undef pfp_init
#undef pfp_prepare
#define pfp_config auto_output_config
#define pfp_layout pfm2_layout
#define pfp_plan auto_output_plan
#define pfp_init auto_output_init
#define pfp_prepare pfm2_prepare
#include "ppu_output_fixture.c"
