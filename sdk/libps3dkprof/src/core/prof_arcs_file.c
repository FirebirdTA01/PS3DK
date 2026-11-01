#include "prof_arcs_file.h"
#include "../pal/pal.h"
#include <string.h>

enum { WRITING=1, REPLAY=2, CLOSED=3, REQUEST_MAX=65536, RETRIES=8 };
static int default_open(void *ctx,const char *p,int rw,int *fd,int *err)
{ (void)ctx;return rw?pal_open_rw_excl(p,fd,err):pal_open_excl(p,fd,err); }
static long default_read(void *ctx,int fd,void *p,unsigned long n,int *err)
{ (void)ctx;return pal_read(fd,p,n,err); }
static long default_write(void *ctx,int fd,const void *p,unsigned long n,int *err)
{ (void)ctx;return pal_write(fd,p,n,err); }
static int default_rewind(void *ctx,int fd,int *err)
{ (void)ctx;return pal_rewind(fd,err); }
static int default_sync(void *ctx,int fd,int *err)
{ (void)ctx;return pal_sync(fd,err); }
static int default_close(void *ctx,int fd,int *err)
{ (void)ctx;return pal_close(fd,err); }
static const struct ps3dk_prof_file_ops defaults={
    default_open,default_read,default_write,default_rewind,default_sync,default_close
};

static int fail(struct ps3dk_prof_arc_file *f,enum ps3dk_prof_file_operation op,int err)
{
    if(!f->error) {
        f->error=PS3DK_PROF_STREAM_IO;
        f->os_error=err?err:EIO;
        f->operation=op;
    }
    return f->error;
}
/* Called under busy; ownership is gone even if close reports EINTR. */
static void close_owned(struct ps3dk_prof_arc_file *f,int *owned,enum ps3dk_prof_file_operation op)
{
    if(*owned<0)return;
    int fd=*owned,err=0;
    *owned=-1;
    if(f->ops.close(f->ctx,fd,&err)!=0)fail(f,op,err);
}
static void close_all(struct ps3dk_prof_arc_file *f)
{
    close_owned(f,&f->output_fd,PS3DK_PROF_FILE_CLOSE_OUTPUT);
    close_owned(f,&f->spool_fd,PS3DK_PROF_FILE_CLOSE_SPOOL);
    f->phase=CLOSED;
}
static int open_file(struct ps3dk_prof_arc_file *f,const char *path,int rw,int *owned,
                     enum ps3dk_prof_file_operation op)
{
    for(unsigned retry=0;;++retry) {
        int fd=-1,err=0;
        if(f->ops.open(f->ctx,path,rw,&fd,&err)==0) {
            if(fd<0)return fail(f,op,EIO);
            *owned=fd;return 0;
        }
        if(err!=EINTR||retry==RETRIES)return fail(f,op,err);
    }
}
static int fd_op(struct ps3dk_prof_arc_file *f,int fd,
                 int (*fn)(void *,int,int *),enum ps3dk_prof_file_operation op)
{
    for(unsigned retry=0;;++retry) {
        int err=0;
        if(fn(f->ctx,fd,&err)==0)return 0;
        if(err!=EINTR||retry==RETRIES)return fail(f,op,err);
    }
}
static size_t bounded_len(const char *s,size_t limit)
{
    size_t n=0;
    if(s)while(n<limit&&s[n])++n;
    return n;
}
int ps3dk_prof__file_init(struct ps3dk_prof_arc_file *f,const char *dir,const char *token,
                        const struct ps3dk_prof_file_ops *ops,void *ctx)
{
    if(!f)return PS3DK_PROF_STREAM_INVALID;
    if(f->initialized||f->busy)return PS3DK_PROF_STREAM_STATE;
    if(!ops)ops=&defaults;
    size_t dn=bounded_len(dir,1024),tn=bounded_len(token,65);
    if(!dn||!tn||tn>64||dn>=1024)return PS3DK_PROF_STREAM_INVALID;
    /* Preserve root '/', but never send an accidental double separator to LV2. */
    while(dn>1&&dir[dn-1]=='/')--dn;
    size_t separator=dir[dn-1]=='/'?0:1;
    size_t prefix=dn+separator+tn;
    if(prefix+sizeof ".arcs.pending">1024||
       !ops->open||!ops->read||!ops->write||!ops->rewind||!ops->sync||!ops->close)
        return PS3DK_PROF_STREAM_INVALID;
    for(size_t i=0;i<tn;++i) {
        unsigned char c=(unsigned char)token[i];
        if(!((c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='_'||c=='-'))
            return PS3DK_PROF_STREAM_INVALID;
    }
    f->ops=*ops;f->ctx=ctx;f->initialized=1;f->busy=1;f->phase=WRITING;
    f->spool_fd=-1;f->output_fd=-1;
    memcpy(f->spool_path,dir,dn);
    if(separator)f->spool_path[dn]='/';
    memcpy(f->spool_path+dn+separator,token,tn);
    memcpy(f->pending_path,f->spool_path,prefix);
    memcpy(f->spool_path+prefix,".spool",sizeof ".spool");
    memcpy(f->pending_path+prefix,".arcs.pending",sizeof ".arcs.pending");
    int rc=open_file(f,f->spool_path,1,&f->spool_fd,PS3DK_PROF_FILE_OPEN_SPOOL);
    if(rc)close_all(f);
    f->busy=0;return rc;
}
static int write_all(struct ps3dk_prof_arc_file *f,const void *data,size_t n,int phase,
                     enum ps3dk_prof_file_operation op)
{
    if(!f||(!data&&n))return PS3DK_PROF_STREAM_INVALID;
    if(!f->initialized||f->busy||f->phase!=phase)return PS3DK_PROF_STREAM_STATE;
    if(f->error)return f->error;
    int fd=phase==WRITING?f->spool_fd:f->output_fd;
    f->busy=1;
    const unsigned char *p=data;
    unsigned retry=0;
    while(n) {
        unsigned long ask=n>REQUEST_MAX?REQUEST_MAX:(unsigned long)n;
        int err=0;long count=f->ops.write(f->ctx,fd,p,ask,&err);
        if(count<0&&err==EINTR&&retry<RETRIES){++retry;continue;}
        if(count<=0||(unsigned long)count>ask) {
            fail(f,op,count<0?err:EIO);break;
        }
        retry=0;p+=(size_t)count;n-=(size_t)count;
    }
    f->busy=0;return f->error;
}
int ps3dk_prof__file_spool_write(void *f,const void *data,size_t n)
{ return write_all(f,data,n,WRITING,PS3DK_PROF_FILE_WRITE_SPOOL); }
int ps3dk_prof__file_output_write(void *f,const void *data,size_t n)
{ return write_all(f,data,n,REPLAY,PS3DK_PROF_FILE_WRITE_OUTPUT); }
int ps3dk_prof__file_prepare(struct ps3dk_prof_arc_file *f)
{
    if(!f)return PS3DK_PROF_STREAM_INVALID;
    if(!f->initialized||f->busy||f->phase!=WRITING)return PS3DK_PROF_STREAM_STATE;
    f->busy=1;
    if(!f->error)fd_op(f,f->spool_fd,f->ops.sync,PS3DK_PROF_FILE_SYNC_SPOOL);
    if(!f->error)fd_op(f,f->spool_fd,f->ops.rewind,PS3DK_PROF_FILE_REWIND);
    if(!f->error)open_file(f,f->pending_path,0,&f->output_fd,PS3DK_PROF_FILE_OPEN_OUTPUT);
    if(f->error)close_all(f);else f->phase=REPLAY;
    f->busy=0;return f->error;
}
ptrdiff_t ps3dk_prof__file_read(void *ctx,void *data,size_t n)
{
    struct ps3dk_prof_arc_file *f=ctx;
    if(!f||(!data&&n))return -1;
    if(!f->initialized||f->busy||f->phase!=REPLAY||f->error)return -1;
    if(!n)return 0;
    f->busy=1;long count;
    unsigned long ask=n>REQUEST_MAX?REQUEST_MAX:(unsigned long)n;
    for(unsigned retry=0;;++retry) {
        int err=0;count=f->ops.read(f->ctx,f->spool_fd,data,ask,&err);
        if(count<0&&err==EINTR&&retry<RETRIES)continue;
        if(count<0||(unsigned long)count>ask) {
            fail(f,PS3DK_PROF_FILE_READ,count<0?err:EIO);count=-1;
        }
        break;
    }
    f->busy=0;return (ptrdiff_t)count;
}
int ps3dk_prof__file_output_flush(void *ctx)
{
    struct ps3dk_prof_arc_file *f=ctx;
    if(!f)return PS3DK_PROF_STREAM_INVALID;
    if(!f->initialized||f->busy||f->phase!=REPLAY)return PS3DK_PROF_STREAM_STATE;
    if(f->error)return f->error;
    f->busy=1;fd_op(f,f->output_fd,f->ops.sync,PS3DK_PROF_FILE_SYNC_OUTPUT);
    f->busy=0;return f->error;
}
int ps3dk_prof__file_output_close(void *ctx)
{
    struct ps3dk_prof_arc_file *f=ctx;
    if(!f)return PS3DK_PROF_STREAM_INVALID;
    if(!f->initialized||f->busy||f->output_fd<0)return PS3DK_PROF_STREAM_STATE;
    f->busy=1;close_owned(f,&f->output_fd,PS3DK_PROF_FILE_CLOSE_OUTPUT);f->phase=CLOSED;
    f->busy=0;return f->error;
}
int ps3dk_prof__file_cleanup(struct ps3dk_prof_arc_file *f)
{
    if(!f)return PS3DK_PROF_STREAM_INVALID;
    if(!f->initialized||f->busy)return PS3DK_PROF_STREAM_STATE;
    f->busy=1;close_all(f);f->busy=0;return f->error;
}
