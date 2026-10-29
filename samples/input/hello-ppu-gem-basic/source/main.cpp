#include "gem_session.h"
#include <atomic>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <malloc.h>
#include <unistd.h>
#include <sys/process.h>
#include <sys/sys_time.h>
#include <sys/spu_initialize.h>
#include <sys/ppu_thread.h>
#include <cell/gem.h>
#include <cell/sysmodule.h>
#include <cell/sysutil.h>

SYS_PROCESS_PARAM(1001,0x100000);
using namespace gem_basic;
static_assert(sizeof(CellGemInertialState)==112 && alignof(CellGemInertialState)==16,
              "GEM PRX state layout");
static_assert(sizeof(CellGemAttribute)==24 && sizeof(CellGemAttribute::spurs)==4,
              "GEM PRX pointer layout");
static_assert(move_button==CELL_GEM_CTRL_MOVE && start_button==CELL_GEM_CTRL_START &&
              cross_button==CELL_GEM_CTRL_CROSS,"button mapping");
static_assert(std::atomic<unsigned>::is_always_lock_free,"callback flag must be lock free");
static std::atomic<unsigned> quitting{0};
struct Device {
    CellSpurs *spurs=nullptr;
    void *work=nullptr;
    std::uint64_t last_print=0;
    bool printed=false, exit_printed=false;
    bool pulse_requested=false;
    std::uint64_t pulse_start=0;
};
static bool prx_range(const void *p,std::size_t size) {
    const auto address=reinterpret_cast<std::uintptr_t>(p);
    return p && size && address<=UINT32_MAX && size-1<=UINT32_MAX-address;
}
static void callback(std::uint64_t status,std::uint64_t,void *) {
    if(status==CELL_SYSUTIL_REQUEST_EXITGAME) quitting.store(1,std::memory_order_release);
}
static int pump(void *p) {
    auto &d=*static_cast<Device*>(p);
    const int rc=cellSysutilCheckCallback();
    if(rc) return rc;
    if(!quitting.load(std::memory_order_acquire)) return 0;
    if(!d.exit_printed) {std::puts("GEM_EXIT_CALLBACK serviced");d.exit_printed=true;}
    return 1;
}
static int control(void *p,Stage stage) {
    auto &d=*static_cast<Device*>(p);
    int rc=0;
    switch(stage) {
    case Stage::callback: rc=cellSysutilRegisterCallback(0,callback,nullptr);break;
    case Stage::load_spurs: rc=cellSysmoduleLoadModule(CELL_SYSMODULE_SPURS);break;
    case Stage::load_gem: rc=cellSysmoduleLoadModule(CELL_SYSMODULE_GEM);break;
    case Stage::spu_init: rc=sys_spu_initialize(2,0);break;
    case Stage::allocate: {
        const int bytes=cellGemGetMemorySize(1);
        if(bytes<=0) {rc=bytes ? bytes : -1;break;}
        d.spurs=static_cast<CellSpurs*>(memalign(CELL_SPURS_ALIGN,sizeof(CellSpurs)));
        d.work=memalign(128,static_cast<std::size_t>(bytes));
        if(!prx_range(d.spurs,sizeof(CellSpurs)) || !prx_range(d.work,bytes)) {
            std::free(d.spurs);std::free(d.work);d.spurs=nullptr;d.work=nullptr;rc=-1;break;
        }
        std::memset(d.spurs,0,sizeof(CellSpurs));
        std::memset(d.work,0,static_cast<std::size_t>(bytes));
        std::printf("GEM_MEMORY spurs=%p work=%p bytes=%d state_align=%zu\n",
                    static_cast<void*>(d.spurs),d.work,bytes,alignof(CellGemInertialState));
        break;
    }
    case Stage::spurs_init: {
        sys_ppu_thread_t thread{};
        int priority=0;
        rc=sys_ppu_thread_get_id(&thread);
        if(rc) break;
        rc=sys_ppu_thread_get_priority(thread,&priority);
        if(rc) break;
        if(priority<=0) {rc=-1;break;}
        CellSpursAttribute attr{};
        rc=cellSpursAttributeInitialize(&attr,2,100,priority-1,true);
        if(rc) break;
        rc=cellSpursAttributeSetNamePrefix(&attr,"GemBasic",8);
        if(rc) break;
        rc=cellSpursInitializeWithAttribute(d.spurs,&attr);
        break;
    }
    case Stage::gem_init: {
        CellGemAttribute attr{};
        const std::uint8_t priorities[8]={8,8,0,0,0,0,0,0};
        cellGemAttributeInit(&attr,1,d.work,d.spurs,priorities);
        rc=cellGemInit(&attr);break;
    }
    case Stage::gem_end: rc=cellGemEnd();break;
    case Stage::spurs_end: rc=cellSpursFinalize(d.spurs);break;
    case Stage::free_memory:
        std::free(d.work);std::free(d.spurs);d.work=nullptr;d.spurs=nullptr;break;
    case Stage::unload_gem: rc=cellSysmoduleUnloadModule(CELL_SYSMODULE_GEM);break;
    case Stage::unload_spurs: rc=cellSysmoduleUnloadModule(CELL_SYSMODULE_SPURS);break;
    case Stage::unregister_callback: rc=cellSysutilUnregisterCallback(0);break;
    default: rc=-1;break;
    }
    std::printf("GEM_STAGE %s rc=0x%08x\n",stage_name(stage),unsigned(rc));
    return rc;
}
static int connected(void *,bool *ready) {
    CellGemInfo info{};
    const int rc=cellGemGetInfo(&info);
    if(!rc) *ready=info.max_connect>=1 && info.now_connect>=1 && info.status[0]==CELL_GEM_STATUS_READY;
    return rc;
}
static int read(void *,Sample *sample) {
    CellGemInertialState state{};
    const int rc=cellGemGetInertialState(0,CELL_GEM_INERTIAL_STATE_FLAG_LATEST,0,&state);
    if(rc) return rc;
    if(state.timestamp<0) return -1;
    sample->buttons=state.pad.digitalbuttons;sample->trigger=state.pad.analog_T;
    sample->timestamp=static_cast<std::uint64_t>(state.timestamp);
    sample->counter=static_cast<std::uint32_t>(state.counter);
    for(unsigned i=0;i<3;++i) {
        sample->accel[i]=state.accelerometer[i];sample->gyro[i]=state.gyro[i];
    }
    return 0;
}
static int rumble(void *p,std::uint8_t value) {
    auto &d=*static_cast<Device*>(p);
    const auto request_time=static_cast<std::uint64_t>(sys_time_get_system_time());
    if(value) {d.pulse_requested=true;d.pulse_start=request_time;}
    const auto elapsed=d.pulse_requested ? request_time-d.pulse_start : 0;
    int rc=cellGemSetRumble(0,value);
    unsigned char actual=0;
    if(!rc) rc=cellGemGetRumble(0,&actual);
    if(!rc && actual!=value) rc=-1;
    std::printf("GEM_RUMBLE requested=%u readback=%u rc=0x%08x stop_request_elapsed_us=%" PRIu64 " proof=API_ONLY\n",
                unsigned(value),unsigned(actual),unsigned(rc),value ? UINT64_C(0) : elapsed);
    if(!value && !rc) d.pulse_requested=false;
    return rc;
}
static int colour(void *,unsigned value) {
    const float r=value==2?1.0f:0.0f,g=value==3?1.0f:0.0f,b=value==1?1.0f:0.0f;
    int rc=cellGemForceRGB(0,r,g,b);
    float ar=0,ag=0,ab=0;
    if(!rc) rc=cellGemGetRGB(0,&ar,&ag,&ab);
    if(!rc && (ar!=r || ag!=g || ab!=b)) rc=-1;
    std::printf("GEM_COLOUR requested=%u rc=0x%08x proof=API_ONLY\n",value,unsigned(rc));
    return rc;
}
static std::uint64_t now(void *) {return static_cast<std::uint64_t>(sys_time_get_system_time());}
static void wait(void *,std::uint64_t maximum_us) {usleep(static_cast<useconds_t>(maximum_us));}
static void observe(void *p,const Sample &s,bool changed) {
    auto &d=*static_cast<Device*>(p);
    if(!d.printed || changed || s.timestamp-d.last_print>=1000000) {
        std::printf("GEM_INPUT buttons=0x%04x trigger=%u accel=(%.5f,%.5f,%.5f) gyro=(%.5f,%.5f,%.5f) timestamp=%" PRIu64 " counter=%" PRIu32 " changed=%u\n",
            unsigned(s.buttons),unsigned(s.trigger),double(s.accel[0]),double(s.accel[1]),double(s.accel[2]),
            double(s.gyro[0]),double(s.gyro[1]),double(s.gyro[2]),s.timestamp,s.counter,unsigned(changed));
        d.last_print=s.timestamp;d.printed=true;
    }
}
int main() {
    std::setvbuf(stdout,nullptr,_IONBF,0);
    std::puts("GEM_BEGIN camera=unused slot=0 connect_s=10 read_s=30");
    std::puts("GEM_CONTROLS Move=250ms_rumble Cross=colour Start=exit");
    std::puts("GEM_PROOF API/input/lifecycle only; IMU values, rumble and sphere colour require hardware verification");
    Device d;
    const Ops ops{&d,control,connected,read,rumble,colour,pump,now,wait,observe};
    Stats stats;
    const Result result=run(ops,stats);
    std::printf("GEM_SUMMARY fresh=%u duplicate=%u input_changes=%u elapsed_us=%" PRIu64 " cleanup_failures=%u retained=%u first_error=0x%08x stage=%s\n",
        stats.fresh,stats.duplicates,stats.input_changes,stats.elapsed_us,stats.cleanup_failures,
        unsigned(stats.retained_resources),unsigned(stats.first_error),stage_name(stats.error_stage));
    const char *marker=result==Result::complete ? "GEM_API_OK" : result==Result::quit ? "GEM_QUIT" :
                       result==Result::refused ? "GEM_REFUSED" : "GEM_FAIL";
    std::puts(marker);
    return result==Result::complete || result==Result::quit ? 0 : 1;
}
