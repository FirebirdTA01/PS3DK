#include "../../samples/audio/hello-ppu-mic/source/mic_session.h"
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <vector>
using namespace mic_sample;
static unsigned pass=0,fail=0;
static void test(const char *name,bool ok) {std::printf("%s %s\n",ok?"ok":"FAIL",name);(ok?pass:fail)++;}
struct Fake {
    uint64_t time=0, quit_at=UINT64_MAX, detach_at=UINT64_MAX, delay_read=0, delay_type=0;
    int fail_acquire=-1,fail_release=-1, read_size=800, mic_type=2;
    bool present=true, ambiguous=false, wrong_format=false, borrowed=false, partial_first=false, change_type=false;
    unsigned reads=0,opens=0,starts=0; int opened=-1; unsigned requested=0;
    std::vector<int> released;
    static Fake &f(void *p){return *static_cast<Fake*>(p);}
    static int acquire(void *p,Stage s,int d,unsigned c) {
        auto &a=f(p); if(int(s)==a.fail_acquire)return -77;
        if(s==Stage::open){a.opens++;a.opened=d;a.requested=c;}
        if(s==Stage::start)a.starts++;
        return a.borrowed&&(s==Stage::mic_module||s==Stage::avconf_module)?1:0;
    }
    static int release(void *p,Stage s,int){auto &a=f(p);a.released.push_back(int(s));return int(s)==a.fail_release?-88:0;}
    static int inventory(void *){return 0;}
    static int attached(void *p,int d){auto&a=f(p);return a.present&&a.time<a.detach_at&&(d==0||(a.ambiguous&&d==1));}
    static int type(void *p,int,int *t){auto&a=f(p);a.time+=a.delay_type;*t=a.change_type&&a.reads?3:a.mic_type;return 0;}
    static int format(void *p,int,Format *v){auto&a=f(p);*v={a.wrong_format?2u:a.requested,2,16,1,48000};return 0;}
    static int read(void *p,int,uint8_t *b,unsigned n){auto&a=f(p);a.reads++;a.time+=a.delay_read;int size=a.partial_first&&a.reads==1?1:a.read_size;if(size>0&&unsigned(size)<=n)std::memset(b,0,unsigned(size));return size;}
    static int pump(void *p){return f(p).time>=f(p).quit_at;}
    static uint64_t now(void *p){return f(p).time;}
    static void wait(void *p,uint64_t n){f(p).time+=n;}
    static void emit(void *,const Window &){}
    Ops ops(){return {this,acquire,release,inventory,attached,type,format,read,pump,now,wait,emit};}
    bool freed(Stage s){return std::find(released.begin(),released.end(),int(s))!=released.end();}
};
int main(){
    test("capture completion exits zero",exit_status(Result::complete)==0);
    test("execution failure exits one",exit_status(Result::failed)==1);
    test("refusal exits two",exit_status(Result::refused)==2);
    test("guest quit exits three",exit_status(Result::quit)==3);
    {Fake f;Stats s;auto r=run(f.ops(),{},s);test("silence is captured data and cleans in reverse order",r==Result::complete&&s.frames>0&&s.bytes>0&&s.device==0&&f.requested==4&&f.time==30000000&&f.released==std::vector<int>({5,4,3,2,1,0})&&s.retained==0);}
    {Fake f;f.present=false;Stats s;auto r=run(f.ops(),{},s);test("absent Eye refuses at ten seconds without open",r==Result::refused&&f.time==10000000&&f.opens==0&&f.freed(Stage::init));}
    {Fake f;f.mic_type=3;Stats s;test("USB never substitutes for Eye",run(f.ops(),{},s)==Result::refused&&f.opens==0);}
    {Fake f;f.ambiguous=true;Stats s;test("ambiguous Eye selection refuses",run(f.ops(),{},s)==Result::refused&&f.opens==0);}
    {Fake f;f.ambiguous=true;Stats s;Config c;c.device=1;test("explicit slot resolves ambiguity",run(f.ops(),c,s)==Result::complete&&f.opened==1);}
    {Fake f;f.mic_type=3;Stats s;Config c{true,0,1};test("generic requires explicit mode and uses requested channels",run(f.ops(),c,s)==Result::complete&&f.requested==1);}
    {Fake f;Stats s;Config c{true,-1,1};test("generic without device refuses before init",run(f.ops(),c,s)==Result::refused&&f.released.empty());}
    {Fake f;Stats s;Config c{false,0,2};test("Eye channel downgrade rejected before init",run(f.ops(),c,s)==Result::refused&&f.opens==0);}
    {Fake f;f.wrong_format=true;Stats s;test("negotiated downgrade closes without starting",run(f.ops(),{},s)==Result::refused&&f.starts==0&&f.freed(Stage::open));}
    {Fake f;f.read_size=0;Stats s;test("zero bytes refuses at two seconds with cleanup",run(f.ops(),{},s)==Result::refused&&f.time==2000000&&f.freed(Stage::start));}
    {Fake f;f.read_size=-33;Stats s;test("read error retains first failure",run(f.ops(),{},s)==Result::failed&&s.first_error==-33&&f.freed(Stage::start));}
    {Fake f;f.read_size=16385;Stats s;test("oversized read never reaches meter",run(f.ops(),{},s)==Result::failed&&s.frames==0&&f.freed(Stage::start));}
    {Fake f;f.detach_at=10000;Stats s;test("detach after start requests stop",run(f.ops(),{},s)==Result::refused&&f.freed(Stage::start));}
    {Fake f;f.change_type=true;Stats s;test("device type change refuses before another read",run(f.ops(),{},s)==Result::refused&&f.reads==1&&f.freed(Stage::start));}
    {Fake f;f.partial_first=true;Stats s;test("trailing partial frame fails completed session",run(f.ops(),{},s)==Result::failed&&s.frames>0&&f.time==30000000&&f.freed(Stage::start));}
    {Fake f;f.quit_at=10000;Stats s;test("capture quit stops before closing",run(f.ops(),{},s)==Result::quit&&f.released.front()==int(Stage::start));}
    {Fake f;f.present=false;f.quit_at=10000;Stats s;test("discovery quit does not invent acquired stages",run(f.ops(),{},s)==Result::quit&&!f.freed(Stage::open)&&f.freed(Stage::init));}
    {Fake f;f.delay_type=11000000;Stats s;test("slow selection cannot open beyond discovery deadline",run(f.ops(),{},s)==Result::refused&&f.opens==0);}
    {Fake f;f.delay_read=31000000;Stats s;test("slow read cannot establish successful capture beyond deadline",run(f.ops(),{},s)==Result::refused&&f.reads==1&&s.bytes==0&&f.freed(Stage::start));}
    {Fake f;f.delay_read=2100000;Stats s;test("late first payload cannot erase the no-data deadline",run(f.ops(),{},s)==Result::refused&&f.reads==1&&s.bytes==0);}
    {Fake f;f.delay_type=2100000;Stats s;test("slow capture type query expires stall before read",run(f.ops(),{},s)==Result::refused&&f.reads==0);}
    for(int stage=0;stage<6;stage++){Fake f;f.fail_acquire=stage;Stats s;auto r=run(f.ops(),{},s);test("acquire failure unwinds only earlier stages",r==Result::failed&&s.first_error==-77&&f.released.size()==unsigned(stage)&&!f.freed(Stage(stage)));}
    {Fake f;f.borrowed=true;Stats s;test("borrowed modules are not unloaded",run(f.ops(),{},s)==Result::complete&&!f.freed(Stage::mic_module)&&!f.freed(Stage::avconf_module));}
    {Fake f;f.fail_release=int(Stage::start);Stats s;test("stop failure retains active mic and still releases independent resources",run(f.ops(),{},s)==Result::failed&&s.retained!=0&&!f.freed(Stage::open)&&!f.freed(Stage::init)&&!f.freed(Stage::mic_module)&&f.freed(Stage::callback)&&f.freed(Stage::avconf_module));}
    {Fake f;f.fail_release=int(Stage::open);Stats s;test("close failure prevents end and unload",run(f.ops(),{},s)==Result::failed&&!f.freed(Stage::init)&&!f.freed(Stage::mic_module));}
    {Fake f;f.fail_release=int(Stage::init);Stats s;test("end failure prevents module unload",run(f.ops(),{},s)==Result::failed&&!f.freed(Stage::mic_module));}
    {Fake f;f.fail_release=int(Stage::callback);Stats s;test("callback cleanup failure cannot pass",run(f.ops(),{},s)==Result::failed&&s.cleanup_failures==1);}
    {Fake f;f.read_size=-33;f.fail_release=int(Stage::start);Stats s;test("cleanup failure does not replace first error",run(f.ops(),{},s)==Result::failed&&s.first_error==-33&&s.cleanup_failures==1);}
    std::printf("mic-session: %u passed, %u failed\n",pass,fail);return fail?1:0;
}
