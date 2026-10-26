#include <GLES/gl.h>
#include <GLES/glext.h>
#include <PSGL/psgl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/process.h>
#include "vertex_vpo.h"
#include "fragment_fpo.h"
SYS_PROCESS_PARAM(1001, 0x100000);
static unsigned checks, failures;
static GLuint positions, red_buffer, green_buffer, blue_buffer;
static const float quad[24] = {
    -1,-1,0,1, 1,-1,0,1, 1,1,0,1,
    -1,-1,0,1, 1,1,0,1, -1,1,0,1
};
static void check(int ok, const char *name)
{
    ++checks;
    if (!ok) { ++failures; printf("PSGL_ATTR_FAIL %s\n",name); }
}
static GLuint buffer(const float *data, size_t size)
{
    GLuint name = 0;
    glGenBuffers(1,&name); glBindBuffer(GL_ARRAY_BUFFER,name);
    glBufferData(GL_ARRAY_BUFFER,size,data,GL_STATIC_DRAW);
    return name;
}
static void array(GLuint index, GLuint object, const void *pointer)
{
    glBindBuffer(GL_ARRAY_BUFFER,object);
    glVertexAttribPointer(index,4,GL_FLOAT,GL_FALSE,16,pointer);
    glEnableVertexAttribArray(index);
}
static void linked(GLuint program, GLuint position, GLuint color)
{
    GLint status = GL_FALSE;
    glBindAttribLocation(program,position,"position");
    glBindAttribLocation(program,color,"color");
    glLinkProgram(program);
    glGetProgramiv(program,GL_LINK_STATUS,&status);
    check(status == GL_TRUE,"link status");
    check(glGetAttribLocation(program,"position") == (GLint)position,"linked position");
    check(glGetAttribLocation(program,"color") == (GLint)color,"linked color");
    glUseProgram(program);
}
static void rendered(unsigned char r, unsigned char g, unsigned char b,
                      const char *name)
{
    unsigned char pixel[4] = {0,0,0,0};
    const unsigned char want[4] = {r,g,b,255};
    glClearColor(0,0,0,0); glClear(GL_COLOR_BUFFER_BIT);
    unsigned tries;
    for (tries=1; tries<=10; ++tries) {
        glDrawArrays(GL_TRIANGLES,0,6);
        glReadPixels(4,4,1,1,GL_RGBA,GL_UNSIGNED_BYTE,pixel);
        if (!memcmp(pixel,want,4)) break;
        if (tries<10) usleep(200000);
    }
    printf("PSGL_ATTR_DRAW %s attempts=%u pixel=%02x%02x%02x%02x\n",
           name,tries>10?10:tries,pixel[0],pixel[1],pixel[2],pixel[3]);
    GLenum error=glGetError();
    check(!memcmp(pixel,want,4) && error==GL_NO_ERROR,name);
}
int main(void)
{
#define SIX(...) __VA_ARGS__,__VA_ARGS__,__VA_ARGS__,__VA_ARGS__,__VA_ARGS__,__VA_ARGS__
    static const float red[24]={SIX(1,0,0,1)};
    static const float green[24]={SIX(0,1,0,1)};
    static const float blue[24]={SIX(0,0,1,1)};
#undef SIX
    PSGLinitOptions options={0};
    psglInit(&options);
    PSGLdevice *device=psglCreateDeviceAuto(0,0,0);
    PSGLcontext *context=psglCreateContext();
    if(!device || !context){puts("PSGL_ATTR_FAIL setup");return 1;}
    psglMakeCurrent(context,device);
    glViewport(0,0,8,8);
    glDisable(GL_DEPTH_TEST);glDisable(GL_CULL_FACE);glDisable(GL_BLEND);
    glDisable(GL_DITHER);glDisable(GL_SCISSOR_TEST);
    GLuint vs=glCreateShader(GL_VERTEX_SHADER),fs=glCreateShader(GL_FRAGMENT_SHADER);
    glShaderBinary(1,&vs,GL_SHADER_BINARY_FORMAT_CGB_PS3DK,vertex_vpo,vertex_vpo_size);
    glShaderBinary(1,&fs,GL_SHADER_BINARY_FORMAT_CGB_PS3DK,fragment_fpo,fragment_fpo_size);
    GLuint a=glCreateProgram(),b=glCreateProgram();
    glAttachShader(a,vs);glAttachShader(a,fs);glAttachShader(b,vs);glAttachShader(b,fs);
    check(a && b && glGetError()==GL_NO_ERROR,"shader objects");
    positions=buffer(quad,sizeof(quad));red_buffer=buffer(red,sizeof(red));
    green_buffer=buffer(green,sizeof(green));blue_buffer=buffer(blue,sizeof(blue));
    linked(a,0,2);array(0,positions,0);array(2,blue_buffer,0);
    rendered(0,0,255,"low-location control");
    glDisableVertexAttribArray(0);glDisableVertexAttribArray(2);

    linked(a,7,5);array(7,positions,0);array(5,red_buffer,0);array(6,green_buffer,0);
    rendered(255,0,0,"high-location VBO");
    glBindAttribLocation(a,6,"color");
    check(glGetAttribLocation(a,"color")==5,"pending binding not visible before relink");
    rendered(255,0,0,"pending binding does not reroute draw");
    /* Relinking the current program must install its new routing without
       requiring a second glUseProgram call. */
    glLinkProgram(a);
    check(glGetAttribLocation(a,"color")==6,"relink activates pending binding");
    rendered(0,255,0,"relinked green");
    linked(b,4,3);array(4,positions,0);array(3,blue_buffer,0);
    rendered(0,0,255,"second program");
    glUseProgram(a);rendered(0,255,0,"first program routing restored");
    array(7,0,quad);rendered(0,255,0,"client position array");
    glDisableVertexAttribArray(6);glVertexAttrib4f(6,0,0,1,1);
    rendered(0,0,255,"disabled current attribute");
    glEnableVertexAttribArray(6);rendered(0,255,0,"array reenabled");
    check(glGetAttribLocation(a,"absent")==-1,"inactive name absent");
    glEnableVertexAttribArray(8);check(glGetError()==GL_INVALID_VALUE,"index8 rejected");
    rendered(0,255,0,"invalid index preserves routing");
    GLint limit=0;glGetIntegerv(GL_MAX_VERTEX_ATTRIBS,&limit);
    check(limit==8 && glGetError()==GL_NO_ERROR,"eight logical locations reported");
    for(unsigned i=0;i<8;++i) {
        unsigned c=(i+1)%8;
        linked(b,i,c);array(i,positions,0);array(c,red_buffer,0);
        rendered(255,0,0,"all logical locations");
    }
    glUseProgram(0);glDeleteProgram(a);glDeleteProgram(b);glDeleteShader(vs);glDeleteShader(fs);
    glDeleteBuffers(1,&positions);glDeleteBuffers(1,&red_buffer);
    glDeleteBuffers(1,&green_buffer);glDeleteBuffers(1,&blue_buffer);
    printf("PSGL_ATTR_%s checks=%u failures=%u\n",failures?"FAIL":"OK",checks,failures);
    psglDestroyContext(context);psglDestroyDevice(device);psglExit();
    return failures?1:0;
}
