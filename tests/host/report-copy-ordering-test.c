#include <stdio.h>
#include <string.h>
#include "../../samples/gcm/hello-ppu-gcm-report-copy-ordering/source/ordering_logic.h"
#define CHECK(x,n) do { if (!(x)) { puts("ASSERTION: " n); return 1; } } while(0)
int main(void) {
    uint32_t p[14]; for(unsigned i=0;i<14;i++)p[i]=0xdeadbeef;
    ord_packet(p+1,128,516,4032);
    const uint32_t want[]={0x82184,0x66626660,0xfeed0001,0x20230c,8256,128,4032,4032,4032,1,0x101,0};
    CHECK(!memcmp(p+1,want,sizeof want)&&p[0]==0xdeadbeef&&p[13]==0xdeadbeef,"packet");
    unsigned char a[32]={0},b[32]={0},early[32]={0};
    a[31]=1; CHECK(!ord_equal(a,b,32),"last_byte"); b[31]=1;
    CHECK(ord_evidence(ORD_B2,a,b,early,32)==1,"frozen_early");
    CHECK(ord_evidence(ORD_A1,a,b,early,32)==0,"settled_a1");
    uint32_t arena[80];for(unsigned i=0;i<80;i++)arena[i]=123;
    arena[32]=77; CHECK(ord_guards(arena,sizeof arena,128,32,256,0,123),"allowed_destination");
    arena[79]=0;CHECK(!ord_guards(arena,sizeof arena,128,32,256,0,123),"distant_guard");
    CHECK(ord_result(8,0,0)==ORD_INCONCLUSIVE,"control_required");
    CHECK(ord_result(8,0,1)==ORD_SUPPORTED&&ord_result(7,0,1)==ORD_INCONCLUSIVE,"count_required");
    CHECK(ord_result(8,1,1)==ORD_REFUTED,"candidate_mismatch");
    unsigned char times[64]={0}; uint64_t t=10;memcpy(times,&t,8);t=20;memcpy(times+16,&t,8);
    t=21;memcpy(times+32,&t,8);t=30;memcpy(times+48,&t,8);
    CHECK(ord_times(times,2,9),"ordered_generation");
    CHECK(!ord_times(times,2,10),"fresh_generation");
    t=19;memcpy(times+32,&t,8);CHECK(!ord_times(times,2,9),"overlapping_pairs");
    const unsigned char expected[3][2][8]={
      {{ORD_WRITE,ORD_REPORT_FENCE,ORD_COPY_OLD,ORD_PUBLISH,0},{ORD_COPY_OLD,ORD_COPY_FENCE,ORD_WRITE,ORD_REPORT_FENCE,ORD_PUBLISH,0}},
      {{ORD_COPY_OLD,ORD_COPY_FENCE,ORD_WRITE,ORD_REPORT_FENCE,ORD_COPY_NEW,ORD_PUBLISH,0},{ORD_WRITE,ORD_REPORT_FENCE,ORD_COPY_OLD,ORD_COPY_NEW,ORD_PUBLISH,0}},
      {{ORD_COPY_OLD,ORD_PUBLISH,0},{ORD_EARLY,ORD_GATE,ORD_COPY_OLD,0}}
    };
    for(unsigned c=0;c<3;c++)for(unsigned n=0;n<2;n++)CHECK(!memcmp(ord_plan(c,n),expected[c][n],8),"schedule");
    puts("ordering logic PASS");return 0;
}
