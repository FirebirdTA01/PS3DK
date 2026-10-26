#include <GLES/gl.h>
#include <PSGL/psgl.h>
#include <Cg/cgGL.h>
#include <Cg/cgBinary.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/process.h>
#include "vertex_vpo.h"
#include "vertex_cgb_vpo.h"
#include "tint_fpo.h"
#include "tint_cgb_fpo.h"
#include "classes_fpo.h"
#include "fixture_files.h"
#include "fixture_files_check.h"
SYS_PROCESS_PARAM(1001,0x100000);
extern CGprogram psgl_cg_create_program_from_memory(CGcontext,const void *,uint32_t,CGprofile);
static unsigned checks,failures;
static const float quad[24]={-1,-1,0,1,1,-1,0,1,1,1,0,1,-1,-1,0,1,1,1,0,1,-1,1,0,1};
static void check(int ok,const char *name){++checks;if(!ok){++failures;printf("PSGL_CG_CREATE_FAIL %s\n",name);}}
static int count_programs(CGcontext cg)
{
    int n=0;for(CGprogram p=cgGetFirstProgram(cg);p;p=cgGetNextProgram(p))++n;return n;
}
/* A refused creation returns NULL, reports exactly the expected error and
   adds no program to the context. */
static void refused(CGcontext cg,CGcontext target,CGenum type,const void *bytes,CGprofile profile,CGerror want,const char *name)
{
    int before=count_programs(cg);
    CGprogram p=cgCreateProgram(target,type,(const char *)bytes,profile,NULL,NULL);
    CGerror got=cgGetError();
    if(p||got!=want)printf("PSGL_CG_CREATE_REFUSAL %s program=%s error=%d want=%d\n",name,p?"nonnull":"null",(int)got,(int)want);
    check(!p&&got==want&&count_programs(cg)==before,name);
}
static void *copy_of(const void *bytes,unsigned size)
{
    void *out=malloc(size);if(out)memcpy(out,bytes,size);return out;
}
/* counted=0 records the result as an observation without a check. */
static void draw(CGprogram vp,CGprogram fp,float r,float g,float b,const unsigned char want[4],const char *name,int counted)
{
    unsigned char got[4]={0};
    cgGLBindProgram(vp);cgGLBindProgram(fp);
    CGparameter sc=cgGetNamedParameter(vp,"scale"),of=cgGetNamedParameter(vp,"offset"),ti=cgGetNamedParameter(fp,"tint");
    if(sc)cgGLSetParameter1f(sc,1.0f);
    if(of)cgGLSetParameter3f(of,0.0f,0.0f,0.0f);
    if(ti)cgGLSetParameter4f(ti,r,g,b,1.0f);
    unsigned n;for(n=1;n<=10;++n){
        glClearColor(0.0f,0.0f,0.0f,1.0f);glClear(GL_COLOR_BUFFER_BIT);
        glDrawArrays(GL_TRIANGLES,0,6);glReadPixels(4,4,1,1,GL_RGBA,GL_UNSIGNED_BYTE,got);
        if(!memcmp(got,want,4))break;
        if(n<10)usleep(200000);
    }
    /* Observations report tries=, so only checked draws carry attempts=. */
    printf("PSGL_CG_CREATE_%s %s %s=%u rgba=%02x%02x%02x%02x\n",counted?"DRAW":"OBSERVE",name,counted?"attempts":"tries",n>10?10:n,got[0],got[1],got[2],got[3]);
    GLenum error=glGetError();
    if(!counted)return;
    check(sc&&of&&ti,name);
    check(!memcmp(got,want,4)&&error==GL_NO_ERROR,name);
}
static void skipped(const char *name){check(0,name);check(0,name);}
static void list_parameters(CGprogram p,const char *tag)
{
    for(CGparameter q=p?cgGetFirstParameter(p,CG_PROGRAM):NULL;q;q=cgGetNextParameter(q))
        printf("PSGL_CG_CREATE_PARAM %s %s type=%d resource=%d index=%lu\n",tag,cgGetParameterName(q),(int)cgGetParameterType(q),(int)cgGetParameterResource(q),(unsigned long)cgGetParameterResourceIndex(q));
}
/* Fixture files for cgCreateProgramFromFile (fixture_files.h preserves the
   previous state of both paths). */
#define VP_FILE "/dev_hdd0/tmp/psgl-cg-create-vertex.vpo"
#define CUT_FILE "/dev_hdd0/tmp/psgl-cg-create-truncated.vpo"
static void class_of(CGprogram p,const char *param,CGparameterclass want)
{
    CGparameter q=p?cgGetNamedParameter(p,param):NULL;
    CGparameterclass got=q?cgGetParameterClass(q):CG_PARAMETERCLASS_UNKNOWN;
    printf("PSGL_CG_CREATE_CLASS %s type=%d class=%d want=%d\n",param,q?(int)cgGetParameterType(q):-1,(int)got,(int)want);
    check(q&&got==want,param);
}
static void type_class(CGtype type,CGparameterclass want,const char *name)
{
    CGparameterclass got=cgGetTypeClass(type);
    if(got!=want)printf("PSGL_CG_CREATE_TYPECLASS %s class=%d want=%d\n",name,(int)got,(int)want);
    check(got==want,name);
}
int main(void)
{
    PSGLinitOptions init={0};psglInit(&init);
    PSGLdevice *d=psglCreateDeviceAuto(0,0,0);PSGLcontext *c=psglCreateContext();
    if(!d||!c){puts("PSGL_CG_CREATE_FAIL setup");return 1;}
    psglMakeCurrent(c,d);glViewport(0,0,8,8);
    glDisable(GL_DEPTH_TEST);glDisable(GL_CULL_FACE);glDisable(GL_BLEND);glDisable(GL_DITHER);
    GLuint pos;glGenBuffers(1,&pos);glBindBuffer(GL_ARRAY_BUFFER,pos);glBufferData(GL_ARRAY_BUFFER,sizeof(quad),quad,GL_STATIC_DRAW);
    glVertexPointer(4,GL_FLOAT,16,0);glEnableClientState(GL_VERTEX_ARRAY);
    CGcontext cg=cgCreateContext();
    /* cgGLBindProgram installs a program only while its profile is enabled. */
    cgGLEnableProfile(CG_PROFILE_SCE_VP_RSX);cgGLEnableProfile(CG_PROFILE_SCE_FP_RSX);

    /* Binary containers through the public entry point: both container
       kinds, both program types that name a precompiled program. */
    CGprogram vp=cgCreateProgram(cg,CG_BINARY,(const char *)vertex_vpo,CG_PROFILE_SCE_VP_RSX,"main",NULL);
    check(vp&&cgGetError()==CG_NO_ERROR,"binary vertex container");
    check(vp&&cgIsProgram(vp)&&cgGetProgramProfile(vp)==CG_PROFILE_SCE_VP_RSX&&cgGetProgramContext(vp)==cg,"vertex program state");
    CGprogram fp=cgCreateProgram(cg,CG_OBJECT,(const char *)tint_fpo,CG_PROFILE_SCE_FP_RSX,"main",NULL);
    check(fp&&cgGetError()==CG_NO_ERROR,"object fragment container");
    check(fp&&cgGetProgramInput(fp)==CG_FRAGMENT,"fragment program state");
    CGprogram cvp=cgCreateProgram(cg,CG_BINARY,(const char *)vertex_cgb_vpo,CG_PROFILE_UNKNOWN,NULL,NULL);
    check(cvp&&cgGetError()==CG_NO_ERROR,"compact vertex container");
    check(cvp&&cgGetProgramProfile(cvp)==CG_PROFILE_SCE_VP_RSX,"compact vertex profile from container");
    CGprogram cfp=cgCreateProgram(cg,CG_OBJECT,(const char *)tint_cgb_fpo,CG_PROFILE_UNKNOWN,NULL,NULL);
    check(cfp&&cgGetError()==CG_NO_ERROR,"compact fragment container");
    check(cfp&&cgGetProgramProfile(cfp)==CG_PROFILE_SCE_FP_RSX,"compact fragment profile from container");
    CGprogram rfp=cgCreateProgram(cg,CG_BINARY,(const char *)tint_fpo,CG_PROFILE_UNKNOWN,NULL,NULL);
    check(rfp&&cgGetProgramInput(rfp)==CG_FRAGMENT,"raw fragment profile from container");
    check(count_programs(cg)==5,"created programs listed");

    /* The container is copied: the caller's bytes may go away. */
    void *scratch=copy_of(tint_fpo,tint_fpo_size);
    CGprogram copied=scratch?cgCreateProgram(cg,CG_BINARY,(const char *)scratch,CG_PROFILE_SCE_FP_RSX,NULL,NULL):NULL;
    if(scratch){memset(scratch,0xa5,tint_fpo_size);free(scratch);}
    check(copied&&cgGetNamedParameter(copied,"tint"),"caller buffer released");

    static const unsigned char rgb1[4]={51,102,153,255},rgb2[4]={255,0,102,255},rgb3[4]={0,204,51,255};
    if(vp&&fp)draw(vp,fp,0.2f,0.4f,0.6f,rgb1,"raw pair draws",1);else skipped("raw pair draws");
    if(vp&&copied)draw(vp,copied,0.0f,0.8f,0.2f,rgb3,"released buffer draws",1);else skipped("released buffer draws");
    /* Drawing with compact containers is outside this row's claim: each
       stage combination is observed without a check. */
    list_parameters(cvp,"compact-vertex");list_parameters(cfp,"compact-fragment");
    list_parameters(vp,"raw-vertex");list_parameters(fp,"raw-fragment");
    if(cvp&&fp)draw(cvp,fp,1.0f,0.0f,0.4f,rgb2,"compact vertex raw fragment",0);
    if(vp&&cfp)draw(vp,cfp,1.0f,0.0f,0.4f,rgb2,"raw vertex compact fragment",0);
    if(cvp&&cfp)draw(cvp,cfp,1.0f,0.0f,0.4f,rgb2,"compact pair",0);

    /* Refusals. */
    refused(cg,NULL,CG_BINARY,vertex_vpo,CG_PROFILE_SCE_VP_RSX,CG_INVALID_CONTEXT_HANDLE_ERROR,"invalid context");
    refused(cg,cg,CG_BINARY,NULL,CG_PROFILE_SCE_VP_RSX,CG_INVALID_PARAMETER_ERROR,"null program");
    refused(cg,cg,CG_SOURCE,"float4 main() : COLOR { return 1; }",CG_PROFILE_SCE_FP_RSX,CG_INVALID_ENUMERANT_ERROR,"source refused");
    const char *listing=cgGetLastListing(cg);
    check(listing&&strstr(listing,"rsx-cg-compiler"),"source refusal listing");
    refused(cg,cg,CG_GLOBAL,vertex_vpo,CG_PROFILE_SCE_VP_RSX,CG_INVALID_ENUMERANT_ERROR,"unknown program type");
    static unsigned char garbage[64];memset(garbage,0x5a,sizeof(garbage));
    refused(cg,cg,CG_BINARY,garbage,CG_PROFILE_SCE_VP_RSX,CG_PROGRAM_LOAD_ERROR,"not a container");
    CgBinaryProgram *bad=(CgBinaryProgram *)copy_of(vertex_vpo,vertex_vpo_size);
    if(bad){
        bad->revision^=0x100u;
        refused(cg,cg,CG_BINARY,bad,CG_PROFILE_SCE_VP_RSX,CG_PROGRAM_LOAD_ERROR,"wrong revision");
        bad->revision^=0x100u;bad->totalSize=(uint32_t)offsetof(CgBinaryProgram,data)+4u;
        /* The backing allocation stays full size: this checks a declared
           totalSize too small for the container's own offsets, not a short
           buffer (the unsized entry point cannot see the caller's length). */
        refused(cg,cg,CG_BINARY,bad,CG_PROFILE_SCE_VP_RSX,CG_PROGRAM_LOAD_ERROR,"undersized declared totalSize");
        bad->totalSize=0x7fffffffu;
        refused(cg,cg,CG_BINARY,bad,CG_PROFILE_SCE_VP_RSX,CG_PROGRAM_LOAD_ERROR,"oversized container");
        free(bad);
    }else{check(0,"wrong revision");check(0,"undersized declared totalSize");check(0,"oversized container");}
    refused(cg,cg,CG_BINARY,vertex_vpo,CG_PROFILE_SCE_FP_RSX,CG_INVALID_PROFILE_ERROR,"vertex container fragment profile");
    refused(cg,cg,CG_OBJECT,tint_cgb_fpo,CG_PROFILE_SCE_VP_RSX,CG_INVALID_PROFILE_ERROR,"compact fragment vertex profile");
    refused(cg,cg,CG_BINARY,vertex_vpo,(CGprofile)12345,CG_INVALID_PROFILE_ERROR,"unsupported profile");
    CGprogram again=cgCreateProgram(cg,CG_BINARY,(const char *)vertex_vpo,CG_PROFILE_SCE_VP_RSX,NULL,NULL);
    check(again&&cgGetError()==CG_NO_ERROR&&cgGetLastListing(cg)==NULL,"success clears listing");
    CGprogram ff=cgCreateProgramFromFile(cg,CG_OBJECT,"/dev_hdd0/tmp/psgl-cg-create-missing.vpo",CG_PROFILE_SCE_VP_RSX,NULL,NULL);
    check(!ff&&cgGetError()==CG_FILE_READ_ERROR,"object file type accepted");

    /* The fixture helpers, against injected faults (in memory only). */
    fixture_files_selfcheck(check);

    /* cgCreateProgramFromFile from a real container file, and from a file
       truncated to half its container (the file length is known here).
       Both paths are snapshotted before anything is written; without both
       snapshots nothing is written and the dependent checks fail. */
    const FixtureOps *fs=&fixture_real_ops;
    FixtureSnapshot saved_vp,saved_cut;
    int saved=fixture_snapshot_pair(fs,&saved_vp,VP_FILE,&saved_cut,CUT_FILE);
    check(saved,"fixture paths saved");
    if(saved){
        check(fixture_write(fs,VP_FILE,vertex_vpo,vertex_vpo_size),"fixture written");
        CGprogram fvp=cgCreateProgramFromFile(cg,CG_OBJECT,VP_FILE,CG_PROFILE_UNKNOWN,NULL,NULL);
        check(fvp&&cgGetError()==CG_NO_ERROR,"object file program");
        check(fvp&&cgGetProgramProfile(fvp)==CG_PROFILE_SCE_VP_RSX&&cgGetProgramContext(fvp)==cg,"object file profile from container");
        check(fs->remove(fs->ctx,VP_FILE)==0,"fixture removed after creation");
        static const unsigned char rgb4[4]={153,51,0,255};
        if(fvp&&fp)draw(fvp,fp,0.6f,0.2f,0.0f,rgb4,"removed file program draws",1);else skipped("removed file program draws");
        check(fixture_write(fs,CUT_FILE,vertex_vpo,vertex_vpo_size/2u),"truncated fixture written");
        int before_cut=count_programs(cg);
        CGprogram cut=cgCreateProgramFromFile(cg,CG_BINARY,CUT_FILE,CG_PROFILE_SCE_VP_RSX,NULL,NULL);
        CGerror cut_error=cgGetError();
        if(cut||cut_error!=CG_PROGRAM_LOAD_ERROR)printf("PSGL_CG_CREATE_REFUSAL truncated file program=%s error=%d want=%d\n",cut?"nonnull":"null",(int)cut_error,(int)CG_PROGRAM_LOAD_ERROR);
        check(!cut&&cut_error==CG_PROGRAM_LOAD_ERROR&&count_programs(cg)==before_cut,"truncated file refused");
        int restored_vp=fixture_snapshot_restore(fs,&saved_vp);
        int restored_cut=fixture_snapshot_restore(fs,&saved_cut);
        if(!restored_vp||!restored_cut)printf("PSGL_CG_CREATE_RESTORE_FAILED vertex=%d truncated=%d\n",restored_vp,restored_cut);
        check(restored_vp&&restored_cut,"fixture paths restored");
    }else{
        static const char *const dependent[]={"fixture written","object file program","object file profile from container",
            "fixture removed after creation","removed file program draws","removed file program draws",
            "truncated fixture written","truncated file refused","fixture paths restored"};
        puts("PSGL_CG_CREATE_FIXTURE_SKIPPED snapshots failed; fixture paths left untouched");
        for(unsigned i=0;i<sizeof(dependent)/sizeof(dependent[0]);++i)check(0,dependent[i]);
    }

    /* cgGetLastErrorString reports the pending error and clears it. */
    CGprogram none=cgCreateProgram(cg,CG_BINARY,NULL,CG_PROFILE_SCE_VP_RSX,NULL,NULL);
    CGerror last=CG_NO_ERROR;const char *message=cgGetLastErrorString(&last);
    check(!none&&last==CG_INVALID_PARAMETER_ERROR&&message&&!strcmp(message,"The parameter used is invalid."),"last error string");
    check(cgGetError()==CG_NO_ERROR,"last error string clears");
    check(!strcmp(cgGetErrorString(CG_PROGRAM_LOAD_ERROR),"The program could not load."),"error string");
    check(!strcmp(cgGetErrorString(CG_NO_ERROR),"No error has occurred."),"no error string");

    /* Parameter classes, from the raw container (LevelC types). */
    CGprogram classes=psgl_cg_create_program_from_memory(cg,classes_fpo,classes_fpo_size,CG_PROFILE_SCE_FP_RSX);
    check(classes!=NULL,"classes fixture");
    list_parameters(classes,"classes");
    class_of(classes,"tint",CG_PARAMETERCLASS_VECTOR);
    class_of(classes,"gain",CG_PARAMETERCLASS_SCALAR);
    class_of(classes,"line",CG_PARAMETERCLASS_SAMPLER);
    class_of(classes,"image",CG_PARAMETERCLASS_SAMPLER);
    class_of(classes,"volume",CG_PARAMETERCLASS_SAMPLER);
    class_of(classes,"environment",CG_PARAMETERCLASS_SAMPLER);
    class_of(classes,"rect",CG_PARAMETERCLASS_SAMPLER);
    type_class(CG_SAMPLER1D,CG_PARAMETERCLASS_SAMPLER,"type sampler1D");
    type_class(CG_SAMPLER2D,CG_PARAMETERCLASS_SAMPLER,"type sampler2D");
    type_class(CG_SAMPLER3D,CG_PARAMETERCLASS_SAMPLER,"type sampler3D");
    type_class(CG_SAMPLERCUBE,CG_PARAMETERCLASS_SAMPLER,"type samplerCUBE");
    type_class(CG_SAMPLERRECT,CG_PARAMETERCLASS_SAMPLER,"type samplerRECT");
    type_class(CG_SAMPLER,CG_PARAMETERCLASS_SAMPLER,"type sampler");
    type_class(CG_FLOAT,CG_PARAMETERCLASS_SCALAR,"type float");
    type_class(CG_HALF,CG_PARAMETERCLASS_SCALAR,"type half");
    type_class(CG_FIXED,CG_PARAMETERCLASS_SCALAR,"type fixed");
    type_class(CG_BOOL,CG_PARAMETERCLASS_SCALAR,"type bool");
    type_class(CG_FLOAT3,CG_PARAMETERCLASS_VECTOR,"type float3");
    type_class(CG_FIXED2,CG_PARAMETERCLASS_VECTOR,"type fixed2");
    type_class(CG_FLOAT4x4,CG_PARAMETERCLASS_MATRIX,"type float4x4");
    type_class(CG_HALF4x4,CG_PARAMETERCLASS_MATRIX,"type half4x4");
    type_class(CG_INT2x2,CG_PARAMETERCLASS_MATRIX,"type int2x2");
    type_class(CG_BOOL3x3,CG_PARAMETERCLASS_MATRIX,"type bool3x3");
    type_class(CG_FIXED4x4,CG_PARAMETERCLASS_MATRIX,"type fixed4x4");
    type_class(CG_STRUCT,CG_PARAMETERCLASS_STRUCT,"type struct");
    type_class(CG_ARRAY,CG_PARAMETERCLASS_ARRAY,"type array");
    type_class(CG_STRING,CG_PARAMETERCLASS_OBJECT,"type string");
    type_class(CG_TEXTURE,CG_PARAMETERCLASS_OBJECT,"type texture");
    type_class(CG_UNKNOWN_TYPE,CG_PARAMETERCLASS_UNKNOWN,"type unknown");

    cgDestroyContext(cg);glDeleteBuffers(1,&pos);
    check(glGetError()==GL_NO_ERROR,"cleanup");
    printf("PSGL_CG_CREATE_%s checks=%u failures=%u\n",failures?"FAIL":"OK",checks,failures);
    psglDestroyContext(c);psglDestroyDevice(d);psglExit();return failures?1:0;
}
