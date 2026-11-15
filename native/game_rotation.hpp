// Rotation manager: arbitrates rotation requests from modules and applies the winner each frame.
// - Request: target {yaw, pitch}; mode 0 = game turn (relative), 1 and 2 = write the rotation, 3 = silent
// (only PlayerAuthInputPacket); priority; relative / yaw-only flags; head; smoothing; maximum step;
// "reached" callback.
// - Submit: refused if the module is off or the target is not finite; replaces the module's previous
// request; its tag is the type on top of the stack (0 if empty).
// - Stack: entering a type not on the stack drops the non-persistent requests of that type (type 0 also
// drops 4); type 0 carries the frame time (<= 0.1 s, 1/60 on first use).
// - Settle (empty stack): drop requests of disabled modules, pick the visible request (mode != 3) with the
// highest priority (tie: lowest id); the others lose their callback; apply it; fire the callback once
// within 0.01 deg.
// - Apply: relative -> target = current + target; yaw delta fmodf(target - current, 360), pitch delta
// target - current; factor 1 - exp(-smoothing*dt) when smoothing > 0; deltas clamped to the max step;
// mode < 2 turns through the game (RVA 0x6da57d0); modes 1 and 2 write rotation + head; mode 2 is
// remembered for the camera.
// - UpdatePlayerFromCamera: a chosen mode-2 request rewrites the pose and skips the game call.
// - PlayerAuthInputPacket (0x90): the chosen silent request, if no visible one wins (or it is mode 0 or
// lower priority), sets yaw +0x34 and head +0x44, and pitch +0x30 unless yaw-only.
// The current rotation is read from the actor; humanisation is not implemented.
#pragma once
#include "game_read.hpp"
#include "game_events.hpp"
#include <atomic>
#include <cstdio>
#include <cstring>
#include <cmath>
#include <functional>
#include <mutex>
#include <vector>

namespace game {
struct RotRequest{int module=0;float yaw=0,pitch=0;int mode=2,priority=1;bool relative=false,yawOnly=false;float head=0;bool hasHead=false;
 float speed=0,maxStep=0;bool hasMax=false;std::function<bool(uintptr_t)> predicate;std::function<void(uintptr_t)> reached;
 int tag=0;bool persistent=false,fresh=true,eligible=true;float out[3]{};bool hasOut=false;};
struct RotManager{std::recursive_mutex guard;std::vector<RotRequest> requests;std::vector<int> stack;int64_t last=0;bool hasLast=false;float dt=1.f/60.f;int frames=0;
 bool selValid=false;int selId=0;float memory[3]{};bool memValid=false;uintptr_t actor=0;std::atomic<bool> enabled[32]{};std::atomic<bool> fault{false};};
inline RotManager& Rotations(){static auto* m=new RotManager;return *m;}
inline void(*rotationLog)(const char*)=nullptr;inline void RotOnce(int i,const char* t){static bool told[16]{};if(i<16&&!told[i]&&rotationLog){told[i]=true;rotationLog(t);}}
inline bool RotEnabled(int module){return module>0&&module<32&&Rotations().enabled[module].load();}
inline bool RotFinite(float v){return std::isfinite(v);}
inline bool RotRead(uintptr_t actor,float* pitch,float* yaw){uint64_t rot{};return Read(actor+0x250,&rot)&&rot&&Read(rot,pitch)&&Read(rot+4,yaw);}

// Submit.
inline void RotSubmit(RotRequest r){auto& m=Rotations();std::lock_guard<std::recursive_mutex> lock(m.guard);if(!RotEnabled(r.module)||!RotFinite(r.yaw)||!RotFinite(r.pitch))return;
 for(auto it=m.requests.begin();it!=m.requests.end();)it=it->module==r.module?m.requests.erase(it):it+1;
 RotOnce(0,"Rotations: first request accepted");r.tag=m.stack.empty()?0:m.stack.back();r.persistent=false;r.fresh=true;r.eligible=true;r.hasOut=false;m.requests.push_back(std::move(r));}
// Persistent submit: the request is marked fresh only if its tag is on the stack at submission time.
inline void RotSubmitPersistent(RotRequest r,int tag){auto& m=Rotations();std::lock_guard<std::recursive_mutex> lock(m.guard);if(!RotEnabled(r.module)||!RotFinite(r.yaw)||!RotFinite(r.pitch))return;
 for(auto it=m.requests.begin();it!=m.requests.end();)it=it->module==r.module?m.requests.erase(it):it+1;
 bool inStack=false;for(int t:m.stack)if(t==tag)inStack=true;r.tag=tag;r.persistent=true;r.fresh=inStack;r.eligible=true;r.hasOut=false;m.requests.push_back(std::move(r));}
// Module whose request drives the view now (the last one applied, still submitted), 0 for none.
inline int RotOwner(){auto& m=Rotations();std::lock_guard<std::recursive_mutex> lock(m.guard);if(!m.selValid)return 0;for(auto& r:m.requests)if(r.module==m.selId)return m.selId;return 0;}
inline bool RotHas(int module){auto& m=Rotations();std::lock_guard<std::recursive_mutex> lock(m.guard);for(auto& r:m.requests)if(r.module==module)return true;return false;}
inline void RotRelease(int module){auto& m=Rotations();std::lock_guard<std::recursive_mutex> lock(m.guard);for(auto it=m.requests.begin();it!=m.requests.end();)it=it->module==module?m.requests.erase(it):it+1;}

// Local player; without one, every request is dropped.
inline uintptr_t RotActor(RotManager& m){Context c{};if(Local(&c)&&(!m.actor||m.actor==c.actor)){m.actor=c.actor;return c.actor;}
 m.requests.clear();m.selValid=false;m.memValid=false;m.actor=0;return 0;}
// Best request: silent (mode 3) or visible.
inline RotRequest* RotBest(RotManager& m,uintptr_t actor,bool silent){RotRequest* best=nullptr;if(menuPause)return nullptr;
 for(auto& r:m.requests){if(silent!=(r.mode==3)||!RotEnabled(r.module))continue;if(r.predicate&&!r.predicate(actor))continue;
  if(!best||best->priority<r.priority||(best->priority==r.priority&&r.module<best->module))best=&r;}return best;}
// Target reached.
inline bool RotReached(RotManager& m,const RotRequest& r,uintptr_t actor,float tolerance){if(!m.selValid||m.selId!=r.module||r.fresh)return false;float p{},y{};if(!RotRead(actor,&p,&y))return false;
 return std::fabs(fmodf(r.yaw-y,360.f))<=tolerance&&std::fabs(r.pitch-p)<=tolerance;}
inline float RotClampPitch(float v){return v>90.f?90.f:(v<-90.f?-90.f:v);}
// Apply a request.
inline void RotApply(RotManager& m,RotRequest& r,uintptr_t actor){Context c{};Client(&c);RotOnce(1,"Rotations: first request applied");
 if(r.fresh||!r.hasOut){float cp{},cy{};if(!RotRead(actor,&cp,&cy))return;
  if(r.relative){r.yaw=fmodf(r.yaw+cy,360.f);r.pitch=RotClampPitch(r.pitch+cp);r.relative=false;}
  float dy=fmodf(r.yaw-cy,360.f),dp=r.pitch-cp,factor=1.f;
  if(r.speed>0.f){float step=m.dt;if(r.tag==1){uint64_t mc{},timer{};float tps=20.f;if(Read(c.client+0x1d0,&mc)&&mc&&Read(mc+0xe8,&timer)&&timer)Read(timer,&tps);step=tps<=0.f?0.05f:1.f/tps;}
   factor=1.f-expf(-(step<0?0:step)*r.speed);factor=factor<0?0:(factor>1?1:factor);}
  dy*=factor;dp*=factor;if(r.hasMax){const float s=r.maxStep;dy=dy>s?s:(dy<-s?-s:dy);dp=dp>s?s:(dp<-s?-s:dp);}
  const float yaw=fmodf(dy+cy,360.f),pitch=RotClampPitch(dp+cp);
  if(r.mode<2){float turn[2]={-dp,dy};reinterpret_cast<void(*)(uintptr_t,float*)>(c.base+Rva(0x6da57d0))(actor,turn);}   // game turn function
  r.out[0]=pitch;r.out[1]=r.hasHead?r.head:yaw;r.out[2]=yaw;r.hasOut=true;r.fresh=false;}
 bool keep=false;
 if(r.mode==1||r.mode==2){uint64_t rot{};if(Read(actor+0x250,&rot)&&rot){*(float*)rot=r.out[0];*(float*)(rot+4)=r.out[2];}   // write rotation and head
  uintptr_t registry{},head{};uint32_t entity{};if(ActorEntity(actor,&registry,&entity)&&Element(registry,entity,"ActorHeadRotationComponent",8,&head)){*(float*)head=r.out[1];*(float*)(head+4)=r.out[1];}
  if(r.mode==2){memcpy(m.memory,r.out,sizeof(m.memory));keep=r.hasOut;}}
 m.memValid=keep;m.selValid=true;m.selId=r.module;}
// Settle when the stack is empty.
inline void RotSettle(RotManager& m){if(m.requests.empty()){m.selValid=false;m.memValid=false;m.actor=0;return;}
 const uintptr_t actor=RotActor(m);if(!actor)return;
 for(auto it=m.requests.begin();it!=m.requests.end();)it=RotEnabled(it->module)?it+1:m.requests.erase(it);   // drop requests of disabled modules
 RotRequest* best=nullptr;for(auto& r:m.requests){if(!r.eligible||r.mode==3)continue;if(!best||best->priority<r.priority||(best->priority==r.priority&&r.module<best->module))best=&r;}
 for(auto& r:m.requests)if(&r!=best)r.reached=nullptr;
 if(!best){m.selValid=false;m.memValid=false;return;}RotOnce(2,"Rotations: a request was selected");
 {static ULONGLONG told=0;const ULONGLONG now=GetTickCount64();if(rotationLog&&now-told>2000){told=now;char line[200];snprintf(line,sizeof(line),"Rotations: applying module %d (mode %d, %s, priority %d, %zu request(s))",best->module,best->mode,best->relative?"relative":"absolute",best->priority,m.requests.size());rotationLog(line);}}   // which module drives the view
 if(best->fresh||((!m.selValid||m.selId!=best->module)&&best->hasOut))RotApply(m,*best,actor);
 if(best->reached&&RotReached(m,*best,actor,0.01f)){auto call=best->reached;call(actor);}}

inline void RotPushWork(int type){auto& m=Rotations();std::lock_guard<std::recursive_mutex> lock(m.guard);bool absent=true;for(int t:m.stack)if(t==type)absent=false;m.stack.push_back(type);if(!absent)return;
 if(type==0){LARGE_INTEGER f{},q{};QueryPerformanceFrequency(&f);QueryPerformanceCounter(&q);const int64_t now=(int64_t)((double)q.QuadPart*1e9/(double)f.QuadPart);
  float dt=1.f/60.f;if(m.hasLast){const float d=(float)(now-m.last)/1e9f;m.last=now;if(d>0.f)dt=d<=0.1f?d:0.1f;}else{m.hasLast=true;m.last=now;}m.dt=dt;++m.frames;}
 for(auto it=m.requests.begin();it!=m.requests.end();)it=(!it->persistent&&(it->tag==type||(type==0&&it->tag==4)))?m.requests.erase(it):it+1;
 // A persistent request of this type is recomputed when its type is entered.
 for(auto& r:m.requests)if(r.persistent&&(r.tag==type||(type==0&&r.tag==4)))r.fresh=true;}
inline void RotPopWork(){auto& m=Rotations();std::lock_guard<std::recursive_mutex> lock(m.guard);if(m.stack.empty())return;m.stack.pop_back();if(m.stack.empty())RotSettle(m);}
inline bool RotCameraWork(uintptr_t actor){auto& m=Rotations();std::lock_guard<std::recursive_mutex> lock(m.guard);if(!m.stack.empty()||m.requests.empty())return false;
 Context c{};if(!Local(&c)||c.actor!=actor)return false;RotRequest* r=RotBest(m,actor,false);if(!r||r->mode!=2||!m.selValid||m.selId!=r->module||!m.memValid)return false;
 uintptr_t registry{},head{};uint32_t entity{};if(ActorEntity(actor,&registry,&entity)&&Element(registry,entity,"ActorHeadRotationComponent",8,&head))*(float*)head=m.memory[1];
 uint64_t rot{};if(Read(actor+0x250,&rot)&&rot){*(float*)rot=m.memory[0];*(float*)(rot+4)=m.memory[2];}
 {static ULONGLONG told=0;const ULONGLONG now=GetTickCount64();if(rotationLog&&now-told>2000){told=now;char line[120];snprintf(line,sizeof(line),"Rotations: camera pose rewritten for module %d",r->module);rotationLog(line);}}return true;}
inline std::atomic<uint64_t> rotAuthInputs{0};   // PlayerAuthInputPackets sent so far (see StepUse in game_actions.hpp)
inline void RotSilentWork(uintptr_t packet){uint64_t table{},get{};if(!Read(packet,&table)||!table||!Read(table+8,&get)||!get||reinterpret_cast<int(*)(uintptr_t)>(get)(packet)!=0x90)return;rotAuthInputs.fetch_add(1);
 auto& m=Rotations();std::lock_guard<std::recursive_mutex> lock(m.guard);if(!m.stack.empty()||m.requests.empty())return;Context c{};if(!Local(&c))return;
 RotRequest* s=RotBest(m,c.actor,true);RotRequest* v=RotBest(m,c.actor,false);if(!s||(v&&v->mode!=0&&v->priority>=s->priority))return;
 *(float*)(packet+0x34)=s->yaw;*(float*)(packet+0x44)=s->yaw;if(!s->yawOnly)*(float*)(packet+0x30)=s->pitch;}
// Records the faulting code and address (module + offset) once, for the journal.
inline int RotFaultFilter(EXCEPTION_POINTERS* e){static std::atomic<bool> told{false};if(!told.exchange(true)&&rotationLog&&e&&e->ExceptionRecord){
 const auto* r=e->ExceptionRecord;const uintptr_t at=reinterpret_cast<uintptr_t>(r->ExceptionAddress);HMODULE mod{};char name[MAX_PATH]="?";
 if(GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,reinterpret_cast<LPCSTR>(at),&mod))GetModuleFileNameA(mod,name,MAX_PATH);
 const char* base=strrchr(name,'\\');char line[400];snprintf(line,sizeof(line),"Rotations: fault %08lx at %s+0x%llx (read/write address 0x%llx)",r->ExceptionCode,base?base+1:name,
  (unsigned long long)(at-reinterpret_cast<uintptr_t>(mod)),(unsigned long long)(r->NumberParameters>1?r->ExceptionInformation[1]:0));rotationLog(line);}
 return EXCEPTION_EXECUTE_HANDLER;}
inline void RotFault(){RotOnce(3,"Rotations: fault caught, rotation manager stopped");Rotations().fault=true;rotationPush=nullptr;rotationPop=nullptr;rotationCamera=nullptr;rotationSilent=nullptr;}
// The "reached" callbacks run module work (block breaking, item use) on the thread of the pushed event: the item
// registry is lent there, otherwise item descriptors asserted (game +0x18db1a2) and the manager stopped.
inline void RotPush(int t){uint64_t* lent=ItemRegistryLendBegin();__try{RotPushWork(t);}__except(RotFaultFilter(GetExceptionInformation())){RotFault();}ItemRegistryLendEnd(lent);}
inline void RotPop(){uint64_t* lent=ItemRegistryLendBegin();__try{RotPopWork();}__except(RotFaultFilter(GetExceptionInformation())){RotFault();}ItemRegistryLendEnd(lent);}
inline bool RotCamera(uintptr_t a){__try{return RotCameraWork(a);}__except(RotFaultFilter(GetExceptionInformation())){RotFault();return false;}}
inline void RotSilent(uintptr_t p){__try{RotSilentWork(p);}__except(RotFaultFilter(GetExceptionInformation())){RotFault();}}
inline void RotInstall(){if(Rotations().fault)return;rotationPush=RotPush;rotationPop=RotPop;rotationCamera=RotCamera;rotationSilent=RotSilent;}
// Readiness check used by the action planner's use step.
inline bool RotReadyFor(int module,float tolerance){auto& m=Rotations();std::lock_guard<std::recursive_mutex> lock(m.guard);if(!m.stack.empty())return false;Context c{};if(!Local(&c))return false;
 RotRequest* r=RotBest(m,c.actor,false);return r&&r->module==module&&RotReached(m,*r,c.actor,tolerance);}
inline float RotFrameDt(){return Rotations().dt;}

// ---- Derp (GameUpdateEvent): angle advancing by dt x speed x 300 deg/s; Packet -> silent (yaw only),
// Entity -> mode 2 {angle, current pitch}, Camera -> relative mode 0; priority 0.
enum RotModuleId{kRotDerp=6,kRotWindCharge=4,kRotPearlCatch=5,kRotAutoBlockReset=3};
struct DerpShared{std::atomic<int> mode{0};std::atomic<float> speed{1.f};uintptr_t actor=0;float angle=0;int last=-1;};
inline DerpShared& Derp(){static auto* d=new DerpShared;return *d;}
inline void DerpWork(){auto& d=Derp();if(!RotEnabled(kRotDerp))return;Context c{};if(!Local(&c)){d.actor=0;return;}const int mode=d.mode;
 float p{},y{};if(!RotRead(c.actor,&p,&y))return;if(d.actor!=c.actor||d.last!=mode){d.actor=c.actor;d.angle=y;d.last=mode;}
 const float step=RotFrameDt()*d.speed*300.f;d.angle=fmodf(d.angle+step,360.f);
 RotRequest r;r.module=kRotDerp;r.priority=0;
 if(mode==2){r.mode=3;r.yaw=d.angle;r.pitch=0;r.yawOnly=true;}else if(mode==1){r.mode=2;r.yaw=d.angle;r.pitch=p;}else{r.mode=0;r.yaw=step;r.pitch=0;r.relative=true;}
 RotSubmit(std::move(r));}
// ---- Auto 360 (GameUpdateEvent): one full turn per activation, alternating direction. Progress t advances by
// dt / max(4 - 3.85 x Speed, 0.15) up to 1; the yaw moves along 360 x (3 - 2t) t^2 (eased). Camera -> relative mode 0
// {delta}, Entity -> mode 2 {angle, current pitch}, Packet -> silent yaw only; priority 3, every GameUpdate.
constexpr int kRotAuto360=14;
struct Auto360Shared{std::atomic<int> mode{0};std::atomic<float> speed{0.7f};std::atomic<bool> start{false};float t=0,angle=0,direction=-1;int active=0;};
inline Auto360Shared& Auto360(){static auto* s=new Auto360Shared;return *s;}
inline void Auto360Work(){auto& s=Auto360();Context c{};
 if(s.start.exchange(false)){s.t=0;s.angle=0;float p{},y{};if(Local(&c)&&RotRead(c.actor,&p,&y))s.angle=y;s.direction=-s.direction;s.active=s.mode;}   // enable
 if(!RotEnabled(kRotAuto360)||menuPause||!Local(&c))return;   // paused with the other view modules while the menu is open
 const float speed=s.speed;float duration=fmaf(speed,-3.85f,4.0f);if(speed==1.0f||duration<0.15f)duration=0.15f;
 const float t0=s.t;float t=RotFrameDt()/duration+t0;if(t>1.0f)t=1.0f;s.t=t;
 const float delta=((3.0f-(t+t))*t*t-(3.0f-(t0+t0))*t0*t0)*360.0f*s.direction;s.angle=fmodf(s.angle+delta,360.0f);
 float p{},y{};if(!RotRead(c.actor,&p,&y))return;
 RotRequest r;r.module=kRotAuto360;r.priority=3;
 if(s.active==2){r.mode=3;r.yaw=s.angle;r.pitch=0;r.yawOnly=true;}else if(s.active==1){r.mode=2;r.yaw=s.angle;r.pitch=p;}else{r.mode=0;r.yaw=delta;r.pitch=0;r.relative=true;}
 RotSubmit(std::move(r));}
inline void DerpTick(){__try{DerpWork();Auto360Work();}__except(RotFaultFilter(GetExceptionInformation())){RotFault();}}
}
