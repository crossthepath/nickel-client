// Action planner: short per-module item sequences (find slot, switch, use, switch back) used by Throw Pot,
// Spear Dash, Auto Block Reset, Wind Charge, Pearl Catch, Nuker and Scaffold.
// - Submitting a sequence cancels the module's previous ones. Each record keeps the actor, start time, step
// index, the original hotbar slot (inventory+0x10) and an optional silent slot.
// - Every frame: silent equipment older than 5000 ms is purged; without a valid local player everything is
// cancelled. Each sequence checks its conditions (any false = cancelled), then runs steps from its index:
// 0 = wait for the next frame, 1 = next step, 2 = cancel. Passing the last step finishes it with result 0.
// - Finishing restores the original slot and sends the original MobEquipmentPacket silently; callbacks run
// with the result after a 5 s resync window opens.
// - Incoming MobEquipmentPacket (0x1f) for the local player, container 0, during a sequence or the 5 s
// after it, is dropped so the server cannot snap the selected slot back.
// Sequences run in the screen render hook (game_render.hpp), not GameUpdate: the NetworkItemStackDescriptor
// constructor reads a thread-local item registry (TLS +0xce8/+0xcf0) that only exists on the render thread.
#pragma once
#include "game_packets.hpp"
#include "game_events.hpp"
#include "game_rotation.hpp"
#include "game_utility_packets.hpp"
#include <algorithm>
#include <windows.h>
#include <psapi.h>
#include <intrin.h>
#include <array>
#include <memory>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <functional>
#include <mutex>
#include <new>
#include <random>
#include <string>
#include <vector>

namespace game {
inline int64_t ActionNs(){LARGE_INTEGER f{},c{};QueryPerformanceFrequency(&f);QueryPerformanceCounter(&c);const int64_t q=c.QuadPart/f.QuadPart,r=c.QuadPart%f.QuadPart;return q*1000000000+r*1000000000/f.QuadPart;}
inline double ActionMs(int64_t since){return (double)(ActionNs()-since)/1000000.0;}
inline volatile int actionStage=0;inline volatile uint64_t actionValue=0;   // last internal stage reached (crash diagnostics)
inline void(*actionLog)(const char*)=nullptr;
// Exception filter that logs the fault code and the faulting module + offset (one line per call), then handles it.
inline int FaultLogFilter(const char* who,EXCEPTION_POINTERS* e){if(actionLog&&e&&e->ExceptionRecord){const auto* r=e->ExceptionRecord;const uintptr_t at=reinterpret_cast<uintptr_t>(r->ExceptionAddress);
  HMODULE mod{};char name[MAX_PATH]="?";if(GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,reinterpret_cast<LPCSTR>(at),&mod))GetModuleFileNameA(mod,name,MAX_PATH);
  const char* base=strrchr(name,'\\');char line[400];snprintf(line,sizeof(line),"%s: fault %08lx at %s+0x%llx (address 0x%llx); stopped until restart",who,r->ExceptionCode,base?base+1:name,
   (unsigned long long)(at-reinterpret_cast<uintptr_t>(mod)),(unsigned long long)(r->NumberParameters>1?r->ExceptionInformation[1]:0));actionLog(line);
  // Return addresses into the faulting module found on the stack (call path).
  if(mod&&e->ContextRecord){MODULEINFO info{};if(K32GetModuleInformation(GetCurrentProcess(),mod,&info,sizeof(info))){const uintptr_t lo=(uintptr_t)mod,hi=lo+info.SizeOfImage;
   std::string path;const uint64_t* sp=reinterpret_cast<const uint64_t*>(e->ContextRecord->Rsp);int found=0;
   for(int i=0;i<256&&found<8;++i){uint64_t v{};if(!Read((uintptr_t)(sp+i),&v))break;if(v>lo&&v<hi){char part[24];snprintf(part,sizeof(part)," +0x%llx",(unsigned long long)(v-lo));path+=part;++found;}}
   if(!path.empty()){std::string text=std::string(who)+": stack"+path;actionLog(text.c_str());}}}}
 return EXCEPTION_EXECUTE_HANDLER;}
inline void ActionFaultLog(const char* where,unsigned long code){if(actionLog){char line[160];snprintf(line,sizeof(line),"Actions: fault in %s (stage %d, code 0x%08lx, value %llx); action queue stopped",where,actionStage,code,(unsigned long long)actionValue);actionLog(line);}}

// ---- player inventory
inline int32_t* ActionSelected(uintptr_t actor){uint64_t inv{};return Read(actor+0x5e0,&inv)&&inv?(int32_t*)(inv+0x10):nullptr;}
inline uint64_t ActionContainer(uintptr_t actor){uint64_t inv{},box{};return Read(actor+0x5e0,&inv)&&inv&&Read(inv+0xb8,&box)?box:0;}
inline uint64_t ActionStackAt(const Context& c,uint64_t box,int slot){const uint64_t f=box?Virtual0(c,box,0x38):0;return f?StackAt(f,box,slot):0;}
inline int ActionSize(const Context& c,uint64_t box){const uint64_t f=box?Virtual0(c,box,0xa0):0;return f?reinterpret_cast<int(*)(uint64_t)>(f)(box):0;}
inline uint64_t ActionHeld(const Context& c,uintptr_t actor){int32_t* s=ActionSelected(actor);return s?ActionStackAt(c,ActionContainer(actor),*s):0;}
// Item of a stack: block *(stack+8), non-zero count at *(block+8), item at *(block).
inline uint64_t ActionItem(uint64_t stack){uint64_t block{},item{};int32_t count{};return stack&&Read(stack+8,&block)&&block&&Read(block+8,&count)&&count&&Read(block,&item)?item:0;}
// Item name: std::string at item+0xd8.
inline bool ActionName(uint64_t stack,std::string* name){const uint64_t item=ActionItem(stack);char text[160]{};if(!item||!Text(item+0xd8,text,sizeof(text)))return false;*name=text;return true;}
inline bool ActionNameIs(uint64_t stack,const std::string& want){std::string n;return ActionName(stack,&n)&&n==want;}
inline bool ActionNameHas(uint64_t stack,const std::string& part){std::string n;return ActionName(stack,&n)&&part.size()<=n.size()&&(part.empty()||n.find(part)!=std::string::npos);}

// ---- silent MobEquipmentPacket
inline void DescriptorReset(uint64_t d){
 auto* text=reinterpret_cast<std::string*>(d+0x40);text->~basic_string();new(text) std::string();
 const int8_t index=*(int8_t*)(d+0x30);if((uint32_t)(index+1)>=2){const uint64_t table=*(uint64_t*)(d+0x20);reinterpret_cast<void(*)(uint64_t,int)>(*(uint64_t*)table)(d+0x20,0);}
 const uint64_t inner=*(uint64_t*)(d+8);if(inner){const uint64_t table=*(uint64_t*)inner;reinterpret_cast<void(*)(uint64_t,int)>(*(uint64_t*)table)(inner,1);}}
// Item registry of the current thread. The game keeps it in thread-local storage: TLS index at game RVA 0x128767c8,
// block = gs:[0x58][index], init byte +0x120, registry weak reference at +0xce8 (object) / +0xcf0 (control block).
// On a server connection the thread that runs GameUpdate subscribers has no registry there, and the game's item
// descriptor code (RVA 0x1a5ece0) then stops on a deliberate null write (RVA 0x18db1a2). The registry seen on the
// client level tick thread is remembered and lent to the current thread around the descriptor call.
inline uint64_t* ItemRegistrySlot(const Context& c){return ItemRegistrySlotAt(c.base);}   // ItemRegistry / ItemRegistrySlotAt: game_read.hpp
inline void ItemRegistryCapture(const Context& c){__try{const uint64_t* slot=ItemRegistrySlot(c);if(slot&&slot[0]&&slot[1]){auto& r=ItemRegistry();r.object=slot[0];r.control=slot[1];}}
 __except(EXCEPTION_EXECUTE_HANDLER){}}
// The game's descriptor constructor; a fault (registry still missing) is caught and the descriptor is left as the empty
// (air) descriptor: fields as initialised below plus the descriptor vtable (game RVA 0xeffaee0).
inline bool DescriptorCall(uint64_t function,uint64_t d,uint64_t stack){__try{reinterpret_cast<void(*)(uint64_t,uint64_t)>(function)(d,stack);return true;}
 __except(EXCEPTION_EXECUTE_HANDLER){return false;}}
inline bool DescriptorLent(const Context& c,uint64_t d,uint64_t stack){uint64_t* slot=nullptr;__try{slot=ItemRegistrySlot(c);}__except(EXCEPTION_EXECUTE_HANDLER){slot=nullptr;}
 auto& r=ItemRegistry();const bool lend=slot&&!slot[1]&&r.control.load();if(lend){slot[0]=r.object;slot[1]=r.control;}
 const bool ok=DescriptorCall(c.base+0x1aebe20,d,stack);if(lend){slot[0]=0;slot[1]=0;}return ok;}
inline void DescriptorBuild(const Context& c,uint64_t d,uint64_t stack){   // NetworkItemStackDescriptor(ItemStack const&), game function at RVA 0x1aebe20
 *(uint64_t*)(d+8)=0;*(int32_t*)(d+0x20)=0;*(uint8_t*)(d+0x30)=0;memset((void*)(d+0x40),0,0x10);*(uint64_t*)(d+0x50)=0;*(uint64_t*)(d+0x58)=0xf;
 actionStage+=100;actionValue=stack;
 if(!DescriptorLent(c,d,stack)){*(uint64_t*)d=c.base+0xeffaee0;static bool told=false;
  if(!told&&actionLog){told=true;std::string n="?";ActionName(stack,&n);const std::string line="Actions: item '"+n+"' could not be described (game assertion); an empty item descriptor is sent instead";actionLog(line.c_str());}}
 actionStage+=100;}
inline void SendMobEquipment(const Context& c,uintptr_t actor,int slot){actionStage=30;uint64_t id{};if(!RuntimeId(actor,&id))return;
 const uint64_t stack=ActionStackAt(c,ActionContainer(actor),slot);GamePacket p=CreatePacket(c,0x1f);
 if(p.object&&stack){*(uint64_t*)(p.object+0x30)=id;DescriptorReset(p.object+0x38);DescriptorBuild(c,p.object+0x38,stack);
  actionStage=31;*(int32_t*)(p.object+0x98)=slot;*(int32_t*)(p.object+0x9c)=slot;*(uint8_t*)(p.object+0xa0)=0;SendPacket(c,p.object);}
 ReleasePacket(p);}

// ---- ItemUseInventoryTransaction, sent through actor->vtable[0x718] (unique_ptr, freed by the game)
inline uint8_t* NewUseTransaction(const Context& c,uint64_t stack){uint8_t* t=(uint8_t*)::operator new(0x110);memset(t,0,0x110);
 actionStage=40;uint8_t* inv=t+0x10;*(float*)inv=1.0f;   // InventoryTransaction: empty unordered_map, then empty vector
 uint64_t* head=(uint64_t*)::operator new(0x38);head[0]=head[1]=(uint64_t)head;*(uint64_t*)(inv+8)=(uint64_t)head;*(uint64_t*)(inv+0x10)=0;
 uint64_t* buckets=(uint64_t*)::operator new(0x80);for(int i=0;i<16;++i)buckets[i]=(uint64_t)head;
 *(uint64_t*)(inv+0x18)=(uint64_t)buckets;*(uint64_t*)(inv+0x20)=(uint64_t)(buckets+16);*(uint64_t*)(inv+0x28)=(uint64_t)(buckets+16);*(uint64_t*)(inv+0x30)=7;*(uint64_t*)(inv+0x38)=8;
 *(int32_t*)(t+0x68)=0;*(uint8_t*)(t+0x6c)=1;*(uint8_t*)(t+0x80)=0;*(int32_t*)(t+0x84)=0;DescriptorBuild(c,(uint64_t)(t+0x90),stack);
 actionStage=41;*(uint16_t*)(t+0x108)=1;*(uint64_t*)t=c.base+0xf115f20;*(int32_t*)(t+8)=2;return t;}
inline void SendUseTransaction(const Context& c,uintptr_t actor,uint8_t* t){const uint64_t send=Virtual0(c,actor,0x718);if(!send)return;uint8_t* owned=t;
 reinterpret_cast<void(*)(uintptr_t,uint8_t**)>(send)(actor,&owned);}
inline bool ActionPosition(uintptr_t actor,float* p){uint64_t state{};return Read(actor+0x240,&state)&&state&&Read(state,&p[0])&&Read(state+4,&p[1])&&Read(state+8,&p[2]);}
// Plain use: type 1, face 0xff, selected slot, player position.
inline void UseItem(const Context& c,uintptr_t actor,uint64_t stack,bool prediction){int32_t* sel=ActionSelected(actor);float pos[3]{};if(!sel||!ActionPosition(actor,pos))return;
 actionStage=20;uint8_t* t=NewUseTransaction(c,stack);actionStage=21;*(int32_t*)(t+0x68)=1;*(uint8_t*)(t+0x6c)=0;*(uint8_t*)(t+0x80)=0xff;*(int32_t*)(t+0x84)=*sel;*(uint8_t*)(t+0x88)=prediction;
 memcpy(t+0xf0,pos,12);*(uint8_t*)(t+0x108)=0;actionStage=22;SendUseTransaction(c,actor,t);actionStage=23;}
// Directed use: type 3, click point = position + direction.
inline void UseItemToward(const Context& c,uintptr_t actor,uint64_t stack,const float* dir,bool prediction){int32_t* sel=ActionSelected(actor);float pos[3]{};if(!sel||!ActionPosition(actor,pos))return;
 uint8_t* t=NewUseTransaction(c,stack);*(int32_t*)(t+0x68)=3;*(int32_t*)(t+0x84)=*sel;*(uint8_t*)(t+0x88)=prediction;memcpy(t+0xf0,pos,12);
 const float click[3]={pos[0]+dir[0],pos[1]+dir[1],pos[2]+dir[2]};memcpy(t+0xfc,click,12);SendUseTransaction(c,actor,t);}
// Arm swing: actor->vtable[0x368](actor, kind, 0).
inline void ActionSwing(const Context& c,uintptr_t actor,int kind){const uint64_t f=Virtual0(c,actor,0x368);if(f)reinterpret_cast<void(*)(uintptr_t,int,int)>(f)(actor,kind,0);}

// ---- sequences
struct ActionCtx{const Context* c;uintptr_t actor;int32_t original;int32_t silent;int32_t* silentSlot;bool* silentHas;int32_t last;bool lastHas;};
using ActionStep=std::function<int(ActionCtx&)>;   // 0 wait, 1 next, 2 cancel
using ActionCondition=std::function<bool(uintptr_t,int64_t)>;   // (actor, sequence start in ns)
struct ActionSequence{int module=0;std::vector<ActionCondition> conditions;std::vector<ActionStep> steps;std::function<void(int)> done;int silent=0;bool rotation=false;};
struct ActionRecord{ActionSequence seq;uintptr_t actor=0;int64_t start=0;size_t index=0;int32_t original=0,silentSlot=0;bool silentHas=false,finished=false;int result=0;};
struct ActionPlanner{std::recursive_mutex guard;std::vector<ActionRecord> records;struct Recent{int32_t slot;int64_t at;};std::vector<Recent> recent;
 int64_t lastDone=0;bool hasLastDone=false,window=false,resync=false;std::atomic<bool> fault{false};};
inline ActionPlanner& Planner(){static auto* p=new ActionPlanner;return *p;}

// Hotbar slot switch.
inline void ActionSwitch(ActionCtx& x,int slot){x.last=slot;x.lastHas=true;
 if(x.silent==1){SendMobEquipment(*x.c,x.actor,slot);*x.silentSlot=slot;*x.silentHas=true;return;}
 // The game announces a slot change (MobEquipmentPacket) on its next tick, but a use can follow in the same frame:
 // servers that check the held slot (PowerNukkitX) then reject the use. The change is announced at once.
 if(int32_t* s=ActionSelected(x.actor)){const bool changed=*s!=slot;*s=slot;if(changed)SendMobEquipment(*x.c,x.actor,slot);}}
inline void ActionSilentBegin(ActionCtx& x){if(x.silent==1&&*x.silentHas)if(int32_t* s=ActionSelected(x.actor))*s=*x.silentSlot;}
inline void ActionSilentEnd(ActionCtx& x){if(x.silent==1)if(int32_t* s=ActionSelected(x.actor))*s=x.original;}
inline void ActionRestore(ActionCtx& x){if(int32_t* s=ActionSelected(x.actor))*s=x.original;
 if(x.silent==1&&*x.silentHas){SendMobEquipment(*x.c,x.actor,x.original);*x.silentHas=false;}}
// Slot search: the selected slot first, then the hotbar (0..min(size,9)-1).
inline bool ActionFind(const Context& c,uintptr_t actor,const std::function<bool(uint64_t)>& match,int* slot){int32_t* sel=ActionSelected(actor);const uint64_t box=ActionContainer(actor);if(!sel||!box)return false;
 if(match(ActionStackAt(c,box,*sel))){*slot=*sel;return true;}
 for(int i=0,n=ActionSize(c,box);i<(n>9?9:n);++i)if(match(ActionStackAt(c,box,i))){*slot=i;return true;}return false;}

inline ActionStep StepFindName(std::string name){return [name](ActionCtx& x){int slot{};if(!ActionFind(*x.c,x.actor,[&](uint64_t s){return ActionNameIs(s,name);},&slot))return 2;ActionSwitch(x,slot);return 1;};}
inline ActionStep StepFindPart(std::string part){return [part](ActionCtx& x){int slot{};if(!ActionFind(*x.c,x.actor,[&](uint64_t s){return ActionNameHas(s,part);},&slot))return 2;ActionSwitch(x,slot);return 1;};}
inline ActionStep StepFindOther(std::string name){return [name](ActionCtx& x){const uint64_t box=ActionContainer(x.actor);const int n=ActionSize(*x.c,box);
 for(int i=0;i<(n>9?9:n);++i){if(x.lastHas&&x.last==i)continue;if(ActionNameIs(ActionStackAt(*x.c,box,i),name)){ActionSwitch(x,i);return 1;}}return 2;};}
inline ActionStep StepWait(double ms){struct W{double ms;int64_t start=0;bool started=false;};auto w=std::make_shared<W>();w->ms=ms;
 return [w](ActionCtx&){if(!w->started){w->start=ActionNs();w->started=true;}const double e=ActionMs(w->start);return w->ms<=e&&e!=w->ms?1:0;};}
inline ActionStep StepSwitchBack(){return [](ActionCtx& x){ActionRestore(x);return 1;};}
// Use step. `rotateModule`: wait until that module's rotation request is reached before using.
// With a rotation, the use waits until a PlayerAuthInputPacket has carried the reached rotation: the server throws
// projectiles along the last rotation it received, and the use packet is sent at once while the input goes out
// on the next tick (a wind charge aimed down was thrown level). Two inputs: some servers (PowerNukkitX) apply a
// received input on their next tick, after packets that arrived in between.
inline ActionStep StepUse(int rotateModule=0){auto mark=std::make_shared<uint64_t>(0);auto marked=std::make_shared<bool>(false);return [rotateModule,mark,marked](ActionCtx& x){if(rotateModule){if(!RotReadyFor(rotateModule,0.01f)){*marked=false;return 0;}if(!*marked){*marked=true;*mark=rotAuthInputs.load();return 0;}if(rotAuthInputs.load()<*mark+2)return 0;}const Context& c=*x.c;actionStage=10;ActionSilentBegin(x);actionStage=11;const uint64_t stack=ActionHeld(c,x.actor);const uint64_t item=ActionItem(stack);
 const int32_t savedA=*(int32_t*)(x.actor+0x430);const uint8_t savedB=*(uint8_t*)(x.actor+0x45d);
 int kind=5;if(item){const uint64_t anim=Virtual0(c,item,0xa0);if(anim)kind=(int8_t)(reinterpret_cast<char(*)(uint64_t,int)>(anim)(item,5)+5);}
 actionStage=12;ActionSwing(c,x.actor,kind);actionStage=13;if(x.silent==1){*(int32_t*)(x.actor+0x430)=savedA;*(uint8_t*)(x.actor+0x45d)=savedB;}
 actionStage=14;UseItem(c,x.actor,stack,false);actionStage=15;ActionSilentEnd(x);return 1;};}
// Spear lunge: swing kind 4 unless silent, then a directed use along the look direction.
inline ActionStep StepSpearLunge(){return [](ActionCtx& x){const Context& c=*x.c;ActionSilentBegin(x);if(x.silent!=1)ActionSwing(c,x.actor,4);
 uint64_t rot{};float pitch{},yaw{};if(Read(x.actor+0x250,&rot)&&rot&&Read(rot,&pitch)&&Read(rot+4,&yaw)){const float p=pitch*0.017453292f,y=yaw*0.017453292f;
  const float dir[3]={-sinf(y)*cosf(p),-sinf(p),cosf(y)*cosf(p)};UseItemToward(c,x.actor,ActionHeld(c,x.actor),dir,false);}
 ActionSilentEnd(x);return 1;};}

// Predicate step: true -> next, false -> cancel.
inline ActionStep StepPredicate(std::function<bool(uintptr_t)> f){return [f](ActionCtx& x){return f(x.actor)?1:2;};}
// Wait until the module's rotation is reached within `tolerance` degrees.
inline ActionStep StepRotationReady(int module,float tolerance){return [module,tolerance](ActionCtx&){return RotReadyFor(module,tolerance)?1:0;};}
// ---- jump request: set here, consumed by the SprintIntentEvent hook on the next input update
struct JumpShared{std::atomic<bool> request{false};bool pressed=false;};
inline JumpShared& Jump(){static auto* j=new JumpShared;return *j;}
inline bool ActionOnGround(uintptr_t actor){uintptr_t registry{},ground{};uint32_t entity{},packed{};return ActorEntity(actor,&registry,&entity)&&Storage(registry,Fnv("OnGroundFlagComponent"),&ground)&&Packed(ground,entity,&packed);}
inline bool JumpRequest(uintptr_t actor){if(!ActionOnGround(actor))return false;Jump().request=true;return true;}
inline void JumpInputWork(uint64_t input,uint64_t state){Context c{};uint64_t mine{};if(!input||!Local(&c)||!Read(c.actor+0x240,&mine)||mine!=state)return;auto& j=Jump();
 const bool was=j.pressed;j.pressed=false;if(was&&!((*(uint32_t*)(input+0x10)>>7)&1)){*(uint8_t*)input&=0x7f;*(uint8_t*)(input+0x60)&=0xef;}
 if(j.request.exchange(false)){*(uint8_t*)input|=0x80;*(uint8_t*)(input+0x60)|=0x10;j.pressed=true;}}
inline void JumpInput(uint64_t input,uint64_t state){static volatile long stopped_=0;if(!stopped_)__try{JumpInputWork(input,state);}__except(game::FeatureFault("JumpInput",&stopped_,GetExceptionInformation())){}}
inline ActionStep StepJump(){return [](ActionCtx& x){return JumpRequest(x.actor)?1:2;};}
inline bool ActionHasItem(const Context& c,uintptr_t actor,const std::string& name){int slot{};return ActionFind(c,actor,[&](uint64_t s){return ActionNameIs(s,name);},&slot);}

// End of a sequence.
inline void ActionFinish(const Context* c,ActionRecord& r,int cancelled){uintptr_t local=c?c->actor:0;
 if(local&&local==r.actor&&(cancelled==1||r.silentHas)){if(int32_t* s=ActionSelected(local))*s=r.original;if(r.seq.silent==1&&r.silentHas){SendMobEquipment(*c,local,r.original);r.silentHas=false;}}
 if(r.seq.rotation&&RotEnabled(r.seq.module))RotRelease(r.seq.module);   // drop the module's rotation request
 r.result=cancelled;r.finished=true;}
// Cleanup: callbacks are invoked outside the lock.
inline void ActionCleanup(std::vector<std::pair<std::function<void(int)>,int>>* calls){auto& p=Planner();
 for(auto it=p.records.begin();it!=p.records.end();){if(!it->finished){++it;continue;}p.lastDone=ActionNs();p.hasLastDone=true;p.window=true;
  if(it->seq.done)calls->push_back({it->seq.done,it->result});it=p.records.erase(it);}}
inline void ActionCall(std::vector<std::pair<std::function<void(int)>,int>>& calls){for(auto& [f,r]:calls)f(r);}

// Submission; must be called on the game thread.
inline void ActionSubmit(ActionSequence seq){auto& p=Planner();std::vector<std::pair<std::function<void(int)>,int>> calls;{std::lock_guard<std::recursive_mutex> lock(p.guard);
 Context c{};const bool have=Local(&c);for(auto& r:p.records)if(r.seq.module==seq.module)ActionFinish(have?&c:nullptr,r,1);ActionCleanup(&calls);
 int32_t* sel=have?ActionSelected(c.actor):nullptr;
 if(!have||!sel){if(seq.done)calls.push_back({seq.done,1});}
 else{ActionRecord r;r.actor=c.actor;r.start=ActionNs();r.original=*sel;r.seq=std::move(seq);p.records.push_back(std::move(r));}}
 ActionCall(calls);}
// Cancels every sequence of a module.
inline void ActionCancel(int module){auto& p=Planner();std::vector<std::pair<std::function<void(int)>,int>> calls;{std::lock_guard<std::recursive_mutex> lock(p.guard);
 Context c{};const bool have=Local(&c);for(auto& r:p.records)if(r.seq.module==module)ActionFinish(have?&c:nullptr,r,1);ActionCleanup(&calls);}ActionCall(calls);}

// Per-frame execution.
inline void ActionFrameWork(){auto& p=Planner();std::vector<std::pair<std::function<void(int)>,int>> calls;{std::lock_guard<std::recursive_mutex> lock(p.guard);
 for(auto it=p.recent.begin();it!=p.recent.end();)it=ActionMs(it->at)>5000.0?p.recent.erase(it):it+1;
 Context c{};const bool have=Local(&c);
 if(!p.records.empty()){
  if(!have){for(auto& r:p.records)ActionFinish(nullptr,r,1);ActionCleanup(&calls);}
  else{for(auto& r:p.records){if(r.finished)continue;if(r.actor!=c.actor){ActionFinish(&c,r,1);continue;}
    bool ok=true;for(auto& cond:r.seq.conditions)if(!cond(r.actor,r.start)){ok=false;break;}if(!ok){ActionFinish(&c,r,1);continue;}
    while(r.index<r.seq.steps.size()){ActionCtx x{&c,r.actor,r.original,r.seq.silent,&r.silentSlot,&r.silentHas,0,false};
     const int status=r.seq.steps[r.index](x);if(x.lastHas)p.recent.push_back({x.last,ActionNs()});
     if(status==0)break;if(status==2){ActionFinish(&c,r,1);break;}++r.index;}
    if(!r.finished&&r.index==r.seq.steps.size())ActionFinish(&c,r,0);}
   ActionCleanup(&calls);}}
 if(p.resync){bool busy=false;for(auto& r:p.records)if(r.silentHas)busy=true;
  if(!busy){p.resync=false;if(have)if(int32_t* s=ActionSelected(c.actor))SendMobEquipment(c,c.actor,*s);}}}
 ActionCall(calls);}
inline volatile uint64_t actionFaultAt=0,actionFaultRead=0;
inline int ActionFilter(EXCEPTION_POINTERS* e){actionFaultAt=(uint64_t)e->ExceptionRecord->ExceptionAddress;actionFaultRead=e->ExceptionRecord->NumberParameters>1?e->ExceptionRecord->ExceptionInformation[1]:0;return EXCEPTION_EXECUTE_HANDLER;}
inline void ActionFrame(){if(Planner().fault)return;unsigned long code=0;__try{ActionFrameWork();}__except(code=GetExceptionCode(),ActionFilter(GetExceptionInformation())){Planner().fault=true;ActionFaultLog("frame",code);
 if(actionLog){uintptr_t b{};uint32_t z{};Supported(&b,&z);char l[128];snprintf(l,sizeof(l),"Actions: fault at %llx (game rva %llx), address %llx",(unsigned long long)actionFaultAt,(unsigned long long)(actionFaultAt-b),(unsigned long long)actionFaultRead);actionLog(l);}}}

// MobEquipmentPacket filter: returns true when the packet must be ignored.
inline bool ActionReceiveWork(uintptr_t packet){uint64_t table{},get{};if(!Read(packet,&table)||!table||!Read(table+8,&get)||!get||reinterpret_cast<int(*)(uintptr_t)>(get)(packet)!=0x1f)return false;
 if(*(uint8_t*)(packet+0xa0))return false;auto& p=Planner();std::lock_guard<std::recursive_mutex> lock(p.guard);
 if(p.records.empty()){if(!p.window)return false;if(p.hasLastDone&&ActionMs(p.lastDone)>5000.0){p.window=false;return false;}}
 Context c{};uint64_t id{};if(!Local(&c)||!RuntimeId(c.actor,&id)||*(uint64_t*)(packet+0x30)!=id)return false;
 const int32_t slot=*(int32_t*)(packet+0x9c);int32_t* sel=ActionSelected(c.actor);if(!sel)return false;
 if(slot!=*sel){auto it=p.recent.begin();for(;it!=p.recent.end();++it)if(it->slot==slot)break;if(it==p.recent.end())return false;p.recent.erase(it);p.resync=true;}
 return true;}
inline bool ActionReceive(uintptr_t packet){if(Planner().fault)return false;__try{return ActionReceiveWork(packet);}__except(EXCEPTION_EXECUTE_HANDLER){return false;}}

// ---- modules
inline float ActionRandom(float low,float high){if(!(low<high))return low;static std::mt19937_64 gen{std::random_device{}()};return low+(high-low)*(float)(gen()>>40)*5.9604645e-08f;}
struct ActionModules{std::mutex guard;
 struct Pot{bool request=false;bool silent=false,doublePot=false;int switchDelay=50,switchBack=0,doubleHealth=4;}pot;
 struct Spear{bool request=false;bool silent=false;float low=50,high=100;}spear;
 struct Wind{bool request=false;bool silent=false;float aim=1,low=50,high=100;int throwDelay=0;}wind;std::atomic<bool> windAiming{false};
 struct Abr{bool request=false;bool silentRotate=true,ghost=true;int switchBack=50,maxStack=1;}abr;std::atomic<bool> abrDone{false};
 struct Pearl{bool request=false;bool upward=true,silentAim=true,silentUse=false;float aim=1;int chargeDelay=0;}pearl;std::atomic<bool> pearlActive{false},pearlDone{false};float pearlTarget[2]{};
 std::atomic<bool> potDone{false},spearDone{false},windDone{false};std::atomic<unsigned> cancelMask{0};};
inline ActionModules& ActionMods(){static auto* m=new ActionModules;return *m;}

// ---- Auto Block Reset
// Limitations: the placement search has no line-of-sight check, and the input mode passed to the reach
// check is fixed to 1 (mouse). With Silent Rotate, the rotation request is persistent with tag 0 and only
// applies if type 0 is on the rotation stack when submitted (RotSubmitPersistent); otherwise the sequence
// waits until its 6000 ms timeout. Cursor busy: no item manager at *(actor+0xae0), a request pending at
// +0x68, or an item held in container 0x3b (screen context, lists at +0x108 then +0xc8).
inline bool ActionCursorBusy(const Context& c,uintptr_t actor){uint64_t mgr{},pending{};if(!Read(actor+0xae0,&mgr)||!mgr)return true;if(Read(mgr+0x68,&pending)&&pending)return true;
 const uint64_t screen=reinterpret_cast<uint64_t(*)(uint64_t)>(c.base+0x2b6c950)(mgr);if(!screen)return false;uint64_t box=0;
 for(uint32_t off:{0x108u,0xc8u}){uint64_t head{},node{};if(!Read(screen+off+8,&head)||!head||!Read(head,&node))continue;
  for(int guard=0;guard<4096&&node&&node!=head;++guard){uint8_t kind{};uint64_t model{},container{};if(Read(node+0x10,&kind)&&kind==0x3b&&Read(node+0x50,&model)&&model&&Read(model+8,&container)&&container){box=container;break;}if(!Read(node,&node))break;}
  if(box)break;}
 if(!box||!ActionSize(c,box))return false;const uint64_t stack=ActionStackAt(c,box,0);uint64_t block{},item{};return stack&&Read(stack+8,&block)&&block&&Read(block,&item)&&item;}
// Can act: region, level (+0x200), game mode (+0xac8), valid local player, health > 0, cursor free.
inline bool AbrCanAct(uintptr_t actor){Context c{};if(!Local(&c)||c.actor!=actor)return false;uint64_t level{},gm{};float h{},m{};
 return ActorRegion(c)&&Read(actor+0x200,&level)&&level&&Read(actor+0xac8,&gm)&&gm&&ReadHealth(&h,&m)&&h>0.f&&!ActionCursorBusy(c,actor);}
// Block flags: legacy block *(block+0x68), bytes at +0x28/+0x29.
inline bool BlockFlags(uint64_t block,uint8_t* b0,uint8_t* b1){uint64_t legacy{};return block&&Read(block+0x68,&legacy)&&legacy&&Read(legacy+0x28,b0)&&Read(legacy+0x29,b1);}
inline bool BlockReplaceable(uint64_t block){uint8_t a{},b{};return BlockFlags(block,&a,&b)&&(a==0||(b&2));}
inline bool BlockSolid(uint64_t block){uint8_t a{},b{};return BlockFlags(block,&a,&b)&&(b&0x17)==0x14;}
inline bool BlockStack(uint64_t stack){uint64_t block{};uint8_t count{};return stack&&Read(stack+0x18,&block)&&ActionItem(stack)&&Read(stack+0x22,&count)&&count&&block&&BlockSolid(block);}
struct AbrPlace{int32_t pos[3];int32_t nb[3];uint8_t face;float hit[3];bool ok;};
// Placement search (faces 0x3f): the cell must be outside the player's AABB (*(actor+0x248)) and
// replaceable; picks the first solid neighbour (order -y,+y,-z,+z,-x,+x) whose click point is within reach
// of the player position.
inline bool AbrFind(const Context& c,uintptr_t actor,const int32_t* pos,float reach,AbrPlace* out){out->ok=false;const uint64_t region=ActorRegion(c);if(!region)return false;
 uint64_t box{};float bb[6]{};if(Read(actor+0x248,&box)&&box)for(int i=0;i<6;++i)Read(box+i*4,&bb[i]);
 const float fx=(float)pos[0],fy=(float)pos[1],fz=(float)pos[2];
 if(fx<bb[3]&&fy<bb[4]&&fz<bb[5]&&!(fx+1.f<=bb[0])&&!(fy+1.f<=bb[1])&&bb[2]<fz+1.f)return false;
 float me[3]{};if(!ActionPosition(actor,me))return false;
 if(!BlockReplaceable(RegionBlock(c,region,pos)))return false;
 static const int dirs[6][3]={{0,-1,0},{0,1,0},{0,0,-1},{0,0,1},{-1,0,0},{1,0,0}};
 for(int i=0;i<6;++i){const int32_t nb[3]={pos[0]+dirs[i][0],pos[1]+dirs[i][1],pos[2]+dirs[i][2]};if(!BlockSolid(RegionBlock(c,region,nb)))continue;
  const bool vertical=i<2;const float hy=vertical?dirs[i][1]*0.5f+fy+0.5f:0.5f+fy,hx=dirs[i][0]*0.5f+fx+0.5f,hz=dirs[i][2]*0.5f+fz+0.5f;
  if(sqrtf((hz-me[2])*(hz-me[2])+(hy-me[1])*(hy-me[1])+(hx-me[0])*(hx-me[0]))>reach)continue;
  memcpy(out->pos,pos,12);memcpy(out->nb,nb,12);out->face=(uint8_t)(i^1);out->hit[0]=hx;out->hit[1]=hy;out->hit[2]=hz;out->ok=true;return true;}
 return false;}
// Placement: transaction of type 0 on the neighbour block.
inline bool AbrPlaceBlock(const Context& c,uintptr_t actor,const AbrPlace& p,float reach){int32_t* sel=ActionSelected(actor);if(!sel)return false;const int32_t slot=*sel;
 AbrPlace again{};if(!AbrFind(c,actor,p.pos,reach,&again)||memcmp(again.nb,p.nb,12)||again.face!=p.face||*sel!=slot)return false;
 const uint64_t stack=ActionHeld(c,actor);if(!BlockStack(stack))return false;float me[3]{};if(!ActionPosition(actor,me))return false;
 const uint64_t region=ActorRegion(c);const uint64_t block=region?RegionBlock(c,region,again.nb):0;int32_t runtime{};if(block)Read(block+0x114,&runtime);
 uint8_t* t=NewUseTransaction(c,stack);*(int32_t*)(t+0x68)=0;memcpy(t+0x70,again.nb,12);*(int32_t*)(t+0x7c)=runtime;*(int32_t*)(t+0x84)=slot;*(uint8_t*)(t+0x88)=0;*(uint8_t*)(t+0x80)=again.face;
 memcpy(t+0xf0,me,12);const float rel[3]={again.hit[0]-again.nb[0],again.hit[1]-again.nb[1],again.hit[2]-again.nb[2]};memcpy(t+0xfc,rel,12);SendUseTransaction(c,actor,t);return true;}
inline float AbrReach(const Context& c,uintptr_t actor){uint64_t gm{};if(!Read(actor+0xac8,&gm)||!gm)return 0.f;const uint64_t f=Virtual0(c,gm,0x50);if(!f)return 0.f;int mode=1;return reinterpret_cast<float(*)(uint64_t,int*)>(f)(gm,&mode);}
inline ActionStep StepFindBlock(int count){return [count](ActionCtx& x){int slot{};if(!ActionFind(*x.c,x.actor,[&](uint64_t s){uint8_t n{};return s&&Read(s+0x22,&n)&&n>=count&&BlockStack(s);},&slot))return 2;ActionSwitch(x,slot);return 1;};}
inline ActionStep StepAbrRotate(bool rotate){return [rotate](ActionCtx& x){if(!rotate)return 1;
 if(!RotHas(kRotAutoBlockReset)){float p{},y{};if(RotRead(x.actor,&p,&y)){RotRequest r;r.module=kRotAutoBlockReset;r.yaw=y;r.pitch=90.f;r.mode=2;r.priority=3;r.predicate=[](uintptr_t a){return AbrCanAct(a);};RotSubmitPersistent(std::move(r),0);}}
 return RotReadyFor(kRotAutoBlockReset,0.01f)?1:0;};}
inline ActionStep StepAbrPlace(AbrPlace place,float reach,bool rotate){return [place,reach,rotate](ActionCtx& x){if(rotate&&!RotReadyFor(kRotAutoBlockReset,0.01f))return 0;
 ActionSilentBegin(x);AbrPlace p{};if(!AbrFind(*x.c,x.actor,place.pos,reach,&p)){ActionSilentEnd(x);return 2;}if(x.silent!=1)ActionSwing(*x.c,x.actor,1);
 const bool ok=AbrPlaceBlock(*x.c,x.actor,p,reach);ActionSilentEnd(x);return ok?1:2;};}

enum ActionModuleId{kThrowPot=1,kSpearDash=2,kAutoBlockReset=3,kWindCharge=4,kPearlCatch=5,kScaffold=7};
inline void ActionCancelRequest(int module){ActionMods().cancelMask|=1u<<module;}   // module turned off on the menu thread; handled on the game thread

// Throw Pot
inline void ThrowPotStart(const ActionModules::Pot& s){ActionSequence q;q.module=kThrowPot;q.silent=s.silent?1:0;
 q.steps.push_back(StepFindName("splash_potion"));if(s.switchDelay>0)q.steps.push_back(StepWait((double)s.switchDelay));q.steps.push_back(StepUse());
 if(s.doublePot){float health{},maximum{};if(ReadHealth(&health,&maximum)&&health<=(float)s.doubleHealth){q.steps.push_back(StepFindOther("splash_potion"));
   if(s.switchDelay>0)q.steps.push_back(StepWait((double)s.switchDelay));q.steps.push_back(StepUse());}}
 if(s.switchBack>0)q.steps.push_back(StepWait((double)s.switchBack));q.steps.push_back(StepSwitchBack());
 q.done=[](int){ActionMods().potDone=true;};ActionSubmit(std::move(q));}
// Spear Dash
inline void SpearDashStart(const ActionModules::Spear& s){ActionSequence q;q.module=kSpearDash;q.silent=s.silent?1:0;
 q.steps.push_back(StepFindPart("spear"));q.steps.push_back(StepSpearLunge());const float delay=ActionRandom(s.low,s.high);
 if(delay>0)q.steps.push_back(StepWait((double)delay));q.steps.push_back(StepSwitchBack());
 q.done=[](int){ActionMods().spearDone=true;};ActionSubmit(std::move(q));}

// Wind Charge: requires being on the ground with a charge in the hotbar; aims down every tick.
// Sequence: predicate (on ground + charge), rotation reached within 1 deg, find, jump, Throw Delay, use
// (rotation within 0.01 deg), Switch Back Delay, switch back. On finish the module turns itself off, which
// clears the aim.
inline void WindChargeStart(const ActionModules::Wind& s){auto& m=ActionMods();Context c{};
 if(!Local(&c)||!ActionOnGround(c.actor)||!ActionHasItem(c,c.actor,"wind_charge")){RotOnce(6,"Wind Charge: not on ground or no charge, stopped");m.windDone=true;return;}RotOnce(7,"Wind Charge: sequence submitted");
 m.windAiming=true;ActionSequence q;q.module=kWindCharge;q.silent=s.silent?1:0;
 q.steps.push_back(StepPredicate([](uintptr_t a){Context k{};return ActionOnGround(a)&&Client(&k)&&ActionHasItem(k,a,"wind_charge");}));
 q.steps.push_back(StepRotationReady(kWindCharge,1.f));q.steps.push_back(StepFindName("wind_charge"));q.steps.push_back(StepJump());
 if(s.throwDelay>0)q.steps.push_back(StepWait((double)s.throwDelay));q.rotation=true;q.steps.push_back(StepUse(kWindCharge));
 const float back=ActionRandom(s.low,s.high);if(back>0)q.steps.push_back(StepWait((double)back));q.steps.push_back(StepSwitchBack());
 q.done=[](int){ActionMods().windDone=true;};ActionSubmit(std::move(q));}
inline void WindChargeTickWork(){auto& m=ActionMods();if(!m.windAiming)return;Context c{};if(!Local(&c))return;float p{},y{};if(!RotRead(c.actor,&p,&y))return;
 RotRequest r;r.module=kRotWindCharge;r.yaw=y;r.pitch=90.f;r.mode=2;r.priority=3;float aim{};{std::lock_guard<std::mutex> lock(m.guard);aim=m.wind.aim;}r.maxStep=aim*90.f;r.hasMax=true;
 r.predicate=[](uintptr_t){return ActionMods().windAiming.load();};RotSubmit(std::move(r));}
inline void WindChargeTick(){if(ActionMods().windAiming)RotOnce(4,"Wind Charge: aiming tick");__try{WindChargeTickWork();}__except(EXCEPTION_EXECUTE_HANDLER){RotOnce(5,"Wind Charge: fault in aiming tick");}}

// Pearl Catch: target = current look (Upward: pitch -90). Sequence: rotation reached, pearl, Charge Delay x
// 50 ms, rotation reached, wind charge, aim released, switch back. Aim every tick: mode 2 (Silent Aim) or
// 0, priority 3, step Aim Speed x 90.
inline void PearlCatchStart(const ActionModules::Pearl& s){auto& m=ActionMods();Context c{};
 if(!Local(&c)||!ActionHasItem(c,c.actor,"ender_pearl")||!ActionHasItem(c,c.actor,"wind_charge")){RotOnce(8,"Pearl Catch: no player, pearl or wind charge, stopped");m.pearlDone=true;return;}
 float p{},y{};if(!RotRead(c.actor,&p,&y)){m.pearlDone=true;return;}m.pearlTarget[0]=y;m.pearlTarget[1]=s.upward?-90.f:p;m.pearlActive=true;
 ActionSequence q;q.module=kPearlCatch;q.silent=s.silentUse?1:0;q.rotation=true;
 q.steps.push_back(StepRotationReady(kPearlCatch,0.01f));q.steps.push_back(StepFindName("ender_pearl"));q.steps.push_back(StepUse(kPearlCatch));
 if(s.chargeDelay>0)q.steps.push_back(StepWait(s.chargeDelay*50.0));
 q.steps.push_back(StepRotationReady(kPearlCatch,0.01f));q.steps.push_back(StepFindName("wind_charge"));q.steps.push_back(StepUse(kPearlCatch));
 q.steps.push_back([](ActionCtx&){ActionMods().pearlActive=false;RotRelease(kPearlCatch);return 1;});q.steps.push_back(StepSwitchBack());
 q.done=[](int){ActionMods().pearlDone=true;};ActionSubmit(std::move(q));}
inline void PearlCatchTickWork(){auto& m=ActionMods();if(!m.pearlActive)return;Context c{};if(!Local(&c))return;bool silent{};float aim{};{std::lock_guard<std::mutex> lock(m.guard);silent=m.pearl.silentAim;aim=m.pearl.aim;}
 RotRequest r;r.module=kRotPearlCatch;r.yaw=m.pearlTarget[0];r.pitch=m.pearlTarget[1];r.mode=silent?2:0;r.priority=3;r.maxStep=aim*90.f;r.hasMax=true;
 r.predicate=[](uintptr_t){return ActionMods().pearlActive.load();};RotSubmit(std::move(r));}
inline void PearlCatchTick(){static volatile long stopped_=0;if(!stopped_)__try{PearlCatchTickWork();}__except(game::FeatureFault("PearlCatchTick",&stopped_,GetExceptionInformation())){}}

// Auto Block Reset: feet = RenderPositionComponent - OffsetsComponent[0]. For n = Max Stack down to 1, the
// cell at y-n must be placeable and the cells up to y-1 replaceable. Sequence: 6000 ms timeout, can-act
// predicate, enough blocks, rotation (Silent Rotate), one placement per cell, Switch Back Delay, switch
// back, 50 ms with Silent Rotate. Ghost Place = silent switch. On finish the module turns itself off.
inline void AutoBlockResetStart(const ActionModules::Abr& s){auto& m=ActionMods();Context c{};
 if(!Local(&c)||!AbrCanAct(c.actor)){RotOnce(9,"Auto Block Reset: player cannot act, stopped");m.abrDone=true;return;}
 uintptr_t render{},offsets{};float feet[3]{},lift{};if(!Element(c.registry,c.entity,"RenderPositionComponent",12,&render)||!Read(render,&feet[0])||!Read(render+4,&feet[1])||!Read(render+8,&feet[2])||!Element(c.registry,c.entity,"OffsetsComponent",0x44,&offsets)||!Read(offsets,&lift)){m.abrDone=true;return;}
 feet[1]-=lift;const float reach=AbrReach(c,c.actor);const int x=(int)floorf(feet[0]),y=(int)floorf(feet[1]),z=(int)floorf(feet[2]);const uint64_t region=ActorRegion(c);
 std::vector<AbrPlace> cells;
 for(int n=s.maxStack;n>=1&&cells.empty();--n){const int32_t start[3]={x,y-n,z};AbrPlace first{};if(!AbrFind(c,c.actor,start,reach,&first))continue;
  bool clear=true;for(int k=y-n;k<y;++k){const int32_t at[3]={x,k,z};if(!BlockReplaceable(RegionBlock(c,region,at))){clear=false;break;}AbrPlace cell{};memcpy(cell.pos,at,12);cells.push_back(cell);}
  if(!clear)cells.clear();}
 if(cells.empty()){if(actionLog){const int32_t b1[3]={x,y-1,z},b2[3]={x,y-2,z};uint8_t f1a{},f1b{},f2a{},f2b{};BlockFlags(RegionBlock(c,region,b1),&f1a,&f1b);BlockFlags(RegionBlock(c,region,b2),&f2a,&f2b);char l[200];snprintf(l,sizeof(l),"Auto Block Reset: no place below (reach %.2f, feet %.2f %.2f %.2f, below %02x/%02x, next %02x/%02x)",reach,feet[0],feet[1],feet[2],f1a,f1b,f2a,f2b);actionLog(l);}m.abrDone=true;return;}
 ActionSequence q;q.module=kAutoBlockReset;q.silent=s.ghost?1:0;q.rotation=s.silentRotate;
 q.conditions.push_back([](uintptr_t,int64_t start){const double e=ActionMs(start);return e<6000.0||e==6000.0;});
 q.steps.push_back(StepPredicate([](uintptr_t a){return AbrCanAct(a);}));q.steps.push_back(StepFindBlock((int)cells.size()));q.steps.push_back(StepAbrRotate(s.silentRotate));
 for(auto& cell:cells)q.steps.push_back(StepAbrPlace(cell,reach,s.silentRotate));
 if(s.switchBack>0)q.steps.push_back(StepWait((double)s.switchBack));q.steps.push_back(StepSwitchBack());if(s.silentRotate)q.steps.push_back(StepWait(50.0));
 RotOnce(11,"Auto Block Reset: sequence submitted");q.done=[](int){ActionMods().abrDone=true;};ActionSubmit(std::move(q));}


// ---- Nuker (level tick): around the feet block (position - OffsetsComponent[0], floored), x and z in
// [-Radius, Radius], y in [-Radius, Radius] (Y Limit: [-Y Radius, Y Radius]; Horizontal Only:
// [min(Height,-1)+1, max(Height,1)-1]). The block currently being mined (actor+0xb4a,
// gamemode+0x10/+0x14/+0x18) is skipped. Filter: legacy flag +0x29 & 0x10, then whitelist and blacklist by
// name. Keeps the "Breaks Per Tick" closest blocks (integer squared distance), sets the game mode progress
// (+0x20/+0x24) to 1 and calls gamemode->vtable[0x10](gamemode, &pos, face) with the face toward the
// player; the swing is still sent when No Swing is on.
// Mode On Break (Nickel): nothing happens until the player breaks a block. The game's destroy function (game mode
// vtable +0x10, the call Nuker itself makes) is hooked; a destroy that is not Nuker's own marks that block, and the area
// around it is nuked for 10 ticks with the same radius, height and lists (instead of around the feet).
struct NukerShared{std::mutex guard;bool enabled=false,noSwing=false,white=false,black=false,horizontal=false,yLimit=false,onBreak=false;int radius=3,perTick=8,height=1,yRadius=3;
 std::atomic<bool> broken{false};std::atomic<int32_t> brokenAt[3]{};int32_t center[3]{};int pending=0;std::atomic<bool> nuking{false};
 std::vector<std::string> whiteList,blackList;std::atomic<bool> fault{false};};
inline NukerShared& Nuker(){static auto* n=new NukerShared;return *n;}
inline bool NukerAllowed(uint64_t block,const NukerShared& s){uint8_t a{},b{};if(!BlockFlags(block,&a,&b)||!(b&0x10))return false;std::string name;XrayBlockName(block,&name);
 if(s.white&&std::find(s.whiteList.begin(),s.whiteList.end(),name)==s.whiteList.end())return false;
 if(s.black&&std::find(s.blackList.begin(),s.blackList.end(),name)!=s.blackList.end())return false;return true;}
using NukerDestroyFn=bool(*)(uint64_t,int32_t*,uint8_t);inline NukerDestroyFn nukerDestroyOriginal=nullptr;
inline bool NukerDestroyDetour(uint64_t gm,int32_t* pos,uint8_t face){auto& n=Nuker();
 if(pos&&!n.nuking.load()){n.brokenAt[0]=pos[0];n.brokenAt[1]=pos[1];n.brokenAt[2]=pos[2];n.broken=true;}
 return nukerDestroyOriginal(gm,pos,face);}
inline void NukerHookDestroy(const Context& c,uint64_t gm){static int state=0;if(state)return;state=-1;const uint64_t fn=Virtual0(c,gm,0x10);if(!fn)return;
 if(MH_CreateHook((void*)fn,(void*)NukerDestroyDetour,(void**)&nukerDestroyOriginal)==MH_OK&&MH_EnableHook((void*)fn)==MH_OK){state=1;if(actionLog)actionLog("Nuker: destroy hook installed (On Break)");}
 else if(actionLog)actionLog("Nuker: destroy hook could not be installed, On Break unavailable");}
inline void NukerWork(){auto& n=Nuker();NukerShared s;{std::lock_guard<std::mutex> lock(n.guard);if(!n.enabled)return;s.noSwing=n.noSwing;s.white=n.white;s.black=n.black;s.horizontal=n.horizontal;s.yLimit=n.yLimit;
  s.radius=n.radius;s.perTick=n.perTick;s.height=n.height;s.yRadius=n.yRadius;s.whiteList=n.whiteList;s.blackList=n.blackList;s.onBreak=n.onBreak;}
 Context c{};if(!Local(&c))return;uint64_t gm{},state{};if(!Read(c.actor+0xac8,&gm)||!gm)return;const uint64_t region=ActorRegion(c);if(!region||!Read(c.actor+0x240,&state)||!state)return;
 float pos[3]{};for(int i=0;i<3;++i)if(!Read(state+i*4,&pos[i]))return;float lift=0;uintptr_t off{};if(Element(c.registry,c.entity,"OffsetsComponent",0x44,&off))Read(off,&lift);
 int fx=(int)floorf(pos[0]),fy=(int)floorf(pos[1]-lift),fz=(int)floorf(pos[2]);
 int low,high;if(!s.horizontal){high=s.yLimit?s.yRadius:s.radius;low=-high;}else{low=(s.height<-1?s.height:-1)+1;high=(s.height>1?s.height:1)-1;}
 uint8_t breaking{};Read(c.actor+0xb4a,&breaking);int32_t current[3]{};if(breaking==1){Read(gm+0x10,&current[0]);Read(gm+0x14,&current[1]);Read(gm+0x18,&current[2]);}
 if(s.onBreak){NukerHookDestroy(c,gm);if(n.broken.exchange(false)){n.center[0]=n.brokenAt[0];n.center[1]=n.brokenAt[1];n.center[2]=n.brokenAt[2];n.pending=10;}
  if(n.pending<=0)return;--n.pending;fx=n.center[0];fy=n.center[1];fz=n.center[2];}
 struct Cand{int32_t p[3];int d;};std::vector<Cand> best;const size_t cap=s.perTick>0?(size_t)s.perTick:0;if(!cap)return;
 auto worse=[](const Cand& a,const Cand& b){return a.d<b.d;};
 for(int dx=-s.radius;dx<=s.radius;++dx)for(int dy=low;dy<=high;++dy)for(int dz=-s.radius;dz<=s.radius;++dz){const int32_t p[3]={fx+dx,fy+dy,fz+dz};
  if(breaking==1&&p[0]==current[0]&&p[1]==current[1]&&p[2]==current[2])continue;if(!NukerAllowed(RegionBlock(c,region,p),s))continue;
  const int d=dx*dx+dy*dy+dz*dz;if(best.size()<cap){best.push_back({{p[0],p[1],p[2]},d});std::push_heap(best.begin(),best.end(),worse);}
  else if(d<best.front().d){std::pop_heap(best.begin(),best.end(),worse);best.back()={{p[0],p[1],p[2]},d};std::push_heap(best.begin(),best.end(),worse);}}
 std::sort(best.begin(),best.end(),[](const Cand& a,const Cand& b){return a.d<b.d;});if(best.empty())return;
 const uint64_t destroy=Virtual0(c,gm,0x10);if(!destroy)return;
 const float savedA=*(float*)(gm+0x24),savedB=*(float*)(gm+0x20);const int32_t swingA=*(int32_t*)(c.actor+0x430);const uint8_t swingB=*(uint8_t*)(c.actor+0x45d);
 *(float*)(gm+0x20)=1.f;*(float*)(gm+0x24)=1.f;
 n.nuking=true;
 for(auto& b:best){if(!NukerAllowed(RegionBlock(c,region,b.p),s))continue;const float dy=pos[1]-((float)b.p[1]+0.5f),dx=pos[0]-((float)b.p[0]+0.5f),dz=pos[2]-((float)b.p[2]+0.5f);
  const float ax=fabsf(dx),ay=fabsf(dy),az=fabsf(dz);uint8_t face;if(ax>=ay&&ax>=az)face=(uint8_t)((dx>0)|4);else if(ay>=az)face=(uint8_t)(dy>0);else face=(uint8_t)((dz>0)|2);
  int32_t at[3]={b.p[0],b.p[1],b.p[2]};reinterpret_cast<void(*)(uint64_t,int32_t*,uint8_t)>(destroy)(gm,at,face);}
 n.nuking=false;
 *(float*)(gm+0x24)=savedA;*(float*)(gm+0x20)=savedB;if(s.noSwing){*(int32_t*)(c.actor+0x430)=swingA;*(uint8_t*)(c.actor+0x45d)=swingB;}}
inline void NukerTick(){if(Nuker().fault)return;__try{NukerWork();}__except(EXCEPTION_EXECUTE_HANDLER){Nuker().fault=true;if(actionLog)actionLog("Nuker: fault caught, module stopped until restart");}}


// ---- Scaffold (level tick, GameUpdate and disable). State: locked Y, last placement, "placed" flag,
// running sequence.
// - Y: without Lock Y, without a stored Y, on ground or feet <= stored Y -> Y = feet - 1.
// - Delay between sequences. Each placement round walks 2*Extend+1 cells in 0.5 steps along the horizontal
// velocity (look direction if <= 0.01) and takes the first replaceable, placeable cell (no line-of-sight
// check); otherwise searches a corner within Range around each cell, inside the previous-cell/cell box,
// closest with 1 <= d^2 <= Range^2.
// - Sequence: total timeout = cells x delay + 2000 ms; per cell: block (>= 1), Switch Delay (without Ghost
// Place), reach check, placement without rotation; then "placed" and switch back. Ghost Place = silent
// switch.
struct ScaffoldShared{std::mutex guard;bool enabled=false,lockY=false,ghost=true;int extend=1,placements=1,range=1,delay=0,switchDelay=50;
 int32_t y=0;bool hasY=false;int64_t placedAt=0;bool hasPlaced=false,placed=false;std::atomic<bool> active{false},fault{false};};
inline ScaffoldShared& Scaffold(){static auto* s=new ScaffoldShared;return *s;}
inline ActionStep StepScaffoldCheck(AbrPlace cell,float reach){return [cell,reach](ActionCtx& x){const float r=std::min(AbrReach(*x.c,x.actor),reach);AbrPlace p{};return AbrFind(*x.c,x.actor,cell.pos,r,&p)?1:2;};}
inline void ScaffoldWork(){auto& s=Scaffold();ScaffoldShared cfg;{std::lock_guard<std::mutex> lock(s.guard);if(!s.enabled)return;cfg.lockY=s.lockY;cfg.ghost=s.ghost;cfg.extend=s.extend;cfg.placements=s.placements;cfg.range=s.range;cfg.delay=s.delay;cfg.switchDelay=s.switchDelay;}
 Context c{};if(!Local(&c))return;uint64_t st{};float pos[6]{},lift=0;if(!Read(c.actor+0x240,&st)||!st)return;for(int i=0;i<6;++i)Read(st+0x18*(i/3)+(i%3)*4,&pos[i]);
 uintptr_t off{};if(Element(c.registry,c.entity,"OffsetsComponent",0x44,&off))Read(off,&lift);const int feetY=(int)floorf(pos[1]-lift);
 if(!cfg.lockY||!s.hasY||ActionOnGround(c.actor)||feetY<=s.y){s.y=feetY-1;s.hasY=true;}
 if(s.active)return;if(s.placed&&!s.hasPlaced)return;if(s.placed&&ActionMs(s.placedAt)<=(double)cfg.delay)return;
 const float reach=AbrReach(c,c.actor);const uint64_t region=ActorRegion(c);if(!region)return;
 float pitch{},yaw{};RotRead(c.actor,&pitch,&yaw);const float speed=sqrtf(pos[3]*pos[3]+pos[5]*pos[5]);const float yr=yaw*0.017453292f;
 const float dirx=speed>0.01f?pos[3]/speed:-sinf(yr),dirz=speed>0.01f?pos[5]/speed:cosf(yr);
 std::vector<AbrPlace> chosen;auto taken=[&](const int32_t* p){for(auto& q:chosen)if(!memcmp(q.pos,p,12))return true;return false;};
 for(int round=0;round<cfg.placements;++round){std::vector<std::array<int32_t,3>> lane;
  if(cfg.extend>=0)for(int k=0;k<=cfg.extend*2;++k){const std::array<int32_t,3> p={(int32_t)floorf(k*0.5f*dirx+pos[0]),s.y,(int32_t)floorf(k*0.5f*dirz+pos[2])};if(lane.empty()||lane.back()!=p)lane.push_back(p);}
  AbrPlace found{};bool ok=false;
  for(auto& p:lane){if(taken(p.data()))continue;if(!BlockReplaceable(RegionBlock(c,region,p.data())))continue;if(AbrFind(c,c.actor,p.data(),reach,&found)){ok=true;break;}}
  if(!ok&&chosen.empty()&&cfg.range>=0){const int r=cfg.range;
   for(size_t k=1;k<lane.size()&&!ok;++k){const auto& C=lane[k];const auto& P=lane[k-1];if(!BlockReplaceable(RegionBlock(c,region,C.data())))continue;
    const int minx=std::min(C[0],P[0]),maxx=std::max(C[0],P[0]),minz=std::min(C[2],P[2]),maxz=std::max(C[2],P[2]);int bestD=0x7fffffff;AbrPlace best{};bool any=false;
    for(int i=-r;i<=r;++i)for(int j=-r;j<=r;++j){const int d=i*i+j*j;if((unsigned)(d-1)>=(unsigned)(r*r)||d>=bestD)continue;const int32_t q[3]={C[0]+i,C[1],C[2]+j};
     if(q[0]==P[0]&&P[1]==C[1]&&q[2]==P[2])continue;if(q[0]<minx||q[0]>maxx||q[2]<minz||q[2]>maxz)continue;AbrPlace t{};if(AbrFind(c,c.actor,q,reach,&t)){best=t;bestD=d;any=true;}}
    if(any){found=best;ok=true;}}}
  {static int told=0;if(told<3&&actionLog){++told;char l[240];int n=0;n+=snprintf(l+n,sizeof(l)-n,"Scaffold diag: y %d feet %.2f lane",s.y,pos[1]-lift);for(auto& p:lane){uint8_t a{},b{};BlockFlags(RegionBlock(c,region,p.data()),&a,&b);n+=snprintf(l+n,sizeof(l)-n," (%d,%d,%d %02x/%02x)",p[0],p[1],p[2],a,b);if(n>200)break;}snprintf(l+n,sizeof(l)-n," ok %d reach %.2f",(int)ok,reach);actionLog(l);}}
  if(!ok)break;chosen.push_back(found);}
 if(chosen.empty())return;const double delay=cfg.ghost?0.0:(double)cfg.switchDelay;s.active=true;
 ActionSequence q;q.module=kScaffold;q.silent=cfg.ghost?1:0;const double limit=(double)chosen.size()*delay+2000.0;
 q.conditions.push_back([limit](uintptr_t,int64_t start){const double e=ActionMs(start);return e<limit||e==limit;});
 for(auto& cell:chosen){q.steps.push_back(StepFindBlock(1));if(delay>0.0)q.steps.push_back(StepWait(delay));q.steps.push_back(StepScaffoldCheck(cell,reach));q.steps.push_back(StepAbrPlace(cell,reach,false));}
 q.steps.push_back([](ActionCtx&){auto& sc=Scaffold();sc.placed=false;sc.placedAt=ActionNs();sc.hasPlaced=true;sc.placed=true;return 1;});q.steps.push_back(StepSwitchBack());
 q.done=[](int){Scaffold().active=false;};ActionSubmit(std::move(q));}
inline void ScaffoldTick(){if(Scaffold().fault)return;__try{ScaffoldWork();}__except(EXCEPTION_EXECUTE_HANDLER){Scaffold().fault=true;if(actionLog)actionLog("Scaffold: fault caught, module stopped until restart");}}

// Single subscriber of the screen render hook (game_render.hpp): menu activations first, then sequence
// execution.
inline void(*volatile actionCombatFrame)()=nullptr;   // Combat modules that submit sequences (Auto Anchor, ...)
inline void ActionGameUpdateWork(){auto& m=ActionMods();ActionModules::Pot pot{};ActionModules::Spear spear{};bool doPot=false,doSpear=false;
 ActionModules::Wind wind{};bool doWind=false;ActionModules::Pearl pearl{};bool doPearl=false;ActionModules::Abr abr{};bool doAbr=false;
 {std::lock_guard<std::mutex> lock(m.guard);if(m.pot.request){m.pot.request=false;pot=m.pot;doPot=true;}if(m.spear.request){m.spear.request=false;spear=m.spear;doSpear=true;}if(m.wind.request){m.wind.request=false;wind=m.wind;doWind=true;}if(m.pearl.request){m.pearl.request=false;pearl=m.pearl;doPearl=true;}if(m.abr.request){m.abr.request=false;abr=m.abr;doAbr=true;}}
 for(unsigned bits=m.cancelMask.exchange(0),i=0;bits;bits>>=1,++i)if(bits&1)ActionCancel((int)i);
 if(doPot)ThrowPotStart(pot);if(doSpear)SpearDashStart(spear);if(doWind)WindChargeStart(wind);if(doPearl)PearlCatchStart(pearl);if(doAbr)AutoBlockResetStart(abr);if(auto f=actionCombatFrame)f();ActionFrame();}
inline void ActionGameUpdate(){if(Planner().fault)return;unsigned long code=0;__try{ActionGameUpdateWork();}__except(code=GetExceptionCode(),EXCEPTION_EXECUTE_HANDLER){Planner().fault=true;ActionFaultLog("update",code);}}
}
