#define _POSIX_C_SOURCE 200809L
#include "prof_arcs_file.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static struct ps3dk_prof_arc_file file;
static const char *mode;
static unsigned checks, opens, closes, reads, writes, interrupts, reentries;
static int spool_fd=-1, output_fd=-1;
static unsigned char data[131073], copy[131073];
#define CHECK(x) do {++checks;if(!(x)){fprintf(stderr,"FAIL line %d: %s\n",__LINE__,#x);exit(1);}} while(0)
static int is(const char *s) {return !strcmp(mode,s);}
static int at(const char *stage,const char *kind) {
    char s[80];int n=snprintf(s,sizeof s,"%s-%s",stage,kind);
    CHECK(n>0&&(size_t)n<sizeof s);return is(s);
}
static void reentry(void) {
    if(!is("reentry"))return;
    unsigned o=opens,c=closes,r=reads,w=writes;
    CHECK(ps3dk_prof__file_cleanup(&file)==PS3DK_PROF_STREAM_STATE);
    CHECK(ps3dk_prof__file_prepare(&file)==PS3DK_PROF_STREAM_STATE);
    CHECK(ps3dk_prof__file_spool_write(&file,data,1)==PS3DK_PROF_STREAM_STATE);
    CHECK(opens==o&&closes==c&&reads==r&&writes==w);++reentries;
}
static int fault(const char *stage,unsigned long n,long *result,int *err) {
    if(at(stage,"zero")){*result=0;return 1;}
    if(at(stage,"over")){*result=(long)n+1;return 1;}
    if(at(stage,"fail")){*result=-1;*err=EIO;return 1;}
    if(at(stage,"progress")&&interrupts++%9<8){*result=-1;*err=EINTR;return 1;}
    if(at(stage,"eintr8")||at(stage,"eintr9")) {
        unsigned limit=at(stage,"eintr8")?8:9;
        if(interrupts++<limit){*result=-1;*err=EINTR;return 1;}
    }
    return 0;
}
static int io_open(void *ctx,const char *path,int rw,int *fd,int *err) {
    CHECK(ctx==&file);++opens;reentry();
    if(is("root-path")){CHECK(!strcmp(path,"/t.spool"));*err=EACCES;return -1;}
    if(is(rw?"open-spool":"open-output")){*err=EACCES;return -1;}
    *fd=open(path,(rw?O_RDWR:O_WRONLY)|O_CREAT|O_EXCL,0600);
    if(*fd<0){*err=errno;return -1;}
    if(rw)spool_fd=*fd;else output_fd=*fd;return 0;
}
static long io_read(void *ctx,int fd,void *buf,unsigned long n,int *err) {
    CHECK(ctx==&file&&fd==spool_fd&&n<=65536);++reads;reentry();long r;
    if(fault("read",n,&r,err))return r;
    if((is("read-short")||is("read-progress"))&&n>7)n=7;
    r=(long)read(fd,buf,n);if(r<0)*err=errno;return r;
}
static long io_write(void *ctx,int fd,const void *buf,unsigned long n,int *err) {
    CHECK(ctx==&file&&n<=65536);++writes;reentry();long r;
    const char *stage=fd==spool_fd?"spool":"output";
    if(fault(stage,n,&r,err))return r;
    if((at(stage,"short")||at(stage,"progress"))&&n>7)n=7;
    r=(long)write(fd,buf,n);if(r<0)*err=errno;return r;
}
static int io_rewind(void *ctx,int fd,int *err) {
    CHECK(ctx==&file&&fd==spool_fd);reentry();
    if(is("rewind")){*err=EIO;return -1;}
    if(lseek(fd,0,SEEK_SET)!=0){*err=errno;return -1;}return 0;
}
static int io_sync(void *ctx,int fd,int *err) {
    CHECK(ctx==&file);reentry();
    if(is(fd==spool_fd?"sync-spool":"sync-output")){*err=ENOSPC;return -1;}
    if(fsync(fd)){*err=errno;return -1;}return 0;
}
static int io_close(void *ctx,int fd,int *err) {
    CHECK(ctx==&file);++closes;reentry();
    CHECK(close(fd)==0);
    if(is(fd==spool_fd?"close-spool":"close-output")||is("both-close")) {
        *err=EINTR;return -1;
    }
    return 0;
}
static const struct ps3dk_prof_file_ops ops={io_open,io_read,io_write,io_rewind,io_sync,io_close};
static void cleanup(int expected,unsigned count) {
    CHECK(ps3dk_prof__file_cleanup(&file)==expected);CHECK(closes==count);
    int os=file.os_error;enum ps3dk_prof_file_operation op=file.operation;
    CHECK(ps3dk_prof__file_cleanup(&file)==expected);CHECK(closes==count);
    CHECK(file.os_error==os&&file.operation==op);
}
static void marker(const char *p) {
    int fd=open(p,O_WRONLY|O_CREAT|O_EXCL,0600);CHECK(fd>=0);
    CHECK(write(fd,"keep",4)==4);CHECK(close(fd)==0);
}
static void check_marker(const char *p) {
    char b[5]={0};int fd=open(p,O_RDONLY);CHECK(fd>=0);
    CHECK(read(fd,b,5)==4&&!strcmp(b,"keep"));CHECK(close(fd)==0);
}
static int fails(const char *s) {return at(s,"zero")||at(s,"over")||at(s,"fail")||at(s,"eintr9");}
int main(int argc,char **argv) {
    if(argc!=3)return 2;mode=argv[1];const char *dir=argv[2];
    for(size_t i=0;i<sizeof data;++i)data[i]=(unsigned char)(i*17);
    if(is("invalid")) {
        const char *bad[]={"","..","a/b","a\\b","a.b","a b","\xc3\xa9"};
        for(size_t i=0;i<sizeof bad/sizeof bad[0];++i)
            CHECK(ps3dk_prof__file_init(&file,dir,bad[i],&ops,&file)==PS3DK_PROF_STREAM_INVALID);
        char long_token[66];memset(long_token,'a',65);long_token[65]=0;
        CHECK(ps3dk_prof__file_init(&file,dir,long_token,&ops,&file)==PS3DK_PROF_STREAM_INVALID);
        char long_dir[1025];memset(long_dir,'a',1024);long_dir[1024]=0;
        CHECK(ps3dk_prof__file_init(&file,long_dir,"t",&ops,&file)==PS3DK_PROF_STREAM_INVALID);
        struct ps3dk_prof_file_ops missing=ops;missing.close=NULL;
        CHECK(ps3dk_prof__file_init(&file,dir,"t",&missing,&file)==PS3DK_PROF_STREAM_INVALID);
        CHECK(!file.initialized&&!opens&&!closes);goto done;
    }
    char collide[1200];
    if(is("collision-spool")||is("collision-output")) {
        CHECK(snprintf(collide,sizeof collide,"%s/t.%s",dir,is("collision-spool")?"spool":"arcs.pending")>0);
        marker(collide);
    }
    int rc=ps3dk_prof__file_init(&file,dir,"t",is("pal")?NULL:&ops,&file);
    if(is("open-spool")||is("collision-spool")||is("root-path")) {
        CHECK(rc==PS3DK_PROF_STREAM_IO);CHECK(closes==0);
        CHECK(file.operation==PS3DK_PROF_FILE_OPEN_SPOOL);
        cleanup(PS3DK_PROF_STREAM_IO,0);
        if(is("collision-spool"))check_marker(collide);goto done;
    }
    CHECK(rc==0);
    CHECK(ps3dk_prof__file_init(&file,dir,"other",&ops,&file)==PS3DK_PROF_STREAM_STATE);
    if(is("path-boundary")||is("path-boundary-trailing"))CHECK(strlen(file.pending_path)==1023);
    if(is("trailing")||is("path-boundary-trailing")) {
        CHECK(strstr(file.pending_path,"//")==NULL&&strstr(file.spool_path,"//")==NULL);
    }
    if(is("cleanup-writing")){cleanup(0,1);goto done;}
    size_t amount=is("empty")?0:sizeof data;
    if(is("spool-progress")||is("output-progress")||is("read-progress"))amount=20;
    rc=ps3dk_prof__file_spool_write(&file,data,amount);
    if(fails("spool")) {
        CHECK(rc==PS3DK_PROF_STREAM_IO&&file.operation==PS3DK_PROF_FILE_WRITE_SPOOL);
        CHECK(closes==0);if(is("spool-eintr9"))CHECK(interrupts==9);
        unsigned w=writes;CHECK(ps3dk_prof__file_spool_write(&file,data,1)==PS3DK_PROF_STREAM_IO);
        CHECK(writes==w);cleanup(PS3DK_PROF_STREAM_IO,1);goto done;
    }
    CHECK(rc==0);if(is("spool-eintr8"))CHECK(interrupts>=9);
    rc=ps3dk_prof__file_prepare(&file);
    if(is("sync-spool")||is("rewind")||is("open-output")||is("collision-output")) {
        CHECK(rc==PS3DK_PROF_STREAM_IO);cleanup(PS3DK_PROF_STREAM_IO,1);
        if(is("collision-output"))check_marker(collide);goto done;
    }
    CHECK(rc==0);CHECK(ps3dk_prof__file_prepare(&file)==PS3DK_PROF_STREAM_STATE);
    CHECK(ps3dk_prof__file_spool_write(&file,data,1)==PS3DK_PROF_STREAM_STATE);
    if(is("preflight-cleanup")){cleanup(0,2);goto done;}
    size_t used=0;
    while(used<amount) {
        ptrdiff_t n=ps3dk_prof__file_read(&file,copy+used,amount-used);
        if(at("read","over")||at("read","fail")||at("read","eintr9")) {
            CHECK(n<0&&file.operation==PS3DK_PROF_FILE_READ);
            if(is("read-eintr9"))CHECK(interrupts==9);
            CHECK(closes==0);cleanup(PS3DK_PROF_STREAM_IO,2);goto done;
        }
        CHECK(n>0&&(size_t)n<=amount-used);used+=(size_t)n;
    }
    CHECK(memcmp(copy,data,amount)==0);
    CHECK(ps3dk_prof__file_read(&file,copy,1)==0);
    rc=ps3dk_prof__file_output_write(&file,data,amount);
    if(fails("output")) {
        CHECK(rc==PS3DK_PROF_STREAM_IO&&file.operation==PS3DK_PROF_FILE_WRITE_OUTPUT);
        CHECK(closes==0);if(is("output-eintr9"))CHECK(interrupts==9);
        CHECK(ps3dk_prof__file_output_close(&file)==PS3DK_PROF_STREAM_IO);
        cleanup(PS3DK_PROF_STREAM_IO,2);goto done;
    }
    CHECK(rc==0);rc=ps3dk_prof__file_output_flush(&file);
    if(is("sync-output")) {
        CHECK(rc==PS3DK_PROF_STREAM_IO&&file.os_error==ENOSPC);CHECK(closes==0);
        CHECK(ps3dk_prof__file_output_close(&file)==PS3DK_PROF_STREAM_IO);
        cleanup(PS3DK_PROF_STREAM_IO,2);goto done;
    }
    CHECK(rc==0);rc=ps3dk_prof__file_output_close(&file);
    if(is("close-output")||is("both-close"))CHECK(rc==PS3DK_PROF_STREAM_IO);else CHECK(rc==0);
    CHECK(ps3dk_prof__file_output_close(&file)==PS3DK_PROF_STREAM_STATE);
    if(is("pal")) {
        CHECK(ps3dk_prof__file_cleanup(&file)==0);
    } else cleanup(is("close-output")||is("close-spool")||is("both-close")?PS3DK_PROF_STREAM_IO:0,2);
    if(is("both-close"))CHECK(file.operation==PS3DK_PROF_FILE_CLOSE_OUTPUT&&file.os_error==EINTR);
    if(is("reentry"))CHECK(reentries>5);
done:
    {char final[1200];CHECK(snprintf(final,sizeof final,"%s/t.arcs",dir)>0);CHECK(access(final,F_OK)==-1&&errno==ENOENT);}
    printf("checks=%u opens=%u closes=%u\n",checks,opens,closes);return 0;
}
