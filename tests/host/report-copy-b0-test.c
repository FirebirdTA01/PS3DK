#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "../../samples/gcm/hello-ppu-gcm-report-copy-b0/source/b0_logic.h"
#define CHECK(c, n) do { if (!(c)) {puts("ASSERTION: " n); return 1;} } while (0)
int main(void) {
    uint32_t words[14];
    for (unsigned i=0;i<14;i++) words[i]=0xcafef00d;
    b0_packet(words+1, 0x12340080, b0_offset(0));
    const uint32_t expected[] = {0x00082184,0x66626660,0xfeed0001,
        0x0020230c,0x3440,0x12340080,16,16,16,1,0x101,0};
    CHECK(!memcmp(words+1,expected,sizeof expected), "exact_packet");
    CHECK(words[0]==0xcafef00d && words[13]==0xcafef00d,"packet_bounds");
    CHECK(b0_offset(1)==0x2040,"candidate2_relative");
    unsigned char expected_bytes[16], got[16];
    for (unsigned i=0;i<16;i++) expected_bytes[i]=(unsigned char)(i+1);
    memcpy(got,expected_bytes,16);
    CHECK(b0_equal(got,expected_bytes),"exact_match");
    got[15]^=1;
    CHECK(!b0_equal(got,expected_bytes),"last_byte_mismatch");
    memset(got,0xa5,16);
    CHECK(!b0_equal(got,expected_bytes),"omitted_copy_caught");
    uint32_t arena[64]; for(unsigned i=0;i<64;i++) arena[i]=0xa5a51234;
    for(unsigned i=32;i<36;i++) arena[i]=i;
    CHECK(b0_guards(arena,sizeof arena,128,0xa5a51234),"valid_guards");
    arena[36]=0;
    CHECK(!b0_guards(arena,sizeof arena,128,0xa5a51234),"trailing_guard");
    arena[36]=0xa5a51234;arena[0]=0;
    CHECK(!b0_guards(arena,sizeof arena,128,0xa5a51234),"distant_guard");
    CHECK(b0_decide(1,1,1,0)==B0_NEXT,"mismatch_advances");
    CHECK(b0_decide(1,1,1,1)==B0_MATCH,"match_stops");
    CHECK(b0_decide(0,1,1,0)==B0_STOP,"timeout_stops");
    CHECK(b0_decide(1,0,1,1)==B0_STOP,"corruption_stops");
    CHECK(b0_decide(1,1,0,1)==B0_STOP,"changed_reference_stops");
    puts("B0 packet, byte equality, guards and stop policy PASS");
    return 0;
}
