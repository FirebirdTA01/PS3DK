#ifndef REPORT_COPY_B0_LOGIC_H
#define REPORT_COPY_B0_LOGIC_H
#include <stdint.h>
#include <stddef.h>
#ifndef B0_MUTANT
#define B0_MUTANT 0
#endif
#define B0_REPORT_INDEX 516u
#define B0_BYTES 16u
/* Experimental offsets, not an established hardware source mapping. */
static inline uint32_t b0_offset(unsigned candidate) {
    return B0_REPORT_INDEX*16u + (candidate==0 ? 0x1400u:0u);
}
/* Subchannel1 M2MF: DMA_BUFFER_IN/OUT, then OFFSET_IN through BUFFER_NOTIFY.
 * No generic transfer wrapper: it would replace the experimental input object. */
static inline void b0_packet(uint32_t *p,uint32_t destination,uint32_t source) {
    p[0]=(2u<<18)|(1u<<13)|0x184u;
    p[1]=B0_MUTANT==5 ? 0xfeed0000u:0x66626660u;
    p[2]=0xfeed0001u;
    p[3]=(8u<<18)|(1u<<13)|0x30cu;
    p[4]=source;p[5]=destination;
    p[6]=16;p[7]=16;p[8]=16;p[9]=1;p[10]=0x101;p[11]=0;
}
static inline int b0_equal(const unsigned char *a,const unsigned char *b) {
    if(B0_MUTANT==2 && a[0]==0xa5) return 1;
    for(unsigned i=0;i<(B0_MUTANT==1 ? 15u:16u);i++) if(a[i]!=b[i]) return 0;
    return 1;
}
static inline int b0_guards(const volatile uint32_t *p,size_t bytes,size_t dest,uint32_t sentinel) {
    if(B0_MUTANT==3) return 1;
    for(size_t i=0;i<bytes/4;i++) {
        if(i*4>=dest && i*4<dest+16) continue;
        if(p[i]!=sentinel) return 0;
    }
    return 1;
}
enum { B0_STOP, B0_NEXT, B0_MATCH };
static inline int b0_decide(int idle,int guards,int stable,int equal) {
    if((!idle && B0_MUTANT!=4) || !guards || !stable) return B0_STOP;
    return equal ? B0_MATCH:B0_NEXT;
}
#endif
