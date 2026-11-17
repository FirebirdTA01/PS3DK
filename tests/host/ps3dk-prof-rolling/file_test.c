#define _POSIX_C_SOURCE 200809L
#define _FILE_OFFSET_BITS 64
#include "roll_file.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define CHECK(x) do { if (!(x)) { fprintf(stderr,"FAIL file:%d: %s\n",__LINE__,#x); exit(1); } } while (0)
static unsigned reads, writes, resizes, sizes, syncs;
static int fail_read, fail_write, fail_sync, fake_resize;
static size_t short_write;
static unsigned calls(void) { return reads+writes+resizes+sizes+syncs; }
ssize_t __real_pread64(int,void *,size_t,off_t);
ssize_t __real_pwrite64(int,const void *,size_t,off_t);
int __real_ftruncate64(int,off_t);
int __real_fstat64(int,struct stat *);
int __real_fsync(int);
ssize_t __wrap_pread64(int fd,void *p,size_t n,off_t off)
{ ++reads; if(fail_read) {errno=fail_read;return -1;} return __real_pread64(fd,p,n,off); }
ssize_t __wrap_pwrite64(int fd,const void *p,size_t n,off_t off)
{ ++writes; if(fail_write) {errno=fail_write;return -1;} if(short_write && n>short_write)n=short_write;return __real_pwrite64(fd,p,n,off); }
int __wrap_ftruncate64(int fd,off_t n)
{ ++resizes; return fake_resize ? 0:__real_ftruncate64(fd,n); }
int __wrap_fstat64(int fd,struct stat *s) { ++sizes;return __real_fstat64(fd,s); }
int __wrap_fsync(int fd) { ++syncs;if(fail_sync) {errno=EIO;return -1;} return __real_fsync(fd); }

static uint32_t crc(const unsigned char *p,size_t n) {
    uint32_t c=UINT32_MAX;
    while(n--){c^=*p++;for(unsigned i=0;i<8;i++)c=(c>>1)^(UINT32_C(0xedb88320)&(0u-(c&1u)));}
    return ~c;
}
static void packet(unsigned char p[20],uint64_t seq) {
    memset(p,0,20);p[0]=3; /* physical framing only */
    for(unsigned i=0;i<8;i++)p[8+i]=(unsigned char)(seq>>(8*i));
    uint32_t c=crc(p,16);
    for(unsigned i=0;i<4;i++)p[16+i]=(unsigned char)(c>>(8*i));
}
static void store_roundtrip(int fd) {
    pfr_config cfg={32768,2,128,256,{1}};pfr_layout layout;
    pfr_file f;pfr_io io;pfr_view before,after;unsigned char p[20];
    CHECK(pfr_plan(&cfg,&layout)==PFR_OK);
    void *scratch=malloc(layout.scratch_bytes),*state=malloc(pfr_state_bytes());
    void *second=malloc(pfr_state_bytes());
    CHECK(scratch && state && second);
    CHECK(pfr_file_bind(&f,&io,fd,layout.file_bytes)==PFR_OK);
    short_write=17;
    CHECK(pfr_format_v2(state,&io,&cfg,scratch,layout.scratch_bytes)==PFR_OK);
    for(unsigned i=0;i<8;i++) {
        packet(p,i);
        CHECK(pfr_commit(state,NULL,0,p,sizeof p)==PFR_OK);
        uint64_t n=0;CHECK(io.size(io.ctx,&n)==0 && n==layout.file_bytes);
    }
    short_write=0;
    CHECK(pfr_get_view(state,&before)==PFR_OK && before.evicted_segments==6);
    packet(p,8);fail_write=EINTR;
    CHECK(pfr_commit(state,NULL,0,p,sizeof p)==PFR_IO);
    unsigned prior=calls();
    CHECK(pfr_commit(state,NULL,0,p,sizeof p)==PFR_FAILED && calls()==prior);
    fail_write=0;
    CHECK(pfr_open(second,&io,&cfg,scratch,layout.scratch_bytes)==PFR_OK);
    CHECK(pfr_get_view(second,&after)==PFR_OK && after.generation==before.generation && after.next_chunk==8);
    free(second);free(state);free(scratch);
}

int main(int argc,char **argv)
{
    pfr_file f;
    pfr_io io;
    const uint64_t high=(UINT64_C(1)<<32)+4096, cap=high+8192;
    uint64_t size=99;
    unsigned before;
    char b[16]={0};
    const char sentinel[16]="wide-offset-OK";
    CHECK(argc==2);
    int fd=open(argv[1],O_RDWR|O_CREAT|O_EXCL,0600);
    CHECK(fd>=0);
    CHECK(pfr_file_bind(&f,&io,fd,cap)==PFR_OK);
    CHECK(f.fd==fd && f.extent==cap && !f.error && io.ctx==&f);
    CHECK(io.size(io.ctx,&size)==0 && size==0);
    CHECK(io.resize(io.ctx,cap)==0);
    CHECK(io.size(io.ctx,&size)==0 && size==cap);
    CHECK(io.write_at(io.ctx,high,sentinel,sizeof sentinel)==16);
    CHECK(io.read_at(io.ctx,high,b,sizeof b)==16 && !memcmp(b,sentinel,16));
    CHECK(lseek(fd,0,SEEK_CUR)==0); /* host pread/pwrite do not move the cursor */
    memset(b,1,sizeof b);
    CHECK(io.read_at(io.ctx,4096,b,sizeof b)==16);
    for(unsigned i=0;i<16;i++) CHECK(b[i]==0); /* no 32-bit wrap */
    CHECK(io.sync(io.ctx)==0);

    before=calls();
    CHECK(io.write_at(io.ctx,cap-1,sentinel,2)<0);
    CHECK(io.read_at(io.ctx,UINT64_MAX,b,1)<0);
    CHECK(io.write_at(io.ctx,0,sentinel,SIZE_MAX)<0);
    CHECK(io.resize(io.ctx,cap+1)<0);
    CHECK(calls()==before); /* rejection before any IO */

    fail_write=EINTR;before=writes;
    CHECK(io.write_at(io.ctx,high,sentinel,16)<0 && writes==before+1 && f.error==EINTR);
    fail_write=0;fail_read=EINTR;before=reads;
    CHECK(io.read_at(io.ctx,high,b,16)<0 && reads==before+1 && f.error==EINTR);
    fail_read=0;fail_sync=1;
    CHECK(io.sync(io.ctx)<0);fail_sync=0;
    fake_resize=1;
    CHECK(io.resize(io.ctx,4096)<0); /* a successful syscall alone is insufficient */
    fake_resize=0;
    CHECK(io.size(io.ctx,&size)==0 && size==cap);
    CHECK(io.resize(io.ctx,0)==0);

    before=calls();
    CHECK(pfr_file_bind(&f,&io,fd,UINT64_MAX)==PFR_INVALID);
    CHECK(pfr_file_bind(&f,&io,-1,cap)==PFR_INVALID);
    CHECK(calls()==before && fcntl(fd,F_GETFD)>=0); /* binding never closes */
    store_roundtrip(fd);
    CHECK(close(fd)==0 && unlink(argv[1])==0);
    puts("PASS positioned file IO above 4 GiB, bounds, EINTR and resize checks");
    return 0;
}
