/* Driver for independently encoded Python fixtures. No runtime trust claim. */
#include "roll_retention_inspect.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef struct { unsigned char *data; size_t bytes; unsigned reads, writes; uint64_t read_bytes; const char *mode; } memory;
static uint32_t u32(const unsigned char *p) { return (uint32_t)p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24; }
static uint64_t u64(const unsigned char *p) { return u32(p)|(uint64_t)u32(p+4)<<32; }
static int64_t read_at(void *ctx,uint64_t off,void *dst,size_t n) {
    memory *m=ctx; ++m->reads;
    if(!strcmp(m->mode,"zero") && m->reads==1) return 0;
    if(!strcmp(m->mode,"negative") && m->reads==1) return -1;
    if(!strcmp(m->mode,"overlarge") && m->reads==1) return (int64_t)n+1;
    if(!strcmp(m->mode,"short") && n>7) n=7;
    if(off>m->bytes || n>m->bytes-(size_t)off) return -1;
    memcpy(dst,m->data+(size_t)off,n); m->read_bytes+=n; return (int64_t)n;
}
static int size_of(void *ctx,uint64_t *n) { memory *m=ctx;if(!strcmp(m->mode,"size_fail"))return -1;*n=m->bytes;return 0; }
static int64_t write_at(void *ctx,uint64_t off,const void *b,size_t n) {memory *m=ctx;(void)off;(void)b;(void)n;++m->writes;return -1;}
static int resize(void *ctx,uint64_t n) {memory *m=ctx;(void)n;++m->writes;return -1;}
static int sync_io(void *ctx) {memory *m=ctx;++m->writes;return -1;}
int main(int argc,char **argv) {
    FILE *f; long length; memory m={0}; pfr_io io; void *store,*scratch,*work;
    pfi_result result,before; pfi_requirements req,req_before; pfi_buffers buffers; pfi_limits limits;
    int rc=99,unchanged=1; size_t slot_bytes;
    if(argc!=12) return 2;
    m.mode=argv[11]; f=fopen(argv[2],"rb");if(!f)return 3;
    if(fseek(f,0,SEEK_END) || (length=ftell(f))<0 || length>32*1024*1024 || fseek(f,0,SEEK_SET))return 4;
    m.bytes=(size_t)length;m.data=malloc(m.bytes?m.bytes:1);if(!m.data)return 5;
    if(fread(m.data,1,m.bytes,f)!=m.bytes || fclose(f))return 6;
    io=(pfr_io){&m,read_at,write_at,resize,size_of,sync_io};
    limits=(pfi_limits){strtoull(argv[5],NULL,10),(uint32_t)strtoul(argv[6],NULL,10),
        (uint32_t)strtoul(argv[7],NULL,10),(uint32_t)strtoul(argv[8],NULL,10)};
    slot_bytes=(size_t)strtoull(argv[9],NULL,10);if(slot_bytes>32*1024*1024)return 7;
    store=malloc(pfr_state_bytes());scratch=malloc(slot_bytes?slot_bytes:1);work=malloc(PFI_WORK_BYTES);
    if(!store||!scratch||!work)return 8;
    buffers=(pfi_buffers){store,pfr_state_bytes(),scratch,slot_bytes,work,(size_t)strtoull(argv[10],NULL,10)};
    if(buffers.work_bytes>PFI_WORK_BYTES)return 9;
    if(!strcmp(m.mode,"short_store"))--buffers.store_bytes;
    if(!strcmp(m.mode,"overlap"))buffers.work=buffers.slot;
    memset(&result,0xa5,sizeof result);memcpy(&before,&result,sizeof result);
    memset(&req,0xa5,sizeof req);memcpy(&req_before,&req,sizeof req);
    if(!strcmp(argv[1],"physical")) {
        pfr_config c; pfr_layout layout; pfr_view view;
        if(m.bytes<4096)return 10;
        memset(&c,0,sizeof c); c.max_bytes=u64(m.data+24);c.retained_slots=u32(m.data+32);
        c.checkpoint_bytes=u32(m.data+36);c.payload_bytes=u32(m.data+40);memcpy(c.session,m.data+48,16);
        rc=pfr_plan(&c,&layout);
        if(!rc)rc=pfr_open(store,&io,&c,scratch,slot_bytes);
        if(!rc)rc=pfr_get_view(store,&view);
    } else if(!strcmp(argv[1],"measure")) {
        rc=pfi_measure(&io,&limits,buffers.work,buffers.work_bytes,&req);
        unchanged=memcmp(&req,&req_before,sizeof req)==0;
        if(!rc)printf("{\"file\":%llu,\"slot\":%u,\"scratch\":%llu,\"store\":%llu,",
            (unsigned long long)req.layout.file_bytes,req.layout.slot_bytes,
            (unsigned long long)req.layout.scratch_bytes,(unsigned long long)req.store_bytes);
    } else if(!strcmp(argv[1],"inspect")) {
        rc=pfi_inspect(&io,&limits,&buffers,argv[3],strlen(argv[3]),strtoull(argv[4],NULL,10),&result);
        unchanged=memcmp(&result,&before,sizeof result)==0;
        if(!rc)printf("{\"file\":%llu,\"crc\":%u,\"kind\":%u,\"flags\":%u,\"evidence\":%u,"
            "\"generation\":%llu,\"first\":%llu,\"next\":%llu,\"recovered\":%u,",
            (unsigned long long)result.file_bytes,result.file_crc32,result.checkpoint_kind,result.original_flags,result.evidence,
            (unsigned long long)result.generation,(unsigned long long)result.first_chunk,
            (unsigned long long)result.next_chunk,result.recovered);
    } else return 11;
    if(rc || !strcmp(argv[1],"physical"))printf("{");
    printf("\"rc\":%d,\"unchanged\":%d,\"reads\":%u,\"read_bytes\":%llu,\"writes\":%u}\n",
        rc,unchanged,m.reads,(unsigned long long)m.read_bytes,m.writes);
    free(store);free(scratch);free(work);free(m.data);return 0;
}
