#include "mic_session.h"
#include <algorithm>
namespace mic_sample {
const char *stage_name(Stage s) {
    static const char *names[]={"callback","mic_module","avconf_module","init","open","start"};
    return names[unsigned(s)];
}
Result run(const Ops &o,const Config &c,Stats &s) {
    s={};
    if(c.device < -1 || c.device>=8 || (c.generic&&c.device<0) ||
       (!c.generic&&c.channels!=4) || (c.channels!=1&&c.channels!=2&&c.channels!=4)) {
        s.reason="configuration";return Result::refused;
    }
    const auto start=o.now(o.context);
    auto time=start;
    bool owned[6]{};
    Result result=Result::failed;
    auto error=[&](const char *reason,int rc) {
        if(!s.first_error){s.first_error=rc?rc:-1;s.reason=reason;}
        result=Result::failed;
    };
    auto checkpoint=[&](uint64_t deadline,const char *reason,Result expiry) {
        const int p=o.pump(o.context);
        if(p<0){error("callback",p);return false;}
        const auto n=o.now(o.context);
        if(n<time){error("clock",-1);return false;}
        time=n;
        if(p){result=Result::quit;s.reason="user_exit";return false;}
        if(time>=deadline){result=expiry;s.reason=reason;return false;}
        return true;
    };
    auto acquire=[&](Stage st) {
        const int rc=o.acquire(o.context,st,s.device,c.channels);
        if(rc==0){owned[unsigned(st)]=true;return true;}
        if(rc==1&&(st==Stage::mic_module||st==Stage::avconf_module))return true;
        error(stage_name(st),rc);return false;
    };
    struct Report {const Ops *o;Stats *s;};
    Report report{&o,&s};
    const auto emit=[](void *p,const Window &w) {
        auto &r=*static_cast<Report*>(p);r.s->windows++;
        if(r.o->emit)r.o->emit(r.o->context,w);
    };
    auto body=[&]() {
        const uint64_t discover_end=start+10000000;
        for(Stage st:{Stage::callback,Stage::mic_module,Stage::avconf_module,Stage::init}) {
            if(!acquire(st)||!checkpoint(discover_end,"discovery_timeout",Result::refused))return;
        }
        int rc=o.inventory(o.context);
        if(rc){error("inventory",rc);return;}
        for(;;) {
            if(!checkpoint(discover_end,"no_matching_device",Result::refused))return;
            int selected=-1;
            for(int d=0;d<8;d++) {
                if(c.device>=0&&d!=c.device)continue;
                rc=o.attached(o.context,d);
                if(rc<0){error("attached",rc);return;}
                if(!checkpoint(discover_end,"discovery_timeout",Result::refused))return;
                if(!rc)continue;
                int type=-1;rc=o.type(o.context,d,&type);
                if(rc){error("type",rc);return;}
                if(!checkpoint(discover_end,"discovery_timeout",Result::refused))return;
                if(type!=(c.generic?3:2))continue;
                if(selected>=0){result=Result::refused;s.reason="ambiguous_device";return;}
                selected=d;
            }
            if(selected>=0){s.device=selected;break;}
            o.wait(o.context,std::min<uint64_t>(10000,discover_end-time));
        }
        if(!acquire(Stage::open)||!checkpoint(discover_end,"open_timeout",Result::refused))return;
        Format f{};rc=o.format(o.context,s.device,&f);
        if(rc){error("format",rc);return;}
        if(!checkpoint(discover_end,"format_timeout",Result::refused))return;
        Meter meter(f);
        if(!meter.valid()||f.channels!=c.channels){result=Result::refused;s.reason="unsupported_format";return;}
        if(!acquire(Stage::start)||!checkpoint(discover_end,"start_timeout",Result::refused))return;
        const auto end=time+30000000;
        auto last_data=time;
        auto progress=[&]() {
            const auto stall_end=last_data+2000000;
            const bool duration=end<=stall_end;
            return checkpoint(std::min(end,stall_end),duration?"capture_deadline":"no_data",
                              duration&&s.frames?Result::complete:Result::refused);
        };
        alignas(128) uint8_t buffer[16384];
        for(;;) {
            if(!progress())break;
            rc=o.attached(o.context,s.device);
            if(rc<0){error("attached",rc);break;}
            if(!progress())break;
            if(!rc){result=Result::refused;s.reason="detached";break;}
            int type=-1;rc=o.type(o.context,s.device,&type);
            if(rc){error("type",rc);break;}
            if(!progress())break;
            if(type!=(c.generic?3:2)){result=Result::refused;s.reason="device_changed";break;}
            rc=o.read(o.context,s.device,buffer,sizeof(buffer));
            if(rc<0){error("read",rc);break;}
            if(unsigned(rc)>sizeof(buffer)){error("read_length",-1);break;}
            if(!progress())break;
            if(rc) {
                last_data=time;
                s.bytes+=unsigned(rc);
                if(!meter.push(buffer,unsigned(rc),emit,&report)){error("pcm",-1);break;}
                s.frames=meter.frames();
            }
            if(!progress())break;
            const auto stall_end=last_data+2000000;
            if(time>=stall_end){result=Result::refused;s.reason="no_data";break;}
            o.wait(o.context,std::min<uint64_t>(10000,std::min(end-time,stall_end-time)));
        }
        if(result==Result::complete&&!meter.finish(emit,&report))error("partial_frame",-1);
    };
    body();
    bool live=false;
    for(int i=5;i>=0;i--) {
        if(!owned[i])continue;
        const Stage st=Stage(i);
        if(live&&(st==Stage::open||st==Stage::init||st==Stage::mic_module)) {
            s.retained|=1u<<i;continue;
        }
        const int rc=o.release(o.context,st,s.device);
        if(rc) {
            s.cleanup_failures++;s.retained|=1u<<i;error("cleanup",rc);
            if(st==Stage::start||st==Stage::open||st==Stage::init)live=true;
        }
    }
    const auto finish=o.now(o.context);
    if(finish<time)error("clock",-1);
    s.elapsed_us=finish>=start?finish-start:0;
    return result;
}
}
