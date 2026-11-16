#ifndef REPORT_COPY_ORDERING_LOGIC_H
#define REPORT_COPY_ORDERING_LOGIC_H
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#ifndef ORD_MUTANT
#define ORD_MUTANT 0
#endif
enum { ORD_A1, ORD_A2, ORD_B2 };
enum { ORD_INCONCLUSIVE, ORD_SUPPORTED, ORD_REFUTED };
enum { ORD_END, ORD_WRITE, ORD_REPORT_FENCE, ORD_COPY_FENCE, ORD_COPY_OLD,
       ORD_COPY_NEW, ORD_PUBLISH, ORD_EARLY, ORD_GATE };
static inline void ord_packet(uint32_t *p,uint32_t destination,unsigned index,unsigned bytes) {
    p[0]=(2u<<18)|(1u<<13)|0x184u;
    p[1]=ORD_MUTANT==5?0xfeed0000u:0x66626660u;p[2]=0xfeed0001u;
    p[3]=(8u<<18)|(1u<<13)|0x30cu;
    p[4]=index*16u;p[5]=destination;
    p[6]=bytes;p[7]=bytes;p[8]=bytes;p[9]=1;p[10]=0x101;p[11]=0;
}
static inline int ord_equal(const unsigned char *a,const unsigned char *b,size_t n) {
    if(ORD_MUTANT==1 && n)--n;
    return memcmp(a,b,n)==0;
}
/* Return a mismatch. B2 must use the frozen pre-drain observation. */
static inline int ord_evidence(unsigned claim,const unsigned char *expected,const unsigned char *settled,const unsigned char *immediate,size_t n) {
    return !ord_equal(expected,claim==ORD_B2 && ORD_MUTANT!=2?immediate:settled,n);
}
static inline int ord_guards(const volatile uint32_t *p,size_t total,size_t d0,size_t bytes,size_t d1,int second,uint32_t sentinel) {
    if(ORD_MUTANT==3)return 1;
    for(size_t i=0;i<total/4;i++) {
        size_t b=i*4;
        if((b>=d0 && b<d0+bytes)||(second && b>=d1 && b<d1+bytes))continue;
        if(p[i]!=sentinel)return 0;
    }
    return 1;
}
static inline unsigned ord_result(unsigned trials,unsigned bad,unsigned caught) {
    if(bad)return ORD_REFUTED;
    return trials==8 && (caught || ORD_MUTANT==4)?ORD_SUPPORTED:ORD_INCONCLUSIVE;
}
static inline int ord_times(const unsigned char *p,unsigned pairs,uint64_t old_end) {
    uint64_t previous=0;
    for(unsigned i=0;i<pairs;i++) {
        uint64_t begin,end;memcpy(&begin,p+i*32,8);memcpy(&end,p+i*32+16,8);
        if(!begin || end<begin || begin<previous || (!i && ORD_MUTANT!=7 && begin<=old_end))return 0;
        previous=end;
    }
    return 1;
}
static inline const unsigned char *ord_plan(unsigned claim,unsigned control) {
    static const unsigned char plans[3][2][8]={
      {{ORD_WRITE,ORD_REPORT_FENCE,ORD_COPY_OLD,ORD_PUBLISH,0},{ORD_COPY_OLD,ORD_COPY_FENCE,ORD_WRITE,ORD_REPORT_FENCE,ORD_PUBLISH,0}},
      {{ORD_COPY_OLD,ORD_COPY_FENCE,ORD_WRITE,ORD_REPORT_FENCE,ORD_COPY_NEW,ORD_PUBLISH,0},{ORD_WRITE,ORD_REPORT_FENCE,ORD_COPY_OLD,ORD_COPY_NEW,ORD_PUBLISH,0}},
      {{ORD_COPY_OLD,ORD_PUBLISH,0},{ORD_EARLY,ORD_GATE,ORD_COPY_OLD,0}}
    };
    return plans[claim][ORD_MUTANT==6 && claim==ORD_A2?1:control];
}
#endif
