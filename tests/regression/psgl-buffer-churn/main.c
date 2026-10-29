#include <GLES/gl.h>
#include <GLES/glext.h>
#include <PSGL/psgl.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/process.h>
SYS_PROCESS_PARAM(1001,0x100000);
int main(void)
{
    PSGLinitOptions init={0};psglInit(&init);
    PSGLdevice *dev=psglCreateDeviceAuto(0,0,0);PSGLcontext *ctx=psglCreateContext();
    if(!dev||!ctx)return 1;psglMakeCurrent(ctx,dev);
    GLuint pos,colors,indices;unsigned char bytes[192]={0};
    glGenBuffers(1,&pos);glGenBuffers(1,&colors);glGenBuffers(1,&indices);
    glBindBuffer(GL_ARRAY_BUFFER,pos);glBufferData(GL_ARRAY_BUFFER,192,bytes,GL_STATIC_DRAW);
    unsigned step=0;
    for(unsigned size=1;size<=3;++size)
        for(unsigned vbo=0;vbo<2;++vbo)
            for(unsigned layout=0;layout<3;++layout)
                for(unsigned mode=0;mode<3;++mode){
                    if(vbo){glBindBuffer(GL_ARRAY_BUFFER,colors);glBufferData(GL_ARRAY_BUFFER,160,bytes,GL_STATIC_DRAW);}
                    if(mode){glBindBuffer(GL_ELEMENT_ARRAY_BUFFER,indices);glBufferData(GL_ELEMENT_ARRAY_BUFFER,mode==1?14:28,bytes,GL_STATIC_DRAW);}
                    printf("CHURN step=%u\n",++step);
                }
    glBindBuffer(GL_ARRAY_BUFFER,colors);
    glBufferData(GL_ARRAY_BUFFER,160,bytes,GL_STATIC_DRAW);
    glBufferSubData(GL_ARRAY_BUFFER,0,160,bytes);
    glBufferData(GL_ARRAY_BUFFER,160,bytes,GL_STATIC_DRAW);
    glBufferData(GL_ARRAY_BUFFER,8,bytes,GL_STATIC_DRAW);
    glBufferData(GL_ARRAY_BUFFER,160,bytes,GL_STATIC_DRAW);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER,indices);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER,28,bytes,GL_STATIC_DRAW);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER,24,bytes,GL_STATIC_DRAW);
    puts("CHURN before final 28-byte replacement");
    glBufferData(GL_ELEMENT_ARRAY_BUFFER,28,bytes,GL_STATIC_DRAW);
    puts("CHURN after final 28-byte replacement");
    glDeleteBuffers(1,&pos);glDeleteBuffers(1,&colors);glDeleteBuffers(1,&indices);
    puts("CHURN_OK");psglDestroyContext(ctx);psglDestroyDevice(dev);psglExit();return 0;
}
