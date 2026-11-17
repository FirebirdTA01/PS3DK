#include "roll_file.h"
#include <sys/file.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#define CHECK(x) do { if (!(x)) { fprintf(stderr,"FAIL file:%d: %s\n",__LINE__,#x); exit(1); } } while (0)
static uint64_t size, position, request, count;
static unsigned seeks, transfers, stats, truncates, syncs;
static int32_t seek_error, transfer_error;
static int seek_wrong, resize_wrong, overcount;
static unsigned calls(void) {return seeks+transfers+stats+truncates+syncs;}
int32_t sysLv2FsLSeek64(int32_t fd,uint64_t offset,int32_t whence,uint64_t *out)
{
    CHECK(fd==7 && whence==0);++seeks;
    position=offset;*out=offset+(unsigned)seek_wrong;return seek_error;
}
int32_t sysLv2FsRead(int32_t fd,void *p,uint64_t n,uint64_t *out)
{
    CHECK(fd==7 && p);++transfers;request=n;*out=overcount?n+1:(count<n?count:n);return transfer_error;
}
int32_t sysLv2FsWrite(int32_t fd,const void *p,uint64_t n,uint64_t *out)
{return sysLv2FsRead(fd,(void *)p,n,out);}
int32_t sysLv2FsFStat(int32_t fd,sysFSStat *out)
{CHECK(fd==7);++stats;out->st_size=size;return 0;}
int32_t sysLv2FsFtruncate(int32_t fd,uint64_t n)
{CHECK(fd==7);++truncates;if(!resize_wrong)size=n;return 0;}
int32_t sysLv2FsFsync(int32_t fd) {CHECK(fd==7);++syncs;return 0;}
int main(void)
{
    const uint64_t high=(UINT64_C(1)<<32)+4096,cap=high+(UINT64_C(1)<<33);
    pfr_file f;pfr_io io;char b[16]={0};unsigned before;uint64_t got=0;
    CHECK(pfr_file_bind(&f,&io,7,cap)==PFR_OK);
    CHECK(io.resize(io.ctx,cap)==0 && size==cap);
    CHECK(io.size(io.ctx,&got)==0 && got==cap);
    count=16;
    CHECK(io.write_at(io.ctx,high,b,16)==16 && position==high && request==16);
    CHECK(io.read_at(io.ctx,high,b,16)==16 && position==high);
    count=3;CHECK(io.write_at(io.ctx,high,b,16)==3); /* short result preserved */
    count=0;CHECK(io.read_at(io.ctx,high,b,16)==0); /* no progress preserved */
    overcount=1;CHECK(io.write_at(io.ctx,high,b,16)<0);overcount=0;
    seek_wrong=1;before=transfers;
    CHECK(io.write_at(io.ctx,high,b,16)<0 && transfers==before);seek_wrong=0;
    seek_error=(int32_t)UINT32_C(0x80010004);before=transfers;
    CHECK(io.read_at(io.ctx,high,b,16)<0 && transfers==before && f.error==UINT32_C(0x80010004));seek_error=0;
    transfer_error=(int32_t)UINT32_C(0x8001002b);before=transfers;
    CHECK(io.write_at(io.ctx,high,b,16)<0 && transfers==before+1 && f.error==UINT32_C(0x8001002b));transfer_error=0;
    before=calls();
    CHECK(io.write_at(io.ctx,cap-8,b,16)<0);
    CHECK(io.read_at(io.ctx,UINT64_MAX,b,16)<0);
    CHECK(io.resize(io.ctx,cap+1)<0 && calls()==before);
    resize_wrong=1;CHECK(io.resize(io.ctx,4096)<0);resize_wrong=0;
    count=UINT64_MAX;
    CHECK(io.read_at(io.ctx,0,b,(size_t)INT_MAX+1u)==INT_MAX && request==INT_MAX);
    CHECK(io.sync(io.ctx)==0);
    puts("PASS LV2 positioned IO: wide offsets, seek equality, short results, errors and counts");
    return 0;
}
