#include "mic_meter.h"
namespace mic_sample {
static uint32_t root(uint64_t n) {
    uint32_t lo=0, hi=32769;
    while (lo+1<hi) {
        const uint32_t m=lo+(hi-lo)/2;
        if(uint64_t(m)*m<=n)lo=m;else hi=m;
    }
    return lo;
}
bool Meter::valid() const {
    return (format_.channels==1||format_.channels==2||format_.channels==4)&&
        format_.subframe==2&&format_.bits==16&&format_.endian<=1&&format_.rate==48000;
}
void Meter::flush(Emit emit,void *context) {
    if(!window_.frames)return;
    for(unsigned c=0;c<format_.channels;c++)window_.rms[c]=root(squares_[c]/window_.frames);
    if(emit)emit(context,window_);
    window_={};for(auto &s:squares_)s=0;
}
void Meter::frame(Emit emit,void *context) {
    for(unsigned c=0;c<format_.channels;c++) {
        const unsigned a=carry_[2*c],b=carry_[2*c+1];
        const unsigned word=format_.endian?(a<<8)|b:(b<<8)|a;
        const int32_t signed_sample=word>=32768?int32_t(word)-65536:int32_t(word);
        const uint32_t mag=unsigned(signed_sample<0?-signed_sample:signed_sample);
        if(mag>window_.peak[c])window_.peak[c]=mag;
        squares_[c]+=uint64_t(mag)*mag;
        if(signed_sample==-32768||signed_sample==32767)window_.clipped[c]++;
    }
    frames_++;window_.frames++;
    if(window_.frames==4800)flush(emit,context);
}
bool Meter::push(const uint8_t *bytes,size_t size,Emit emit,void *context) {
    if(!valid()||(!bytes&&size))return false;
    const size_t width=2*format_.channels;
    for(size_t i=0;i<size;i++) {
        carry_[pending_++]=bytes[i];
        if(pending_==width) {frame(emit,context);pending_=0;}
    }
    return true;
}
bool Meter::finish(Emit emit,void *context) {
    if(!valid()||pending_)return false;
    flush(emit,context);return true;
}
}
