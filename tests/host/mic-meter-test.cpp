#include "../../samples/audio/hello-ppu-mic/source/mic_meter.h"
#include <cstdio>
#include <vector>
using namespace mic_sample;
static unsigned pass = 0, fail = 0;
static void test(const char *name, bool ok) { std::printf("%s %s\n", ok ? "ok" : "FAIL", name); (ok ? pass : fail)++; }
static void emit(void *c, const Window &w) { static_cast<std::vector<Window> *>(c)->push_back(w); }
int main() {
    const Format be{4,2,16,1,48000}, le{1,2,16,0,48000};
    { Meter m(be); test("four-channel PCM16 accepted", m.valid()); }
    for (Format f : {Format{3,2,16,1,48000}, Format{4,4,16,1,48000}, Format{4,2,24,1,48000},
                     Format{4,2,16,2,48000}, Format{4,2,16,1,44100}}) {
        Meter m(f); test("unsupported format rejected", !m.valid() && !m.push(nullptr,0,nullptr,nullptr));
    }
    const uint8_t raw[]{0xff,0x80,0x00,0x7f,0xff,0x00,0x03,0xff,0xfc}; // unaligned: -32768,32767,3,-4
    { Meter m(be); std::vector<Window> w;
      bool ok=m.push(raw+1,8,emit,&w)&&m.finish(emit,&w);
      test("unaligned BE channels and signed extrema", ok&&w.size()==1&&w[0].frames==1&&w[0].peak[0]==32768&&w[0].rms[1]==32767&&w[0].rms[2]==3&&w[0].rms[3]==4&&w[0].clipped[0]==1&&w[0].clipped[1]==1); }
    { Meter m(le); std::vector<Window> w; const uint8_t b[]{3,0,0xfc,0xff};
      bool ok=m.push(b,4,emit,&w)&&m.finish(emit,&w);
      test("LE RMS uses mean squares with integer floor",ok&&w.size()==1&&w[0].frames==2&&w[0].peak[0]==4&&w[0].rms[0]==3); }
    { Meter m(le); std::vector<Window> w; const uint8_t b[]{0,0,100,0};
      bool ok=m.push(b,4,emit,&w)&&m.finish(emit,&w);
      test("RMS is not mean absolute amplitude",ok&&w.size()==1&&w[0].rms[0]==70&&w[0].peak[0]==100); }
    { Meter m(be); std::vector<Window> w; bool ok=true;
      for(unsigned i=1;i<9;i++) ok=m.push(raw+i,1,emit,&w)&&ok;
      ok=m.finish(emit,&w)&&ok;
      test("split bytes preserve frame and channel order",ok&&m.frames()==1&&m.pending()==0&&w.size()==1&&w[0].peak[3]==4&&w[0].peak[0]==32768); }
    { Meter m(be); std::vector<Window> w; bool ok=m.push(raw+1,7,emit,&w);
      test("partial frame never counted or silently completed",ok&&m.frames()==0&&m.pending()==7&&!m.finish(emit,&w)&&w.empty()); }
    { Meter m(le); std::vector<Window> w; std::vector<uint8_t>b(9600,0); bool ok=m.push(b.data(),b.size(),emit,&w);
      test("silence emits one 100ms window",ok&&m.frames()==4800&&w.size()==1&&w[0].rms[0]==0&&w[0].frames==4800&&w[0].clipped[0]==0); }
    { Meter m(le); std::vector<Window> w; std::vector<uint8_t>b(19200); for(size_t i=1;i<b.size();i+=2)b[i]=0x80;
      bool ok=m.push(b.data(),b.size(),emit,&w)&&m.finish(emit,&w);
      test("full-scale squares use 64 bits and windows reset",ok&&w.size()==2&&w[0].rms[0]==32768&&w[1].rms[0]==32768&&w[1].clipped[0]==4800&&m.frames()==9600); }
    { Meter m(le); test("null nonempty input rejected", !m.push(nullptr,1,nullptr,nullptr)); }
    { Meter m(le); std::vector<Window>w;test("empty input does not emit",m.push(nullptr,0,emit,&w)&&m.finish(emit,&w)&&w.empty()); }
    std::printf("mic-meter: %u passed, %u failed\n",pass,fail);return fail?1:0;
}
