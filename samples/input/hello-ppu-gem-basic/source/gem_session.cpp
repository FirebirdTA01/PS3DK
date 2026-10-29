#include "gem_session.h"
#include <algorithm>
#include <cmath>

namespace gem_basic {
const char *stage_name(Stage s) {
    static const char *names[]={"callback","load_spurs","load_gem","spu_init",
        "allocate","spurs_init","gem_init","info","read","pump","rumble",
        "colour","clock","connect_timeout","stale","invalid_sample","gem_end",
        "spurs_end","free_memory","unload_gem","unload_spurs","unregister_callback"};
    return unsigned(s)<sizeof(names)/sizeof(*names) ? names[unsigned(s)] : "unknown";
}

Result run(const Ops &o, Stats &s) {
    s={};
    if(!o.control || !o.connected || !o.read || !o.rumble || !o.colour ||
       !o.pump || !o.now || !o.wait) {
        s.first_error=-1; return Result::failed;
    }
    bool owned[7]{};
    auto error=[&](Stage stage,int rc,Result result=Result::failed) {
        if(!s.first_error) {s.first_error=rc; s.error_stage=stage;}
        return result;
    };
    auto body=[&]() -> Result {
        for(unsigned i=0;i<7;++i) {
            const auto stage=static_cast<Stage>(i);
            const int rc=o.control(o.context,stage);
            if(rc) return error(stage,rc);
            owned[i]=true;
        }
        std::uint64_t current=o.now(o.context), previous=current, begin=current;
        auto tick=[&]() {
            current=o.now(o.context);
            if(current<previous) return false;
            previous=current; return true;
        };
        for(;;) {
            if(!tick()) return error(Stage::clock,-1);
            int rc=o.pump(o.context);
            if(rc==1) return Result::quit;
            if(rc) return error(Stage::pump,rc);
            bool ready=false;
            rc=o.connected(o.context,&ready);
            if(rc) return error(Stage::info,rc);
            if(ready) break;
            if(current-begin>=connect_us) return error(Stage::connect_timeout,-1,Result::refused);
            o.wait(o.context,10000);
        }
        int rc=o.colour(o.context,1);
        if(rc) return error(Stage::colour,rc);
        begin=current;
        std::uint64_t last_fresh=current, motor_start=0;
        bool motor=false, have_sample=false;
        Sample last{};
        unsigned colour=1;
        // Stop REQUEST, not a physical motor-stop guarantee. Service immediately
        // after API/logging returns as well as each polling wakeup.
        auto service_motor=[&]() {
            if(!tick()) {error(Stage::clock,-1);return false;}
            if(motor && current-motor_start>=pulse_us) {
                rc=o.rumble(o.context,0);
                if(rc) {error(Stage::rumble,rc);return false;}
                motor=false;
            }
            return true;
        };
        for(;;) {
            if(!service_motor()) return Result::failed;
            s.elapsed_us=current-begin;
            rc=o.pump(o.context);
            if(rc==1) return Result::quit;
            if(rc) return error(Stage::pump,rc);
            if(!service_motor()) return Result::failed;
            if(s.elapsed_us>=session_us) return Result::complete;
            bool ready=false;
            rc=o.connected(o.context,&ready);
            if(rc) return error(Stage::info,rc);
            if(!service_motor()) return Result::failed;
            if(!ready) return error(Stage::info,-1,Result::refused);
            Sample sample{};
            rc=o.read(o.context,&sample);
            if(rc) return error(Stage::read,rc);
            if(!service_motor()) return Result::failed;
            bool valid=sample.trigger<=255;
            for(unsigned i=0;i<3;++i)
                valid &= std::isfinite(sample.accel[i]) && std::isfinite(sample.gyro[i]);
            const std::uint32_t advance=sample.counter-last.counter;
            if(have_sample && (sample.timestamp<last.timestamp || advance>=UINT32_C(0x80000000))) valid=false;
            if(!valid) return error(Stage::invalid_sample,-1,Result::refused);
            const bool fresh=!have_sample || (sample.timestamp>last.timestamp && advance!=0);
            if(fresh) {
                const bool changed=have_sample && (sample.buttons!=last.buttons || sample.trigger!=last.trigger);
                const auto pressed=std::uint16_t(sample.buttons & ~last.buttons);
                ++s.fresh;
                if(changed) ++s.input_changes;
                if(o.observe) o.observe(o.context,sample,changed);
                if(!service_motor()) return Result::failed;
                if(sample.buttons & start_button) return Result::quit;
                if(motor && !(sample.buttons & move_button)) {
                    rc=o.rumble(o.context,0);
                    if(rc) return error(Stage::rumble,rc);
                    motor=false;
                }
                if((pressed & move_button) && !motor) {
                    motor=true; motor_start=current;
                    rc=o.rumble(o.context,96);
                    if(rc) return error(Stage::rumble,rc);
                    if(!service_motor()) return Result::failed;
                }
                if(pressed & cross_button) {
                    colour=colour%3+1;
                    rc=o.colour(o.context,colour);
                    if(rc) return error(Stage::colour,rc);
                    if(!service_motor()) return Result::failed;
                }
                last=sample; have_sample=true; last_fresh=current;
            } else ++s.duplicates;
            if(current-last_fresh>=stale_us) return error(Stage::stale,-1,Result::refused);
            const auto delay=motor ? std::min<std::uint64_t>(10000,pulse_us-(current-motor_start)) : 10000;
            o.wait(o.context,delay);
        }
    };
    Result result=body();
    auto cleaned=[&](Stage stage,int rc) {
        if(rc) {++s.cleanup_failures; error(stage,rc); result=Result::failed;}
        return rc==0;
    };
    auto cleanup=[&](Stage stage) {return cleaned(stage,o.control(o.context,stage));};
    bool gem_live=owned[unsigned(Stage::gem_init)];
    bool spurs_live=owned[unsigned(Stage::spurs_init)];
    if(gem_live) {
        cleaned(Stage::rumble,o.rumble(o.context,0));
        cleaned(Stage::colour,o.colour(o.context,0));
        if(cleanup(Stage::gem_end)) gem_live=false;
    }
    if(spurs_live && !gem_live && cleanup(Stage::spurs_end)) spurs_live=false;
    if(owned[unsigned(Stage::allocate)] && !gem_live && !spurs_live) cleanup(Stage::free_memory);
    if(owned[unsigned(Stage::load_gem)] && !gem_live) cleanup(Stage::unload_gem);
    if(owned[unsigned(Stage::load_spurs)] && !gem_live && !spurs_live) cleanup(Stage::unload_spurs);
    if(owned[unsigned(Stage::callback)]) cleanup(Stage::unregister_callback);
    s.retained_resources=gem_live || spurs_live;
    return result;
}
}
