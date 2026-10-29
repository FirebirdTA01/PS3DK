#include "mic_session.h"
#include "mic_module_result.h"
#include <atomic>
#include <cinttypes>
#include <cstdio>
#include <cstring>
#include <unistd.h>
#include <sys/process.h>
#include <sys/sys_time.h>
#include <cell/mic.h>
#include <cell/cell_audio_in.h>
#include <cell/sysmodule.h>
#include <cell/sysutil.h>

SYS_PROCESS_PARAM(1001,0x100000);
using namespace mic_sample;
static_assert(sizeof(CellMicInputFormatI)==8 && sizeof(CellMicStatus)==36,"mic PRX record layouts");
static_assert(sizeof(CellAudioInDeviceInfo)==224,"audio input PRX record layout");
static_assert(CELLMIC_TYPE_EYETOY2==2 && CELLMIC_TYPE_USBAUDIO==3 && CELL_MAX_MICS==8,"mic selection types");
static_assert(std::atomic<unsigned>::is_always_lock_free,"callback flag must be lock free");
static_assert(module_result(CELL_OK)==0,"successful module load is owned");
static_assert(module_result(int(CELL_SYSMODULE_ERROR_DUPLICATED))==1,"duplicate module load is borrowed");
static_assert(module_result(int(CELL_SYSMODULE_ERROR_UNKNOWN))==int(CELL_SYSMODULE_ERROR_UNKNOWN),"module errors are preserved");
static std::atomic<unsigned> quit_requested{0};
struct Device {bool exit_logged=false;int last_type[8]{-1,-1,-1,-1,-1,-1,-1,-1};unsigned channels=4;};
static void callback(uint64_t status,uint64_t,void *) {
    if(status==CELL_SYSUTIL_REQUEST_EXITGAME)quit_requested.store(1,std::memory_order_release);
}
static int acquire(void *,Stage s,int device,unsigned channels) {
    int rc=-1;
    switch(s) {
    case Stage::callback:rc=cellSysutilRegisterCallback(0,callback,nullptr);break;
    case Stage::mic_module:rc=cellSysmoduleLoadModule(CELL_SYSMODULE_MIC);break;
    case Stage::avconf_module:rc=cellSysmoduleLoadModule(CELL_SYSMODULE_AVCONF_EXT);break;
    case Stage::init:rc=cellMicInit();break;
    case Stage::open:rc=cellMicOpenRaw(device,48000,int(channels));break;
    case Stage::start:rc=cellMicStart(device);break;
    }
    std::printf("MIC_ACQUIRE %s device=%d rc=0x%08x\n",stage_name(s),device,unsigned(rc));
    if(s==Stage::mic_module||s==Stage::avconf_module)return module_result(rc);
    return rc;
}
static int release(void *,Stage s,int device) {
    int rc=-1;
    switch(s) {
    case Stage::start:rc=cellMicStop(device);break;
    case Stage::open:rc=cellMicClose(device);break;
    case Stage::init:rc=cellMicEnd();break;
    case Stage::avconf_module:rc=cellSysmoduleUnloadModule(CELL_SYSMODULE_AVCONF_EXT);break;
    case Stage::mic_module:rc=cellSysmoduleUnloadModule(CELL_SYSMODULE_MIC);break;
    case Stage::callback:rc=cellSysutilUnregisterCallback(0);break;
    }
    std::printf("MIC_RELEASE %s rc=0x%08x\n",stage_name(s),unsigned(rc));return rc;
}
static int inventory(void *) {
    CellAudioInDeviceInfo info[16]{};
    const int count=cellAudioInGetAvailableDeviceInfo(16,info);
    std::printf("MIC_INVENTORY count=%d (audio-in numbers are not mic slot identities)\n",count);
    if(count<0)return count;
    if(count>16)return -1;
    for(int i=0;i<count;i++) {
        const auto &v=info[i];
        std::printf("MIC_DEVICE inventory=%d number=%u port=%u state=%u id=0x%016" PRIx64 " type=0x%016" PRIx64 " name=%.*s\n",
            i,unsigned(v.deviceNumber),unsigned(v.portType),unsigned(v.state),uint64_t(v.deviceId),uint64_t(v.type),64,v.name);
    }
    return 0;
}
static int attached(void *,int device){return cellMicIsAttached(device);}
static int type(void *p,int device,int *value) {
    auto &d=*static_cast<Device*>(p);
    const int rc=cellMicGetType(device,value);
    if(!rc&&d.last_type[device]!=*value) {
        std::printf("MIC_SLOT slot=%d reported_type=%d\n",device,*value);d.last_type[device]=*value;
    }
    return rc;
}
static int format(void *,int device,Format *f) {
    CellMicInputFormatI v{};
    const int rc=cellMicGetFormatRaw(device,&v);
    if(!rc)*f={v.channelNum,v.subframeSize,v.bitResolution,v.dataType,v.sampleRate};
    std::printf("MIC_FORMAT rc=0x%08x channels=%u subframe=%u bits=%u endian=%u rate=%u\n",
        unsigned(rc),unsigned(v.channelNum),unsigned(v.subframeSize),unsigned(v.bitResolution),unsigned(v.dataType),unsigned(v.sampleRate));
    return rc;
}
static int read(void *,int device,uint8_t *bytes,unsigned capacity){return cellMicReadRaw(device,bytes,int(capacity));}
static int pump(void *p) {
    const int rc=cellSysutilCheckCallback();if(rc)return rc;
    if(!quit_requested.load(std::memory_order_acquire))return 0;
    auto &d=*static_cast<Device*>(p);
    if(!d.exit_logged){std::puts("MIC_EXIT_CALLBACK serviced");d.exit_logged=true;}
    return 1;
}
static uint64_t now(void *){return uint64_t(sys_time_get_system_time());}
static void wait(void *,uint64_t us){usleep(static_cast<useconds_t>(us));}
static void emit(void *p,const Window &w) {
    const auto &d=*static_cast<Device*>(p);
    for(unsigned c=0;c<d.channels;c++)std::printf("MIC_LEVEL channel=%u frames=%u peak=%u rms=%u clipped=%u\n",c,w.frames,w.peak[c],w.rms[c],w.clipped[c]);
}
static bool args(int argc,char **argv,Config &c) {
    bool device=false,channels=false,generic=false;
    for(int i=1;i<argc;i++) {
        if(!std::strcmp(argv[i],"--generic")&&!generic){c.generic=generic=true;continue;}
        bool dev=!std::strcmp(argv[i],"--device"),ch=!std::strcmp(argv[i],"--channels");
        if((!dev&&!ch)||(dev&&device)||(ch&&channels)||i+1>=argc)return false;
        const char *v=argv[++i];if(v[0]<'0'||v[0]>'7'||v[1])return false;
        if(dev){c.device=v[0]-'0';device=true;}else{c.channels=unsigned(v[0]-'0');channels=true;}
    }
    return (!c.generic||device)&&(!channels||c.generic)&&
        (c.channels==1||c.channels==2||c.channels==4);
}
int main(int argc,char **argv) {
    std::setvbuf(stdout,nullptr,_IONBF,0);
    Config c;
    if(!args(argc,argv,c)) {
        std::puts("Usage: hello-ppu-mic [--device 0..7] [--generic --channels 1|2|4]");
        std::puts("MIC_REFUSED configuration");return exit_status(Result::refused);
    }
    std::printf("MIC_BEGIN mode=%s device=%d channels=%u rate=48000 discovery_s=10 capture_s=30 no_data_s=2\n",c.generic?"GENERIC":"EYE",c.device,c.channels);
    std::puts("MIC_PROOF actual Eye array capture and channel identity require hardware verification; no raw audio is saved");
    Device d;d.channels=c.channels;
    Ops o{&d,acquire,release,inventory,attached,type,format,read,pump,now,wait,emit};
    Stats s;const auto r=run(o,c,s);
    std::printf("MIC_SUMMARY mode=%s device=%d frames=%" PRIu64 " bytes=%" PRIu64 " windows=%u elapsed_us=%" PRIu64 " cleanup_failures=%u retained=%u first_error=0x%08x reason=%s\n",
        c.generic?"GENERIC":"EYE",s.device,s.frames,s.bytes,s.windows,s.elapsed_us,s.cleanup_failures,s.retained,unsigned(s.first_error),s.reason);
    std::puts(r==Result::complete?"MIC_CAPTURE_OK":r==Result::quit?"MIC_QUIT":r==Result::refused?"MIC_REFUSED":"MIC_FAIL");
    return exit_status(r);
}
