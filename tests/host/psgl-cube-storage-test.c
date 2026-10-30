/* Production helpers are extracted by the driver. Only allocation, address
 * translation and GPU completion are replaced by deterministic backends. */
#include <GLES/glext.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cube-types.h"
typedef struct {
    void *gcm, *bound_vertex_program, *bound_fragment_program;
    unsigned dirty, ffp_installed_mask, gl_sampler_mask, gl_sampler_cube_mask;
    GLboolean gl_sampler_routing;
    uint8_t gl_sampler_units[8];
} PSGLcontext;
#define PSGL_TEXTURE_ALIGNMENT 128u
static unsigned checks, failures, error_code, waits, frees, unsafe_frees;
static int fail_cpu, fail_gpu, fail_offset, fail_wait;
static void *queued;
static uint32_t queued_value;
static void check(int ok,const char *name)
{
    ++checks;if(!ok){++failures;printf("FAIL %s\n",name);}
}
static void psgl_gl_error(GLenum e) { error_code=e; }
static void psgl_zero(void *p,size_t n) { memset(p,0,n); }
static void *cpu_alloc(size_t count,size_t size) { return fail_cpu?NULL:calloc(count,size); }
static void *rsxMemalign(size_t alignment,size_t size)
{
    (void)alignment;return fail_gpu?NULL:malloc(size);
}
static void rsxFree(void *p)
{
    ++frees;
    if(queued==p){++unsafe_frees;return;}
    free(p);
}
static int cellGcmAddressToOffset(void *p,uint32_t *offset)
{
    (void)p;*offset=128;return fail_offset;
}
static int psgl_context_wait_idle_bounded(void *context,const char *what)
{
    (void)context;(void)what;++waits;
    if(fail_wait)return 0;
    if(queued){check(*(uint32_t *)queued==queued_value,"queued contents survive until completion");queued=NULL;}
    return 1;
}
#define calloc cpu_alloc
#include "cube-production.h"
#undef calloc
/* State emitters are the command boundary. The production validator must
   reject mixed units before calling any of them, even with dirty==0. */
typedef void PSGLcgProgram;
typedef void PSGLffpTexProgram;
static unsigned commands, attributes, g_psgl_ffp_tex_refusal_reported, psgl_ffp_program_installs;
static PSGLtextureObject *draw_texture;
#define EMITTER(name) static void name(PSGLcontext *c) { (void)c; ++commands; }
EMITTER(psgl_bind_render_target)
EMITTER(psgl_emit_viewport)
EMITTER(psgl_emit_scissor)
EMITTER(psgl_emit_blend)
EMITTER(psgl_emit_depth)
EMITTER(psgl_emit_stencil)
EMITTER(psgl_emit_alpha)
EMITTER(psgl_emit_raster)
EMITTER(psgl_emit_vertex_arrays)
static void psgl_emit_current_attribs(PSGLcontext *c) { (void)c; ++attributes; }
static PSGLcgProgram *psgl_cg_program(void *p) { return p; }
static uint32_t psgl_ffp_state_mask(PSGLcontext *c) { (void)c; return 0; }
static PSGLffpTexProgram *psgl_ffp_tex_program(unsigned n) { (void)n; return NULL; }
static int psgl_ffp_update_texenv(PSGLcontext *c,PSGLffpTexProgram *p) { (void)c;(void)p;return 0; }
static int psgl_ffp_state_mask_to_programs(unsigned n,PSGLcgProgram **v,PSGLcgProgram **f) { (void)n;(void)v;(void)f;return 0; }
static void psgl_ffp_update_matrices(PSGLcontext *c,PSGLcgProgram *p) { (void)c;(void)p; }
static void psgl_ffp_update_lighting(PSGLcontext *c,PSGLcgProgram *p) { (void)c;(void)p; }
static int psgl_emit_vertex_program(PSGLcontext *c,PSGLcgProgram *p) { (void)c;(void)p;++commands;return 1; }
static int psgl_emit_fragment_program(PSGLcontext *c,PSGLcgProgram *p) { (void)c;(void)p;++commands;return 1; }
static int psgl_emit_textures(PSGLcontext *c) { return psgl_cube_prepare(c,draw_texture); }
#include "cube-boundary.h"
int main(void)
{
    error_code=0;glGenerateMipmapOES(GL_TEXTURE_CUBE_MAP);
    check(error_code==GL_INVALID_OPERATION,"cube mip generation explicitly refused");
    PSGLcontext context={0};PSGLtextureObject texture={0};
    texture.target=GL_TEXTURE_CUBE_MAP;texture.min_filter=GL_NEAREST;texture.mag_filter=GL_NEAREST;
    texture.wrap_s=texture.wrap_t=texture.wrap_r=GL_CLAMP_TO_EDGE;
    uint32_t pitch=0,stride=0,total=0;
    check(psgl_cube_layout(3,3,&pitch,&stride,&total)&&pitch==128&&stride==384&&total==2304,"NPOT six-face layout");
    check(!psgl_cube_layout(0,0,&pitch,&stride,&total),"zero layout refused");
    check(!psgl_cube_layout(0xffffffffu,0xffffffffu,&pitch,&stride,&total),"overflow layout refused");
    unsigned char image[16]={1,2,3,255,4,5,6,255,7,8,9,255,10,11,12,255};
    for(unsigned f=0;f<6;++f){
        check(psgl_cube_set_face(&context,&texture,f,2,2,GL_RGBA,GL_RGBA,GL_UNSIGNED_BYTE,image,4),"set face");
        check(psgl_cube_complete(&texture)==(f==5),"all six faces required");
    }
    void *old_face=texture.faces[0].pixels;fail_cpu=1;
    check(!psgl_cube_set_face(&context,&texture,0,2,2,GL_RGBA,GL_RGBA,GL_UNSIGNED_BYTE,image,4)&&texture.faces[0].pixels==old_face&&error_code==GL_OUT_OF_MEMORY,"CPU OOM preserves face");fail_cpu=0;
    check(psgl_cube_prepare(&context,&texture),"initial pack");
    check(texture.gcm_texture.cubemap==1&&texture.gcm_texture.pitch==128&&texture.size==1536,"cube descriptor");
    for(unsigned f=0;f<6;++f){
        uint32_t *p=(uint32_t *)((unsigned char *)texture.address+f*256);
        check(p[0]==0xff010203u&&p[1]==0xff040506u&&p[32]==0xff070809u&&p[33]==0xff0a0b0cu,"all face row strides and colors");
        check(p[2]==0&&p[31]==0&&p[63]==0,"padding initialized within slab");
    }
    void *old=texture.address;queued=old;queued_value=*(uint32_t *)old;
    image[0]=99;check(psgl_cube_set_face(&context,&texture,0,2,2,GL_RGBA,GL_RGBA,GL_UNSIGNED_BYTE,image,4),"pending replacement");
    fail_gpu=1;unsigned before=frees;
    check(!psgl_cube_prepare(&context,&texture)&&texture.address==old&&texture.cube_dirty&&frees==before,"GPU OOM preserves old and pending storage");fail_gpu=0;
    fail_offset=1;
    check(!psgl_cube_prepare(&context,&texture)&&texture.address==old&&texture.cube_dirty&&frees==before+1,"offset failure frees candidate only");fail_offset=0;
    fail_wait=1;before=frees;
    check(!psgl_cube_prepare(&context,&texture)&&texture.address==old&&texture.cube_dirty&&frees==before+1&&error_code==GL_OUT_OF_MEMORY,"timeout preserves ownership and dirty state");
    check(!psgl_cube_release(&context,&texture)&&texture.address==old&&texture.faces[0].pixels,"release timeout preserves CPU and GPU ownership");
    fail_wait=0;before=frees;
    check(psgl_cube_prepare(&context,&texture)&&texture.address!=old&&!texture.cube_dirty&&frees==before+1,"replacement commits after completion");
    check(*(uint32_t *)texture.address==0xff630203u,"replacement uses pending CPU data");
    unsigned char growth[36];
    for(unsigned i=0;i<9;++i)memcpy(growth+4*i,"\x0e\x1e\x2e\xff",4);
    old=texture.address;queued=old;queued_value=*(uint32_t *)old;
    for(unsigned f=0;f<6;++f)psgl_cube_set_face(&context,&texture,f,3,3,GL_RGBA,GL_RGBA,GL_UNSIGNED_BYTE,growth,4);
    CellGcmTexture old_descriptor=texture.gcm_texture;uint32_t old_size=texture.size;
    fail_wait=1;
    check(!psgl_cube_prepare(&context,&texture)&&texture.address==old&&texture.size==old_size&&
          !memcmp(&texture.gcm_texture,&old_descriptor,sizeof(old_descriptor)),"growth timeout preserves complete GPU descriptor");fail_wait=0;
    check(psgl_cube_prepare(&context,&texture)&&texture.address!=old&&texture.size==2304,
          "queued growth waits before releasing smaller storage");
    texture.faces[5].type=GL_UNSIGNED_SHORT_4_4_4_4;
    check(!psgl_cube_complete(&texture),"different source type incomplete");texture.faces[5].type=GL_UNSIGNED_BYTE;
    texture.wrap_s=GL_REPEAT;check(!psgl_cube_complete(&texture),"NPOT repeat incomplete");texture.wrap_s=GL_CLAMP_TO_EDGE;
    check(psgl_cube_complete(&texture),"NPOT clamp complete");
    queued=texture.address;queued_value=*(uint32_t *)queued;
    check(psgl_cube_release(&context,&texture)&&!texture.address&&!texture.faces[0].pixels,"release after completion");
    check(!unsafe_frees&&waits>=4,"no queued allocation freed early");
    for(unsigned f=0;f<6;++f)psgl_cube_set_face(&context,&texture,f,2,2,GL_RGBA,GL_RGBA,GL_UNSIGNED_BYTE,image,4);
    draw_texture=&texture;context.bound_vertex_program=context.bound_fragment_program=&context;
    context.gl_sampler_routing=GL_TRUE;context.gl_sampler_mask=3;context.gl_sampler_cube_mask=2;
    context.dirty=PSGL_DIRTY_ALL;commands=attributes=0;error_code=0;
    check(!psgl_validate_draw_state(&context)&&!commands&&!attributes&&error_code==GL_INVALID_OPERATION,"mixed rejection before all commands");
    context.dirty=0;error_code=0;
    check(!psgl_validate_draw_state(&context)&&!commands&&!attributes&&error_code==GL_INVALID_OPERATION,"mixed rechecked without dirty bits");
    context.gl_sampler_units[1]=7;context.dirty=PSGL_DIRTY_TEXTURES;
    check(psgl_validate_draw_state(&context)&&attributes==1,"selection-only repair crosses draw boundary");
    texture.cube_dirty=GL_TRUE;context.dirty=PSGL_DIRTY_TEXTURES;fail_gpu=1;attributes=0;error_code=0;
    check(!psgl_validate_draw_state(&context)&&!attributes&&error_code==GL_OUT_OF_MEMORY,"allocation failure skips remaining draw validation");fail_gpu=0;
    fail_wait=1;error_code=0;
    check(!psgl_validate_draw_state(&context)&&!attributes&&error_code==GL_OUT_OF_MEMORY,"completion timeout skips remaining draw validation");fail_wait=0;
    psgl_cube_release(&context,&texture);
    printf("cube storage tests=%u pass=%u fail=%u\n",checks,checks-failures,failures);return failures?1:0;
}
