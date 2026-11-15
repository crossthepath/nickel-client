// Hitbox: enlarges entity hitboxes for the game's entity pick and can draw the enlarged boxes.
// - Pick: whole-function hook on the entity pick (RVA 0x3d281b0, returns the HitResult through its first argument;
//   arguments 5 and 6 are the ray start and end). After the game has filled the result, every actor that passes the
//   common target filter (local player excluded, living, Players Only, Anti Bot, Teams) is tested: its box
//   *(actor+0x248) grown by Width / 2 on x and z and Height / 2 on y is intersected with the ray (slab test, t in
//   [0, 1]); the smallest entry t wins. The result is replaced by that actor when it holds no entity, or when the new
//   point is closer to the ray start than the entity point already there. The written result is an entity hit
//   (type 1): ray start, ray direction, weak entity reference, entity id, the actor's real box, and the hit point
//   clamped into that real box.
// - Render: for the same filtered actors, the render box (RenderPosition, minus the height offset, with the AABB
//   size) grown the same way is drawn as 12 depth-tested lines of thickness 1.5 in the Color setting.
#pragma once
#include "game_combat.hpp"
#include "game_world.hpp"
#include <MinHook.h>
#include <windows.h>
#include <atomic>
#include <cstring>
#include <cfloat>
#include <cmath>
#include <mutex>
#include <vector>

namespace game {
struct HitboxConfig{bool enabled=false;float width=0.4f,height=0.4f;bool playersOnly=true,render=false;float color[4]={1,1,1,1};};
struct HitboxShared{std::mutex guard;HitboxConfig cfg;volatile long fault=0;void(*log)(const char*)=nullptr;};
inline HitboxShared& Hitbox(){static auto* s=new HitboxShared;return *s;}
inline HitboxConfig HitboxSnapshot(){auto& s=Hitbox();std::lock_guard<std::mutex> lock(s.guard);return s.cfg;}

// Ray / box slab test over the segment start -> end; writes the entry fraction and returns true on a hit.
inline bool HitboxRay(const float* box,const float* start,const float* end,float* t){
 const float eps=9.999999974752427e-07f;float tnear,tfar;
 const float dx=end[0]-start[0];
 if(std::fabs(dx)>=eps){const float a=(box[0]-start[0])/dx,b=(box[3]-start[0])/dx;const float hi=a<b?b:a,lo=b<a?b:a;
  tnear=0.0f<lo?lo:0.0f;tfar=hi<1.0f?hi:1.0f;if(!(tnear<=tfar))return false;}
 else if(box[0]<=start[0]&&start[0]<=box[3]){tnear=0.0f;tfar=1.0f;}
 else return false;
 const float dy=end[1]-start[1];
 if(std::fabs(dy)>=eps){const float a=(box[1]-start[1])/dy,b=(box[4]-start[1])/dy;const float lo=b<a?b:a,hi=a<b?b:a;
  tfar=hi<tfar?hi:tfar;tnear=tnear<lo?lo:tnear;if(!(tnear<=tfar))return false;}
 else if(!(box[1]<=start[1]&&start[1]<=box[4]))return false;
 const float dz=end[2]-start[2];
 if(std::fabs(dz)>=eps){const float a=(box[2]-start[2])/dz,b=(box[5]-start[2])/dz;float lo=a<=b?a:b;lo=lo<=tnear?tnear:lo;float hi=b<=a?a:b;hi=tfar<=hi?tfar:hi;
  tnear=lo;if(!(lo<=hi))return false;}
 else if(!(box[2]<=start[2]&&start[2]<=box[5]))return false;
 *t=tnear;return true;
}

// Actors of the ActorOwnerComponent storage, walked from the last packed entity to the first.
template<class F> inline void HitboxEachActor(const Context& c,F&& f){uintptr_t owners{};uint64_t begin{},end{};if(!Storage(c.registry,Fnv("ActorOwnerComponent"),&owners)||!Read(owners+0x20,&begin)||!Read(owners+0x28,&end)||end<begin||(end-begin)%4||(end-begin)>4*65536)return;
 for(uint64_t at=end;at>begin;){at-=4;uint32_t entity{};uintptr_t slot{};uint64_t actor{};if(!Read(at,&entity))break;if(entity>=0xfffc0000u)continue;if(!InStorage(owners,entity,8,&slot)||!Read(slot,&actor)||!actor)continue;f((uintptr_t)actor);}}

// MSVC shared_ptr control block: vtable (destroy, delete this), strong count, weak count.
struct HitboxRefCount{void** table;volatile long uses;volatile long weaks;};
inline void HitboxDropWeak(HitboxRefCount* ref){if(ref&&InterlockedDecrement(&ref->weaks)==0)reinterpret_cast<void(*)(void*)>(ref->table[1])(ref);}
inline void HitboxDropStrong(HitboxRefCount* ref){if(ref&&InterlockedDecrement(&ref->uses)==0){reinterpret_cast<void(*)(void*)>(ref->table[0])(ref);HitboxDropWeak(ref);}}
inline bool HitboxLock(HitboxRefCount* ref){if(!ref)return false;long count=ref->uses;while(count){const long seen=InterlockedCompareExchange(&ref->uses,count+1,count);if(seen==count)return true;count=seen;}return false;}

// True when the result is an entity hit whose weak reference still resolves to an actor.
inline bool HitboxResultActor(uintptr_t hit){int32_t kind{};uint64_t holder{},ref{};uint32_t entity{};uintptr_t owners{},slot{};uint64_t actor{};long uses{};
 if(!Read(hit+0x18,&kind)||kind!=1||!Read(hit+0x40,&ref)||!ref||!Read(ref+8,&uses)||!uses||!Read(hit+0x38,&holder)||!holder||!Read(hit+0x48,&entity))return false;
 return Storage(holder+0x30,Fnv("ActorOwnerComponent"),&owners)&&InStorage(owners,entity,8,&slot)&&Read(slot,&actor)&&actor;}

inline void HitboxWrite(uintptr_t hit,uintptr_t best,const float* b,const float* p,const float* start,const float* end);
// Replaces the game's pick result with the closest enlarged hitbox crossed by the ray.
inline void HitboxPickWork(uintptr_t hit,const float* start,const float* end){
 const HitboxConfig k=HitboxSnapshot();if(!k.enabled)return;Context c{};if(!Local(&c))return;
 const float grow[3]={k.width*0.5f,k.height*0.5f,k.width*0.5f};
 auto grown=[&](const float* b,float* e){e[0]=b[0]-grow[0];e[1]=b[1]-grow[1];e[2]=b[2]-grow[2];e[3]=b[3]+grow[0];e[4]=b[4]+grow[1];e[5]=b[5]+grow[2];};
 uintptr_t best=0;float bestT=FLT_MAX;
 HitboxEachActor(c,[&](uintptr_t actor){if(!CombatTargetValid(c,actor,k.playersOnly,true,true,true))return;uint64_t bb{};float b[6],e[6],t{};if(!Read(actor+0x248,&bb)||!bb||!Floats(bb,b,6))return;
  grown(b,e);const float value=HitboxRay(e,start,end,&t)?t:FLT_MAX;if(value<bestT){bestT=value;best=actor;}});
 if(!best)return;
 uint64_t bb{};float b[6],e[6],t=0;if(!Read(best+0x248,&bb)||!bb||!Floats(bb,b,6))return;grown(b,e);HitboxRay(e,start,end,&t);
 const float u=1.0f-t;const float p[3]={u*start[0]+t*end[0],u*start[1]+t*end[1],start[2]*u+end[2]*t};
 if(HitboxResultActor(hit)){float old[3];if(!Floats(hit+0x2c,old,3))return;const float nx=p[0]-start[0],ny=p[1]-start[1],nz=p[2]-start[2],ox=old[0]-start[0],oy=old[1]-start[1],oz=old[2]-start[2];
  if(!(std::sqrt(ny*ny+nx*nx+nz*nz)<std::sqrt(oz*oz+oy*oy+ox*ox)))return;}
 HitboxWrite(hit,best,b,p,start,end);}
// Writes an entity hit on `actor` into the result: ray, weak entity reference, entity id, real box, point clamped into it.
inline void HitboxWrite(uintptr_t hit,uintptr_t best,const float* b,const float* p,const float* start,const float* end){
 // weak entity reference: weak_ptr from the registry's weak self reference (*(actor+8)), entity id at actor+0x18
 uint64_t registry{},owner{},control{};uint32_t entity{};if(!Read(best+8,&registry)||!registry||!Read(registry,&owner)||!Read(registry+8,&control)||!Read(best+0x18,&entity))return;
 auto* ref=reinterpret_cast<HitboxRefCount*>(control);if(!HitboxLock(ref))return;
 float* out=reinterpret_cast<float*>(hit);
 out[0]=start[0];out[1]=start[1];out[2]=start[2];out[3]=end[0]-start[0];out[4]=end[1]-start[1];out[5]=end[2]-start[2];*reinterpret_cast<int32_t*>(hit+0x18)=1;
 InterlockedIncrement(&ref->weaks);HitboxDropStrong(ref);
 auto* old=*reinterpret_cast<HitboxRefCount**>(hit+0x40);*reinterpret_cast<uint64_t*>(hit+0x38)=owner;*reinterpret_cast<HitboxRefCount**>(hit+0x40)=ref;HitboxDropWeak(old);
 *reinterpret_cast<uint32_t*>(hit+0x48)=entity;
 memcpy(reinterpret_cast<void*>(hit+0x50),b,24);
 const float x=b[0]>p[0]?b[0]:p[0],y=b[1]>p[1]?b[1]:p[1];out[11]=b[3]<x?b[3]:x;out[12]=b[4]<y?b[4]:y;
 const float z=b[2]<=p[2]?p[2]:b[2];out[13]=z<=b[5]?z:b[5];
}

// Piercing: when the pick result is not an entity (nothing on the ray), the closest actor accepted by the common filter
// whose box grown by 0.1 on every side is crossed by the whole ray becomes the result.
// Through blocks: the entity pick ignores blocks, and the game keeps its entity only when it is closer than the block
// under the crosshair (RVA 0x4e0a736: ucomiss against the block distance, then jbe). Replacing results that held no
// entity, as the original module did, therefore never reached an entity behind a block on this game version. While
// Piercing is on, that jump is removed (6-byte nop, bytes verified first, put back when it is turned off), and a pick
// result whose entity fails the target filter is cleared (kind 3) when the block under the crosshair is closer, so
// only accepted entities are hit through blocks.
struct PiercingShared{std::atomic<bool> enabled{false},playersOnly{true};volatile long fault=0;};
inline PiercingShared& Piercing(){static auto* s=new PiercingShared;return *s;}
inline constexpr uint32_t kPiercingJump=0x4e0a73a;
inline const uint8_t kPiercingContext[10]={0x44,0x0f,0x2e,0xc0,0x0f,0x86,0xea,0x02,0x00,0x00};   // ucomiss xmm8,xmm0 / jbe +0x2ea
inline const uint8_t kPiercingNop[6]={0x66,0x0f,0x1f,0x44,0x00,0x00};
inline std::atomic<int> piercingPatched{-1};
// Applies (true) or removes (false) the through-blocks patch. Returns false when the game code is not the expected one.
inline bool PiercingPatch(bool on,void(*log)(const char*)){static int checked=0;uintptr_t base{};uint32_t size{};if(!Supported(&base,&size))return false;const uintptr_t at=base+Rva(kPiercingJump);
 if(!checked){uint8_t now[10]{};bool readable=true;for(int i=0;i<10;++i)if(!Read(at-4+i,&now[i])){readable=false;break;}
  const bool game=readable&&memcmp(now,kPiercingContext,10)==0;const bool patched=readable&&memcmp(now,kPiercingContext,4)==0&&memcmp(now+4,kPiercingNop,6)==0;
  checked=game||patched?1:-1;if(checked<0&&log)log("Piercing: unexpected game code at the block/entity choice, through-blocks unavailable");}
 if(checked<0)return false;if(piercingPatched.load()==(int)on)return true;
 DWORD old{};if(!VirtualProtect((void*)at,6,PAGE_EXECUTE_READWRITE,&old))return false;memcpy((void*)at,on?kPiercingNop:kPiercingContext+4,6);DWORD unused{};VirtualProtect((void*)at,6,old,&unused);
 FlushInstructionCache(GetCurrentProcess(),(void*)at,6);piercingPatched=(int)on;if(log)log(on?"Piercing: entities behind blocks can be picked":"Piercing: block/entity choice restored");return true;}
// Actor of an entity hit result (weak reference holder at +0x38, entity id at +0x48), like CrosshairActor.
inline uintptr_t HitResultActor(uintptr_t hit){uint64_t holder{},actor{};uint32_t entity{};uintptr_t owners{},slot{};
 if(!Read(hit+0x38,&holder)||!holder||!Read(hit+0x48,&entity))return 0;
 if(!Storage(holder+0x30,Fnv("ActorOwnerComponent"),&owners)||!InStorage(owners,entity,8,&slot)||!Read(slot,&actor))return 0;return (uintptr_t)actor;}
inline void PiercingWork(uintptr_t hit,const float* start,const float* end){auto& s=Piercing();if(!s.enabled)return;int32_t kind{};if(!Read(hit+0x18,&kind))return;
 Context c{};if(!Local(&c))return;const bool playersOnly=s.playersOnly;
 if(kind==1){if(piercingPatched.load()!=1)return;   // an entity behind a block: keep it only if the filter accepts it
  const uintptr_t actor=HitResultActor(hit);if(actor&&CombatTargetValid(c,actor,playersOnly,true,true,true))return;
  uint64_t level{};int32_t lk=-1;float bp[3]{},ep[3]{};if(!CrosshairHit(c,&level,&lk)||lk!=0||!Floats(level+0x2c,bp,3)||!Floats(hit+0x2c,ep,3))return;
  auto d2=[&](const float* q){const float x=q[0]-start[0],y=q[1]-start[1],z=q[2]-start[2];return x*x+y*y+z*z;};
  if(d2(bp)<d2(ep))*reinterpret_cast<int32_t*>(hit+0x18)=3;
  return;}
 auto grown=[](const float* b,float* e){e[0]=b[0]-0.1f;e[1]=b[1]-0.1f;e[2]=b[2]-0.1f;e[3]=b[3]+0.1f;e[4]=b[4]+0.1f;e[5]=b[5]+0.1f;};
 uintptr_t best=0;float bestT=FLT_MAX;
 HitboxEachActor(c,[&](uintptr_t actor){if(!CombatTargetValid(c,actor,playersOnly,true,true,true))return;uint64_t bb{};float b[6],e[6],t{};if(!Read(actor+0x248,&bb)||!bb||!Floats(bb,b,6))return;
  grown(b,e);const float value=HitboxRay(e,start,end,&t)?t:FLT_MAX;if(value<bestT){bestT=value;best=actor;}});
 if(!best)return;
 uint64_t bb{};float b[6],e[6],t=0;if(!Read(best+0x248,&bb)||!bb||!Floats(bb,b,6))return;grown(b,e);HitboxRay(e,start,end,&t);
 const float u=1.0f-t;const float p[3]={u*start[0]+t*end[0],u*start[1]+t*end[1],start[2]*u+end[2]*t};
 HitboxWrite(hit,best,b,p,start,end);}
inline void HitboxPickGuarded(uintptr_t hit,const float* start,const float* end){auto& s=Hitbox();if(!s.fault){__try{HitboxPickWork(hit,start,end);}__except(EXCEPTION_EXECUTE_HANDLER){InterlockedExchange(&s.fault,1);if(s.log)s.log("Hitbox: a fault was caught in the entity pick; Hitbox is disabled until the game restarts");}}
 auto& pc=Piercing();if(pc.fault)return;__try{PiercingWork(hit,start,end);}__except(EXCEPTION_EXECUTE_HANDLER){InterlockedExchange(&pc.fault,1);if(s.log)s.log("Piercing: a fault was caught in the entity pick; Piercing is disabled until the game restarts");}}

using HitboxPickFunction=void*(*)(void*,void*,void*,void*,const float*,const float*);inline HitboxPickFunction hitboxPickOriginal=nullptr;
inline void* HitboxPickDetour(void* out,void* a,void* b,void* c,const float* start,const float* end){
 hitboxPickOriginal(out,a,b,c,start,end);if(out&&start&&end)HitboxPickGuarded(reinterpret_cast<uintptr_t>(out),start,end);return out;}
// Installs the entity pick hook after checking the function's first 24 bytes.
inline int InstallHitboxHook(void(*log)(const char*)){static int state=0;if(state)return state;Hitbox().log=log;
 const uint8_t bytes[24]={0x41,0x57,0x41,0x56,0x41,0x55,0x41,0x54,0x56,0x57,0x53,0x48,0x81,0xec,0x90,0x00,0x00,0x00,0x4c,0x89,0xcf,0x4c,0x89,0xc3};
 state=InstallOne(0x3d281b0,bytes,24,(void*)HitboxPickDetour,(void**)&hitboxPickOriginal,"Hitbox (entity pick)",log);return state;}

// Render option: 12 edges per enlarged render box, padded like the game's box outline (distance * thickness / 1000).
inline void HitboxLinesWork(const float* camera,std::vector<WorldLine>* out){
 const HitboxConfig k=HitboxSnapshot();if(!k.enabled||!k.render)return;Context c{};if(!Local(&c))return;
 const float grow[3]={k.width*0.5f,k.height*0.5f,k.width*0.5f},thickness=1.5f;
 HitboxEachActor(c,[&](uintptr_t actor){if(!CombatTargetValid(c,actor,k.playersOnly,true,true,true))return;
  uintptr_t registry{},element{};uint32_t entity{};uint64_t bb{};float pos[3],b[6],lift=0;if(!ActorEntity(actor,&registry,&entity)||!Element(registry,entity,"RenderPositionComponent",12,&element)||!Floats(element,pos,3))return;
  if(Element(registry,entity,"OffsetsComponent",0x44,&element))Floats(element,&lift,1);if(!Read(actor+0x248,&bb)||!bb||!Floats(bb,b,6))return;
  const float halfX=(b[3]-b[0])*0.5f,halfZ=(b[5]-b[2])*0.5f,y=pos[1]-lift;
  float lo[3]={pos[0]-halfX-grow[0],y-grow[1],pos[2]-halfZ-grow[2]},hi[3]={halfX+pos[0]+0.0f+grow[0],(b[4]-b[1])+y+0.0f+grow[1],halfZ+pos[2]+0.0f+grow[2]};
  const float cx=(lo[0]+hi[0])*0.5f-camera[0],cy=(lo[1]+hi[1])*0.5f-camera[1],cz=(lo[2]+hi[2])*0.5f-camera[2];float pad=std::sqrt(cz*cz+cy*cy+cx*cx)*thickness*0.001f;if(pad<=0.001f)pad=0.001f;
  for(int i=0;i<3;++i){lo[i]-=pad;hi[i]+=pad;}
  static const int pick[8][3]={{0,0,0},{1,0,0},{1,0,1},{0,0,1},{0,1,0},{1,1,0},{1,1,1},{0,1,1}};static const int edges[12][2]={{0,1},{1,2},{2,3},{3,0},{4,5},{5,6},{6,7},{7,4},{0,4},{1,5},{2,6},{3,7}};
  for(auto& edge:edges){WorldLine line{};for(int j=0;j<3;++j){line.a[j]=pick[edge[0]][j]?hi[j]:lo[j];line.b[j]=pick[edge[1]][j]?hi[j]:lo[j];}memcpy(line.colorA,k.color,16);memcpy(line.colorB,k.color,16);line.thickness=thickness;line.depth=true;out->push_back(line);}});
}
inline bool HitboxLines(const float* camera,std::vector<WorldLine>& out){__try{HitboxLinesWork(camera,&out);return true;}__except(EXCEPTION_EXECUTE_HANDLER){return false;}}
}
