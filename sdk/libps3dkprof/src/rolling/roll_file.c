/* Private FILE adapter. The output owner serializes access to the descriptor.
 * No runtime feature switch or public profiler lifecycle is changed here. */
#ifndef PS3DK_PROF_PAL_LV2
#define _POSIX_C_SOURCE 200809L
#if defined(_FILE_OFFSET_BITS) && _FILE_OFFSET_BITS != 64
#error "rolling FILE IO requires 64-bit file offsets"
#endif
#define _FILE_OFFSET_BITS 64
#endif
#include "roll_file.h"
#include <errno.h>
#include <limits.h>
#include <string.h>
#ifdef PS3DK_PROF_PAL_LV2
#include <sys/file.h>
_Static_assert(sizeof(((sysFSStat *)0)->st_size)==8,"LV2 file size must be 64-bit");
#else
#include <sys/stat.h>
#include <unistd.h>
_Static_assert(sizeof(off_t)>=8,"host file offsets must be at least 64-bit");
#endif

static int failure(pfr_file *f,uint32_t code)
{
    f->error=code ? code:(uint32_t)EIO;
    return -1;
}
static int range(pfr_file *f,uint64_t offset,size_t bytes)
{
    f->error=0;
    if(offset>f->extent || (uint64_t)bytes>f->extent-offset)
        return failure(f,EINVAL);
    return 0;
}
/* A single positive short transfer is returned verbatim. EINTR is not retried;
 * the physical store treats any negative result as a poisoned transaction. */
static int64_t transfer(pfr_file *f,uint64_t offset,void *p,size_t bytes,int writing)
{
    if(range(f,offset,bytes)!=0) return -1;
    if(!bytes) return 0;
    if(!p) return failure(f,EINVAL);
    size_t count=bytes>(size_t)INT_MAX ? (size_t)INT_MAX:bytes;
#ifdef PS3DK_PROF_PAL_LV2
    uint64_t position=0,done=0;
    int32_t rc=sysLv2FsLSeek64(f->fd,offset,0,&position);
    if(rc) return failure(f,(uint32_t)rc);
    if(position!=offset) return failure(f,EIO);
    rc=writing ? sysLv2FsWrite(f->fd,p,(uint64_t)count,&done)
               : sysLv2FsRead(f->fd,p,(uint64_t)count,&done);
    if(rc) return failure(f,(uint32_t)rc);
    if(done>(uint64_t)count) return failure(f,EIO);
    return (int64_t)done;
#else
    ssize_t done=writing ? pwrite(f->fd,p,count,(off_t)offset)
                         : pread(f->fd,p,count,(off_t)offset);
    if(done<0) return failure(f,(uint32_t)errno);
    if((uint64_t)done>(uint64_t)count) return failure(f,EIO);
    return (int64_t)done;
#endif
}
static int64_t file_read(void *ctx,uint64_t offset,void *p,size_t bytes)
{ return transfer(ctx,offset,p,bytes,0); }
static int64_t file_write(void *ctx,uint64_t offset,const void *p,size_t bytes)
{ return transfer(ctx,offset,(void *)p,bytes,1); }

static int file_size(void *ctx,uint64_t *out)
{
    pfr_file *f=ctx;
    uint64_t size;
    f->error=0;
    if(!out) return failure(f,EINVAL);
#ifdef PS3DK_PROF_PAL_LV2
    sysFSStat st;
    int32_t rc=sysLv2FsFStat(f->fd,&st);
    if(rc) return failure(f,(uint32_t)rc);
    size=st.st_size;
#else
    struct stat st;
    if(fstat(f->fd,&st)!=0) return failure(f,(uint32_t)errno);
    if(st.st_size<0) return failure(f,EIO);
    size=(uint64_t)st.st_size;
#endif
    if(size>f->extent) return failure(f,EIO);
    *out=size;
    return 0;
}
static int file_resize(void *ctx,uint64_t bytes)
{
    pfr_file *f=ctx;
    uint64_t size=0;
    f->error=0;
    if(bytes>f->extent) return failure(f,EINVAL);
#ifdef PS3DK_PROF_PAL_LV2
    int32_t rc=sysLv2FsFtruncate(f->fd,bytes);
    if(rc) return failure(f,(uint32_t)rc);
#else
    if(ftruncate(f->fd,(off_t)bytes)!=0) return failure(f,(uint32_t)errno);
#endif
    if(file_size(f,&size)!=0) return -1;
    if(size!=bytes) return failure(f,EIO);
    return 0;
}
static int file_sync(void *ctx)
{
    pfr_file *f=ctx;
    f->error=0;
#ifdef PS3DK_PROF_PAL_LV2
    int32_t rc=sysLv2FsFsync(f->fd);
    if(rc) return failure(f,(uint32_t)rc);
#else
    if(fsync(f->fd)!=0) return failure(f,(uint32_t)errno);
#endif
    return 0;
}
int pfr_file_bind(pfr_file *f,pfr_io *io,int fd,uint64_t extent)
{
    if(!f || !io || fd<0 || extent>(uint64_t)INT64_MAX) return PFR_INVALID;
    memset(f,0,sizeof *f);
    f->fd=fd;f->extent=extent;
    *io=(pfr_io){f,file_read,file_write,file_resize,file_size,file_sync};
    return PFR_OK;
}
