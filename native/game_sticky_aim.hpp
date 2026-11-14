// Sticky Aim: lowers the mouse sensitivity while the crosshair is on an entity.
// - GameUpdate: with a local player whose external input data is present and an entity under the crosshair (only the
//   last attacked one with Target Only), the sensitivity of the current input mode is saved once and set to
//   saved x (1 - Reduction / 100). Otherwise the saved value is put back.
// - The sensitivity lives in the option layer read by Aim Assist (key = input mode).
// - Disabling puts the saved value back and forgets the last target.
#pragma once
#include "game_aim_assist.hpp"
#include "game_events.hpp"
#include <atomic>

namespace game {
struct StickyAimShared{std::atomic<bool> enabled{false},targetOnly{false},resetPending{false};std::atomic<int> reduction{40};
 float saved=0;bool applied=false;uint32_t mode=0;uint64_t serial=0;volatile long fault=0;};   // game thread only below the atomics
inline StickyAimShared& StickyAim(){static auto* s=new StickyAimShared;return *s;}

// Option layer of type 3 (the one holding the sensitivities), as walked by AimSensitivity.
inline uint64_t StickyLayer(const Context& c){const uint64_t options=GameOptions(c);uint64_t node{},inner{},next{};int32_t kind{};if(!options||!Read(options+0xc78,&node))return 0;
 for(int guard=0;;++guard){if(!node||guard>16||!Read(node+8,&inner)||!inner)return 0;if(!Read(inner+0x238,&next))return 0;if(!next)break;node=next;}
 return Read(inner+0x1d4,&kind)&&kind==3?node:0;}
// Hash-map entry of the layer for a key (FNV-1a over the 4 key bytes); 0 when missing.
inline uint64_t StickyEntry(uint64_t layer,uint32_t key){uint64_t mask{},buckets{},end{},at{},first{};if(!Read(layer+0x48,&mask)||!Read(layer+0x30,&buckets)||!Read(layer+0x20,&end)||!buckets)return 0;
 uint64_t hash=0xcbf29ce484222325ull;for(int i=0;i<4;++i)hash=(hash^((key>>(8*i))&0xff))*0x100000001b3ull;const uint64_t slot=buckets+(hash&mask)*16;
 if(!Read(slot+8,&at)||at==end||!Read(slot,&first))return 0;
 for(int guard=0;guard<64;++guard){uint32_t found{};if(!Read(at+0x10,&found))return 0;if(found==key)return at==end?0:at;if(at==first)return 0;if(!Read(at+8,&at)||!at)return 0;}
 return 0;}
inline float StickyGet(uint64_t layer,uint32_t key){const uint64_t e=StickyEntry(layer,key);float v{};return e&&Read(e+0x14,&v)?v:0.0f;}
inline void StickySet(uint64_t layer,uint32_t key,float value){if(const uint64_t e=StickyEntry(layer,key))*(float*)(e+0x14)=value;}

// External input data present and its object reports true (virtual +8), as the original checks before acting.
inline bool StickyHasInput(const Context& c){uint64_t owner{},buckets{},end{},nodes{},last{};if(!Read(c.actor+8,&owner)||!owner||!Read(owner+0x30,&buckets)||!Read(owner+0x38,&end)||!Read(owner+0x50,&nodes)||!Read(owner+0x58,&last)||end<=buckets||!nodes)return false;
 const uint32_t key=0xa8a30f19u;uint64_t cell=buckets+(uint64_t)(((uint32_t)((end-buckets)>>3)-1u)&key)*8;
 for(int i=0;i<4096;++i){int64_t index{};if(!Read(cell,&index)||index==-1||index<0||index>1048576)return false;const uint64_t node=nodes+(uint64_t)index*0x30;uint32_t id{};if(!Read(node+8,&id))return false;
  if(id==key){uint64_t value{},object{};uint8_t state{};int32_t tag{};if(node==last||!Read(node+0x10,&value)||!value||!Read(node+0x2c,&state)||state==4||!Read(node+0x28,&tag)||tag!=(int32_t)key||!Read(value,&object)||!object)return false;
   const uint64_t f=Virtual0(c,object,8);return f&&reinterpret_cast<bool(*)(uint64_t)>(f)(object);}
  cell=node;}
 return false;}

inline void StickyRestore(StickyAimShared& s,const Context* c){if(!s.applied)return;Context own{};if(!c){if(!Client(&own))return;c=&own;}
 if(const uint64_t layer=StickyLayer(*c))StickySet(layer,s.mode,s.saved);s.applied=false;}
inline void StickyApply(StickyAimShared& s,const Context& c){if(s.applied)return;const int mode=AimInputMode(c);s.mode=(uint32_t)mode;if(mode==0||mode==-1)return;
 const uint64_t layer=StickyLayer(c);if(!layer)return;s.saved=StickyGet(layer,s.mode);s.applied=true;
 StickySet(layer,s.mode,s.saved*(1.0f-(float)s.reduction.load()/100.0f));}

inline void StickyAimWork(){auto& s=StickyAim();
 if(s.resetPending.exchange(false)){StickyRestore(s,nullptr);s.serial=Shared().attackSerial;}   // enable or disable: put the value back, forget the target
 if(!s.enabled)return;
 Context c{};if(!Local(&c)||!StickyHasInput(c)){StickyRestore(s,nullptr);return;}
 const uintptr_t actor=CrosshairActor(c);if(!actor){StickyRestore(s,&c);return;}
 if(s.targetOnly){uint64_t id{},last{},serial{};{auto& e=Shared();std::lock_guard<std::mutex> lock(e.guard);last=e.attackTarget;serial=e.attackSerial;}
  if(!RuntimeId(actor,&id)||serial==s.serial||id!=last){StickyRestore(s,&c);return;}}
 StickyApply(s,c);}
inline void StickyAimTick(){auto& s=StickyAim();if(s.fault)return;__try{StickyAimWork();}__except(EXCEPTION_EXECUTE_HANDLER){InterlockedExchange(&s.fault,1);}}
}
