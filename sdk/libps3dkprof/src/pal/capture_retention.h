/* Completed-capture retention, shared POSIX implementation for host and LV2.
 * Internal only. Caller has closed the current capture successfully. The
 * index owns ONLY opt-in completed captures, in completion order. No glob,
 * mtime inference, recursive deletion, or cleanup of another app's files.
 * A crash may leave the exclusive lock: subsequent cleanup refuses until an
 * operator removes it after checking that no process holds it. */
#ifndef PS3DK_CAPTURE_RETENTION_H
#define PS3DK_CAPTURE_RETENTION_H
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define PR_MAX 1025u
#define PR_HEADER 64u
#define PR_RECORD 24u
#define PR_BYTES (PR_HEADER + PR_MAX * PR_RECORD + 4u)

static uint32_t pr_u32(const uint8_t *p) {
    return (uint32_t)p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24;
}
static uint64_t pr_u64(const uint8_t *p) { return pr_u32(p)|(uint64_t)pr_u32(p+4)<<32; }
static void pr_put32(uint8_t *p,uint32_t v) {
    for(unsigned i=0;i<4;++i) p[i]=(uint8_t)(v>>(i*8));
}
static void pr_put64(uint8_t *p,uint64_t v) {
    pr_put32(p,(uint32_t)v); pr_put32(p+4,(uint32_t)(v>>32));
}
static uint32_t pr_crc_more(uint32_t c,const uint8_t *p,size_t n) {
    while(n--) {
        c^=*p++;
        for(unsigned i=0;i<8;++i) c=(c>>1)^(0xedb88320u&(0u-(c&1)));
    }
    return c;
}
static uint32_t pr_crc(const uint8_t *p,size_t n) { return ~pr_crc_more(~0u,p,n); }
static int pr_read(int fd,void *dst,size_t n) {
    uint8_t *p=(uint8_t *)dst;
    while(n) {
        ssize_t r=read(fd,p,n);
        if(r<0 && errno==EINTR) continue;
        if(r<=0 || (size_t)r>n) return -1;
        p+=r; n-=(size_t)r;
    }
    return 0;
}
static int pr_write(int fd,const void *src,size_t n) {
    const uint8_t *p=(const uint8_t *)src;
    while(n) {
        ssize_t r=write(fd,p,n);
        if(r<0 && errno==EINTR) continue;
        if(r<=0 || (size_t)r>n) return -1;
        p+=r; n-=(size_t)r;
    }
    return 0;
}
static int pr_regular(int fd,uint64_t *size) {
    struct stat s;
    if(fstat(fd,&s) || !S_ISREG(s.st_mode) || s.st_size<0) return -1;
    *size=(uint64_t)s.st_size; return 0;
}
static int pr_name(char *out,size_t cap,const char *dir,const char *app,
                   uint64_t session) {
    int n=snprintf(out,cap,"%s/%s-%016llx.prof",dir,app,(unsigned long long)session);
    return n<0 || (size_t)n>=cap ? -1 : 0;
}

/* Validate framing and identity, not the reader's semantic verdict. Only a
 * real last TRAILER makes a file eligible. Streaming CRC avoids a capture-
 * sized allocation. This cold operation is charged to shutdown, never drain. */
static int pr_inspect(const char *path,const char *app,uint64_t session,
                      uint64_t *bytes,uint32_t *digest,int sync_data) {
    uint8_t head[16],block[4096],crc_bytes[4];
    uint64_t size=0,offset=16,seq=0; uint32_t whole=~0u,last=0;
    struct stat entry;
#ifdef PS3DK_PROF_PAL_LV2
    /* This runtime exposes stat/fstat, not lstat. Never recurse or delete a
     * resolved target: cleanup unlinks only the indexed app/session pathname.
     * Its complete bytes are checked again below. External concurrent edits
     * to managed paths are outside the per-app lock's contract. */
    if(stat(path,&entry)) return errno==ENOENT ? 1 : -1;
#else
    if(lstat(path,&entry)) return errno==ENOENT ? 1 : -1;
#endif
    if(!S_ISREG(entry.st_mode)) return -1;
    int fd=open(path,O_RDONLY),rc=-1;
    if(fd<0) return errno==ENOENT ? 1 : -1;
    if(pr_regular(fd,&size) || size<16 || pr_read(fd,head,16) ||
       memcmp(head,"PS3DKPRF",8) || pr_u32(head+8)!=1) goto done;
    whole=pr_crc_more(whole,head,16);
    while(offset<size) {
        if(size-offset<20 || last==6 || pr_read(fd,head,16)) goto done;
        uint32_t kind=pr_u32(head),len=pr_u32(head+4),c=pr_crc_more(~0u,head,16);
        if(pr_u64(head+8)!=seq++ || len>1048576 || (uint64_t)len+20>size-offset) goto done;
        whole=pr_crc_more(whole,head,16);
        uint32_t remaining=len;
        if(seq==1) {
            if(kind!=1 || len>sizeof block || pr_read(fd,block,len)) goto done;
            size_t at=0;
            for(unsigned k=0;k<6;++k) {
                if(len-at<2) goto done;
                size_t n=(size_t)block[at]|(size_t)block[at+1]<<8; at+=2;
                if(n>len-at) goto done;
                if(k==0 && (n!=strlen(app) || memcmp(block+at,app,n))) goto done;
                at+=n;
            }
            if(len-at!=24 || pr_u64(block+at+4)!=session) goto done;
            c=pr_crc_more(c,block,len); whole=pr_crc_more(whole,block,len); remaining=0;
        }
        while(remaining) {
            size_t n=remaining>sizeof block?sizeof block:remaining;
            if(pr_read(fd,block,n)) goto done;
            c=pr_crc_more(c,block,n); whole=pr_crc_more(whole,block,n); remaining-=(uint32_t)n;
        }
        if(pr_read(fd,crc_bytes,4) || pr_u32(crc_bytes)!=~c) goto done;
        whole=pr_crc_more(whole,crc_bytes,4); offset+=(uint64_t)len+20; last=kind;
    }
    if(last!=6 || seq<3) goto done;
    /* The current capture must reach the filesystem before its completion
     * index can authorize evicting an older one. Failure leaves all old files. */
    if(sync_data && fsync(fd)) goto done;
    *bytes=size; *digest=~whole; rc=0;
done:
    if(close(fd)) rc=-1;
    return rc;
}

/* Write replacement exclusively, sync its contents, then rename while the
 * per-app lock is held. Do not unlink an existing temp: its ownership is
 * uncertain. No durability claim about filesystem directory updates. */
static int pr_save(const char *index,const char *temp,uint8_t *b,unsigned count) {
    size_t n=PR_HEADER+(size_t)count*PR_RECORD;
    pr_put32(b+12,count); pr_put32(b+n,pr_crc(b,n));
    int fd=open(temp,O_WRONLY|O_CREAT|O_EXCL,0600);
    if(fd<0) return -1;
    int rc=pr_write(fd,b,n+4);
    if(!rc && fsync(fd)) rc=-1;
    if(close(fd)) rc=-1;
    if(!rc && rename(temp,index)) rc=-1;
    if(rc) (void)unlink(temp); /* only the file this invocation created */
    return rc;
}

static int pr_complete(const char *dir,const char *app,uint64_t session,unsigned keep) {
    size_t alen=app?strlen(app):0; int rc=-1,lockfd=-1,fd=-1;
    char lock[320],index[320],temp[320],path[320]; uint8_t *b=NULL;
    uint64_t current_size=0; uint32_t current_crc=0; unsigned count=0;
    if(!dir || !*dir || strlen(dir)>255 || !alen || alen>32 || !keep || keep>1024) return -1;
    for(size_t i=0;i<alen;++i) {
        char c=app[i];
        if(!((c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='_'||c=='-')) return -1;
    }
    if(pr_name(path,sizeof path,dir,app,session) ||
       pr_inspect(path,app,session,&current_size,&current_crc,1)) return -1;
    if(snprintf(lock,sizeof lock,"%s/.ps3dk-retain-%s.lock",dir,app)>=(int)sizeof lock ||
       snprintf(index,sizeof index,"%s/.ps3dk-retain-%s.index",dir,app)>=(int)sizeof index ||
       snprintf(temp,sizeof temp,"%s/.ps3dk-retain-%s.tmp",dir,app)>=(int)sizeof temp) return -1;
    lockfd=open(lock,O_WRONLY|O_CREAT|O_EXCL,0600);
    if(lockfd<0) return -1;
    b=(uint8_t *)calloc(1,PR_BYTES);
    if(!b) goto done;
    fd=open(index,O_RDONLY);
    if(fd<0 && errno!=ENOENT) goto done;
    if(fd>=0) {
        uint64_t size;
        if(pr_regular(fd,&size) || size<PR_HEADER+4 || size>PR_BYTES || pr_read(fd,b,(size_t)size)) goto done;
        if(close(fd)) {fd=-1; goto done;} fd=-1;
        count=pr_u32(b+12);
        if(memcmp(b,"P3RETAIN",8) || pr_u32(b+8)!=1 || count>PR_MAX ||
           size!=PR_HEADER+(uint64_t)count*PR_RECORD+4 ||
           pr_u32(b+(size_t)size-4)!=pr_crc(b,(size_t)size-4) ||
           memcmp(b+24,app,alen) || b[24+alen] || pr_u64(b+16)<count) goto done;
        for(unsigned i=0;i<count;++i) {
            const uint8_t *r=b+PR_HEADER+i*PR_RECORD;
            if(pr_u32(r+20)) goto done;
            for(unsigned j=0;j<i;++j)
                if(pr_u64(r)==pr_u64(b+PR_HEADER+j*PR_RECORD)) goto done;
            if(pr_u64(r)==session) goto done;
        }
    } else {
        memcpy(b,"P3RETAIN",8); pr_put32(b+8,1); memcpy(b+24,app,alen);
    }
    if(count==PR_MAX || pr_u64(b+16)==UINT64_MAX) goto done;
    uint8_t *r=b+PR_HEADER+count*PR_RECORD;
    pr_put64(r,session); pr_put64(r+8,current_size); pr_put32(r+16,current_crc); pr_put32(r+20,0);
    pr_put64(b+16,pr_u64(b+16)+1); ++count;
    if(pr_save(index,temp,b,count)) goto done;
    while(count>keep) {
        uint64_t old=pr_u64(b+PR_HEADER),size=0; uint32_t digest=0;
        if(pr_name(path,sizeof path,dir,app,old)) goto done;
        int check=pr_inspect(path,app,old,&size,&digest,0);
        if(check<0 || (!check && (size!=pr_u64(b+PR_HEADER+8) || digest!=pr_u32(b+PR_HEADER+16)))) goto done;
        if(!check && unlink(path)) goto done;
        /* ENOENT is an already-finished deletion after a failed index save. */
        --count; memmove(b+PR_HEADER,b+PR_HEADER+PR_RECORD,(size_t)count*PR_RECORD);
        if(pr_save(index,temp,b,count)) goto done;
    }
    rc=0;
done:
    if(fd>=0 && close(fd)) rc=-1;
    free(b);
    if(close(lockfd)) rc=-1;
    if(unlink(lock)) rc=-1;
    return rc;
}

/* Shared by both PAL translation units; no dependency on the core encoder. */
int ps3dk_prof__pal_retain_complete(const char *path,const char *app,
                                  uint64_t session,unsigned keep) {
    char dir[256];
    const char *slash=path?strrchr(path,'/'):NULL;
    if(!slash || slash==path || (size_t)(slash-path)>=sizeof dir) return -1;
    size_t n=(size_t)(slash-path); memcpy(dir,path,n); dir[n]=0;
    return pr_complete(dir,app,session,keep);
}
#endif
