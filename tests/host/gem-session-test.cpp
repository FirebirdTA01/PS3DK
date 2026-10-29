#include "../../samples/input/hello-ppu-gem-basic/source/gem_session.h"
#include <algorithm>
#include <cstdio>
#include <limits>
#include <utility>
#include <vector>
using namespace gem_basic;
struct Fake {
    std::uint64_t time=0;
    unsigned reads=0;
    int fail_stage=-1, cleanup_fail=-1, read_error=0, pump_error=0;
    unsigned read_error_at=0, pump_error_at=0, motor_on_error=0, zero_error_number=0, zero_calls=0;
    std::uint64_t read_delay=0, observe_delay=0;
    std::uint64_t wait_extra=0, held_from_us=0;
    bool ended_without_zero=false;
    bool cross_cycles=false, move_cycles=false;
    bool absent=false, disconnect=false, stale=false, held=false, release=false,
         bad_trigger=false, nan=false, wrap=false, backwards=false, repeat_time=false,
         quit_connect=false, quit_read=false, clock_back=false;
    std::vector<Stage> stages;
    std::vector<std::pair<std::uint64_t,unsigned>> motors;
    std::vector<unsigned> colours;
    Ops ops() {
        return {this,
            [](void *p,Stage s){ auto &f=*static_cast<Fake*>(p); f.stages.push_back(s);
                if(s==Stage::gem_end) f.ended_without_zero=f.motors.empty() || f.motors.back().second!=0;
                return int(s)==f.fail_stage ? 71 : int(s)==f.cleanup_fail ? 72 : 0; },
            [](void *p,bool *c){auto &f=*static_cast<Fake*>(p);
                *c=!f.absent && !(f.disconnect && f.time>=100000); return 0;},
            [](void *p,Sample *s){auto &f=*static_cast<Fake*>(p); ++f.reads;
                *s={}; s->timestamp=f.stale ? 1 : f.time+1;
                if(f.repeat_time) s->timestamp=1;
                s->counter=f.stale ? 1 : f.wrap ? UINT32_MAX-2+f.reads : f.reads;
                if(f.backwards && f.reads>2) s->timestamp=0;
                s->accel[2]=f.nan ? std::numeric_limits<float>::quiet_NaN() : 1;
                s->trigger=f.bad_trigger ? 256 : 123;
                if(f.held && f.time>=f.held_from_us && (!f.release || f.time<50000)) s->buttons=move_button;
                if(f.time<500000 && (f.time/100000)%2==0) {
                    if(f.cross_cycles) s->buttons|=cross_button;
                    if(f.move_cycles) s->buttons|=move_button;
                }
                if(f.quit_read && f.reads==3) s->buttons=start_button;
                if(f.reads==2) f.time+=f.read_delay;
                return f.read_error_at && f.reads<f.read_error_at ? 0 : f.read_error;},
            [](void *p,std::uint8_t n){auto &f=*static_cast<Fake*>(p);
                f.motors.emplace_back(f.time,n);
                if(n && f.motor_on_error) return int(f.motor_on_error);
                if(!n && ++f.zero_calls==f.zero_error_number) return 73;
                return int(Stage::rumble)==f.cleanup_fail ? 72 : 0;},
            [](void *p,unsigned n){auto &f=*static_cast<Fake*>(p);f.colours.push_back(n);
                return n==0 && int(Stage::colour)==f.cleanup_fail ? 72 : 0;},
            [](void *p){auto &f=*static_cast<Fake*>(p);
                if(f.pump_error_at && f.reads<f.pump_error_at) return 0;
                return f.pump_error ? f.pump_error : f.quit_connect ? 1 : 0;},
            [](void *p){return static_cast<Fake*>(p)->time;},
            [](void *p,std::uint64_t delay){auto &f=*static_cast<Fake*>(p); f.time+=delay+f.wait_extra;f.wait_extra=0;
                if(f.clock_back && f.time==20000) f.time=0;},
            [](void *p,const Sample &,bool){auto &f=*static_cast<Fake*>(p);if(f.reads==2) f.time+=f.observe_delay;}};
    }
    bool saw(Stage s) const {return std::find(stages.begin(),stages.end(),s)!=stages.end();}
    unsigned pulses() const {return unsigned(std::count_if(motors.begin(),motors.end(),[](auto x){return x.second==96;}));}
};
static unsigned passed=0, failed=0;
static void check(bool b,const char *name){std::printf("%s %s\n",b?"PASS":"FAIL",name);b?++passed:++failed;}
static bool released(const Fake &f){return !f.ended_without_zero&&f.saw(Stage::gem_end)&&f.saw(Stage::spurs_end)&&f.saw(Stage::free_memory)&&f.saw(Stage::unregister_callback);}
int main(){
    {Fake f;Stats s;check(run(f.ops(),s)==Result::complete && s.fresh>100 && s.elapsed_us==session_us && released(f),"complete bounded session");}
    {Fake f;f.absent=true;Stats s;check(run(f.ops(),s)==Result::refused&&f.time==connect_us&&released(f),"bounded connection refusal");}
    {Fake f;f.stale=true;Stats s;check(run(f.ops(),s)==Result::refused&&f.time==stale_us&&s.fresh==1&&s.duplicates>0&&released(f),"stale data refusal");}
    {Fake f;f.held=true;Stats s;run(f.ops(),s);check(f.pulses()==1&&f.motors.size()>=2&&f.motors[1]==std::make_pair(pulse_us,0u),"held button pulse expires once");}
    {Fake f;f.held=true;f.release=true;Stats s;run(f.ops(),s);check(f.pulses()==1&&f.motors[1]==std::make_pair(UINT64_C(50000),0u),"release stops rumble");}
    {Fake f;f.held=true;f.disconnect=true;Stats s;check(run(f.ops(),s)==Result::refused&&f.motors.back().second==0&&f.time==100000&&released(f),"disconnect stops motor and cleans");}
    {Fake f;f.held=true;f.stale=true;Stats s;run(f.ops(),s);check(f.motors[1]==std::make_pair(pulse_us,0u),"rumble expires without fresh sensor");}
    {Fake f;f.wrap=true;Stats s;check(run(f.ops(),s)==Result::complete&&s.fresh>100,"counter rollover valid");}
    {Fake f;f.backwards=true;Stats s;check(run(f.ops(),s)==Result::refused&&s.error_stage==Stage::invalid_sample,"timestamp reversal rejected");}
    {Fake f;f.repeat_time=true;Stats s;check(run(f.ops(),s)==Result::refused&&s.fresh==1,"counter alone not freshness");}
    {Fake f;f.bad_trigger=true;Stats s;check(run(f.ops(),s)==Result::refused&&s.error_stage==Stage::invalid_sample,"out of range trigger refused");}
    {Fake f;f.nan=true;Stats s;check(run(f.ops(),s)==Result::refused&&s.fresh==0,"nonfinite IMU refused");}
    {Fake f;f.quit_connect=true;f.absent=true;Stats s;check(run(f.ops(),s)==Result::quit&&released(f)&&f.reads==0,"quit during connect");}
    {Fake f;f.quit_read=true;f.held=true;Stats s;check(run(f.ops(),s)==Result::quit&&f.motors.back().second==0&&released(f),"Start exits and stops rumble");}
    {Fake f;f.clock_back=true;Stats s;check(run(f.ops(),s)==Result::failed&&s.error_stage==Stage::clock,"clock reversal bounded failure");}
    {Fake f;f.read_error=6;Stats s;check(run(f.ops(),s)==Result::failed&&s.fresh==0&&s.first_error==6,"positive read status never consumed");}
    {Fake f;f.pump_error=-77;Stats s;check(run(f.ops(),s)==Result::failed&&s.first_error==-77&&released(f),"callback error cleans");}
    for(int i=0;i<=int(Stage::gem_init);++i){Fake f;f.fail_stage=i;Stats s;
        auto r=run(f.ops(),s);bool good=r==Result::failed&&s.first_error==71;
        good &= !f.saw(Stage::gem_end);
        good &= f.saw(Stage::spurs_end)==(i>int(Stage::spurs_init));
        good &= f.saw(Stage::free_memory)==(i>int(Stage::allocate));
        good &= f.saw(Stage::unregister_callback)==(i>int(Stage::callback));
        check(good,stage_name(static_cast<Stage>(i)));}
    for(auto stage:{Stage::gem_end,Stage::spurs_end,Stage::free_memory,Stage::unload_gem,Stage::unload_spurs,Stage::unregister_callback,Stage::colour}){
        Fake f;f.cleanup_fail=int(stage);Stats s;auto r=run(f.ops(),s);
        bool good=r==Result::failed&&s.first_error==72&&s.cleanup_failures==1;
        if(stage==Stage::gem_end) good &= !f.saw(Stage::spurs_end)&&!f.saw(Stage::free_memory)&&!f.saw(Stage::unload_gem)&&!f.saw(Stage::unload_spurs)&&s.retained_resources;
        if(stage==Stage::spurs_end) good &= !f.saw(Stage::free_memory)&&!f.saw(Stage::unload_spurs)&&s.retained_resources;
        check(good,stage_name(stage));
    }
    {Fake f;f.read_error=-99;f.cleanup_fail=int(Stage::gem_end);Stats s;run(f.ops(),s);check(s.first_error==-99&&s.error_stage==Stage::read&&s.cleanup_failures==1,"first error survives cleanup failure");}
    {Fake f;Stats s;run(f.ops(),s);auto a=std::find(f.stages.begin(),f.stages.end(),Stage::gem_end),b=std::find(f.stages.begin(),f.stages.end(),Stage::spurs_end);check(a<b,"GEM ends before SPURS");}
    {Fake f;f.held=true;f.read_delay=1000000;f.observe_delay=500000;Stats s;run(f.ops(),s);
        check(f.motors[1]==std::make_pair(UINT64_C(1010000),0u),"stop at first slow read return before logging");}
    {Fake f;f.held=true;f.read_error=-91;f.read_error_at=2;Stats s;
        check(run(f.ops(),s)==Result::failed&&s.first_error==-91&&f.motors.back().second==0&&released(f),"read error after activation stops before teardown");}
    {Fake f;f.held=true;f.pump_error=-92;f.pump_error_at=2;Stats s;
        check(run(f.ops(),s)==Result::failed&&s.first_error==-92&&f.motors.back().second==0&&released(f),"callback error after activation stops");}
    {Fake f;f.held=true;f.pump_error=1;f.pump_error_at=2;Stats s;
        check(run(f.ops(),s)==Result::quit&&f.motors.back().second==0&&released(f),"callback quit after activation stops");}
    {Fake f;f.held=true;f.motor_on_error=74;Stats s;
        check(run(f.ops(),s)==Result::failed&&s.first_error==74&&f.motors.back().second==0&&released(f),"failed start still attempts zero");}
    {Fake f;f.held=true;f.zero_error_number=1;Stats s;
        check(run(f.ops(),s)==Result::failed&&s.first_error==73&&f.zero_calls==2&&released(f),"deadline zero failure retried in cleanup");}
    {Fake f;f.held=true;f.read_error=-93;f.read_error_at=2;f.zero_error_number=1;Stats s;
        check(run(f.ops(),s)==Result::failed&&s.first_error==-93&&s.cleanup_failures==1&&released(f),"cleanup zero failure retains first read error");}
    {Fake f;f.cleanup_fail=int(Stage::rumble);Stats s;
        check(run(f.ops(),s)==Result::failed&&s.first_error==72&&s.cleanup_failures==1&&released(f),"cleanup zero failure after idle session");}
    {Fake f;f.held=true;f.wait_extra=400000;Stats s;run(f.ops(),s);
        check(f.motors[1]==std::make_pair(UINT64_C(410000),0u),"oversleep stops at first scheduling opportunity");}
    {Fake f;f.held=true;f.read_delay=235000;Stats s;run(f.ops(),s);
        check(f.motors[1]==std::make_pair(pulse_us,0u),"sleep capped to remaining pulse deadline");}
    {Fake f;f.held=true;f.held_from_us=session_us-10000;Stats s;
        check(run(f.ops(),s)==Result::complete&&f.pulses()==1&&f.motors.back()==std::make_pair(session_us,0u)&&released(f),"session timeout stops active pulse before teardown");}
    {Fake f;f.cross_cycles=true;Stats s;run(f.ops(),s);
        check(f.colours==std::vector<unsigned>({1,2,3,1,0}),"Cross edges cycle colour once per press");}
    {Fake f;f.move_cycles=true;Stats s;run(f.ops(),s);
        check(f.pulses()==3&&f.motors.back().second==0&&released(f),"release and re-press rearm pulse");}
    std::printf("gem-session: %u tests, %u pass, %u fail\n",passed+failed,passed,failed);
    return failed?1:0;
}
