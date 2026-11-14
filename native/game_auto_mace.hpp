// Auto Mace: while falling, switches to a mace and hits a nearby target with it.
// - State: last attacked target, current target, original slot (saved once), pending slot switch, target carried by
//   the last PlayerAuthInputPacket, last attack time, last switch time.
// - Attack (game attack event): remembers the attacked runtime id; with Target Only it also becomes the current target.
// - Screen render: without a local player whose external input data is present, the original slot is put back and
//   forgotten. A pending switch older than 500 ms is dropped. Mace = held item named "mace", else (unless Only Held)
//   the first "mace" of the selected slot then the hotbar. Target: LockOn keeps the current target while it passes
//   the common filter (Players Only, Anti Bot, Teams) and stays within Target Range; otherwise Retarget picks the actor
//   whose eye-to-box squared distance is the smallest within Target Range (Target Only: only the last attacked one).
//   Fall check: FallDistanceComponent > 1.5 and >= Minimum Fall Distance. When the mace is not selected, the original
//   slot is saved, the mace slot is selected (the game sends the MobEquipmentPacket) and the frame ends. When it is,
//   after Attack Cooldown since the last attack and within Attack Range: without Silent Rotations, swing (4) if Swing
//   and attack transaction at once; with Silent Rotations, a relative rotation request (mode 2, priority 2) towards
//   the eye clamped into the target box shrunk by 35 % per side (eye = position + velocity), whose "reached" callback
//   attacks only when the last PlayerAuthInputPacket already carried this module's rotation for the same target.
//   Any failed condition clears the target and puts the original slot back.
// - UpdatePlayerFromCamera (Silent Rotations): the rotation request is renewed (without attack) while the target is
//   valid and the fall check holds; an invalid target is dropped.
// - Packet receive: the server's MobEquipmentPacket (0x1f, container 0) echoing the pending switch of the local player
//   is cancelled once.
// - Packet send: on PlayerAuthInputPacket (0x90), the current target is recorded when the rotation manager's applied
//   request is this module's, cleared otherwise.
// - Enable / disable: everything forgotten, both times set to now; disabling puts the original slot back first.
#pragma once
#include "game_combat.hpp"
#include "game_sticky_aim.hpp"
#include "game_rotation.hpp"
#include "game_actions.hpp"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <mutex>

namespace game {
constexpr int kRotAutoMace=16;
struct AutoMaceShared{
 // settings (menu thread)
 std::atomic<bool> enabled{false},targetOnly{false},silent{true},onlyHeld{false},swing{true},playersOnly{true};
 std::atomic<float> attackRange{3.0f},targetRange{8.0f},minFall{2.0f},cooldown{1.0f};std::atomic<int> targetMode{0};   // 0 LockOn, 1 Retarget
 std::atomic<bool> enablePending{false},disablePending{false};
 // state (game threads, under guard)
 std::mutex guard;
 uint64_t attacked=0;bool hasAttacked=false;   // last attacked runtime id
 uint64_t target=0;bool hasTarget=false;       // current target runtime id
 int32_t original=0;bool hasOriginal=false;    // slot before the switch
 int32_t pending=0;bool hasPending=false;      // switch waiting for the server echo
 uint64_t sentTarget=0;bool hasSentTarget=false;   // target of the last PlayerAuthInputPacket under this module's rotation
 std::atomic<int64_t> lastAttack{0};std::atomic<bool> hasLastAttack{false};
 int64_t lastSwitch=0;bool hasLastSwitch=false;
 volatile long fault=0;};
inline AutoMaceShared& AutoMace(){static auto* s=new AutoMaceShared;return *s;}

inline bool AutoMaceFall(uintptr_t actor,float* fall){uintptr_t registry{},storage{},element{};uint32_t entity{};   // FallDistanceComponent (0xce6b34f6), 8-byte element
 return ActorEntity(actor,&registry,&entity)&&Storage(registry,0xce6b34f6u,&storage)&&InStorage(storage,entity,8,&element)&&Read(element,fall);}
inline bool AutoMaceFalling(uintptr_t actor){float fall{};return AutoMaceFall(actor,&fall)&&fall>1.5f&&AutoMace().minFall.load()<=fall;}
inline bool AutoMaceHolding(const Context& c,uintptr_t actor){return ActionNameIs(ActionHeld(c,actor),"mace");}
// Squared distance from the eye (feet + 1.62001) to the nearest point of the target box, when the target passes the
// common filter and lies within range.
inline bool AutoMaceDistance(const Context& c,uintptr_t target,float range,float* sq){float lo[3],hi[3],feet[3];
 if(!CombatTargetValid(c,target,AutoMace().playersOnly.load()))return false;if(!ActorFeet(c.actor,feet)||!ActorBox(target,lo,hi))return false;
 const float eye[3]={feet[0]+0.0f,feet[1]+1.6200100183486938f,feet[2]+0.0f};float d=0;for(int i=0;i<3;++i){const float q=std::min(hi[i],std::max(eye[i],lo[i]))-eye[i];d+=q*q;}
 if(range*range<d)return false;*sq=d;return true;}
inline uintptr_t AutoMaceResolve(const Context& c,uint64_t id){uintptr_t a{};return ActorByRuntimeId(c.registry,id,&a)?a:0;}
inline void AutoMaceRestore(AutoMaceShared& s,uintptr_t actor){if(s.hasOriginal)if(int32_t* sel=ActionSelected(actor))*sel=s.original;}
inline void AutoMaceForget(AutoMaceShared& s){const int64_t now=CombatNs();s.hasAttacked=false;s.hasTarget=false;s.hasOriginal=false;s.hasPending=false;s.hasSentTarget=false;
 s.lastAttack=now;s.hasLastAttack=true;s.lastSwitch=now;s.hasLastSwitch=true;}

// Attack: swing (4) if Swing, attack transaction, attack time.
inline void AutoMaceHit(const Context& c,uintptr_t target){auto& s=AutoMace();if(s.swing)ActionSwing(c,c.actor,4);AttackActor(c,target);s.lastAttack=CombatNs();s.hasLastAttack=true;}
// Relative rotation request towards the shrunk target box; the attack callback only when attack is true.
inline void AutoMaceRotate(const Context& c,uintptr_t target,bool attack){float lo[3],hi[3];uint64_t st{},rot{},id{};if(!ActorBox(target,lo,hi))return;
 if(!Read(c.actor+0x240,&st)||!st||!Read(c.actor+0x250,&rot)||!rot)return;float sv[9],cur[2];if(!Floats(st,sv,9)||!Floats(rot,cur,2))return;
 const float eye[3]={sv[0]+sv[6],sv[1]+sv[7],sv[2]+sv[8]};float p[3];
 for(int i=0;i<3;++i){const float m=(hi[i]-lo[i])*0.3499999940395355f;const float a=lo[i]+m,b=hi[i]-m;p[i]=std::min(b,std::max(a,eye[i]));}   // shrunk box, clamped eye
 const float dx=p[0]-eye[0],dy=p[1]-eye[1],dz=p[2]-eye[2];const float pitch=atan2f(dy,std::hypot(dx,dz));const float yaw=atan2f(-dx,dz);
 if(!RuntimeId(target,&id))return;
 RotRequest r;r.module=kRotAutoMace;r.relative=true;r.yaw=remainderf(yaw*57.295780181884766f-cur[1],360.0f);r.pitch=pitch*-57.295780181884766f-cur[0];r.mode=2;r.priority=2;
 r.predicate=[id](uintptr_t actor){Context c{};if(!Local(&c)||c.actor!=actor)return false;const uintptr_t t=AutoMaceResolve(c,id);float sq{};   // still falling, holding the mace, target within Target Range
  return t&&AutoMaceFalling(actor)&&AutoMaceHolding(c,actor)&&AutoMaceDistance(c,t,AutoMace().targetRange.load(),&sq);};
 if(attack)r.reached=[id](uintptr_t){Context c{};if(!Local(&c))return;const uintptr_t t=AutoMaceResolve(c,id);float sq{};if(t&&AutoMaceDistance(c,t,AutoMace().attackRange.load(),&sq))AutoMaceHit(c,t);};
 RotSubmit(std::move(r));}

// ---- events
inline void AutoMaceAttackWork(uintptr_t target){auto& s=AutoMace();if(!s.enabled)return;uint64_t id{};const bool has=RuntimeId(target,&id);
 std::lock_guard<std::mutex> lock(s.guard);s.attacked=id;s.hasAttacked=has;if(s.targetOnly){s.target=s.attacked;s.hasTarget=s.hasAttacked;}}

inline void AutoMaceScreenWork(){auto& s=AutoMace();Context c{};const bool local=Local(&c);
 if(s.disablePending.exchange(false)){std::lock_guard<std::mutex> lock(s.guard);if(local)AutoMaceRestore(s,c.actor);AutoMaceForget(s);RotRelease(kRotAutoMace);}
 if(s.enablePending.exchange(false)){std::lock_guard<std::mutex> lock(s.guard);AutoMaceForget(s);}
 if(!s.enabled)return;
 uintptr_t target=0;bool silent=false,ready=false;
 {std::lock_guard<std::mutex> lock(s.guard);
  if(!local){s.hasOriginal=false;return;}
  if(!StickyHasInput(c)){AutoMaceRestore(s,c.actor);s.hasOriginal=false;return;}
  if(s.hasPending&&s.hasLastSwitch&&500.0<(double)(CombatNs()-s.lastSwitch)/1000000.0)s.hasPending=false;
  int32_t* sel=ActionSelected(c.actor);
  auto reset=[&](){s.hasTarget=false;s.hasSentTarget=false;AutoMaceRestore(s,c.actor);s.hasOriginal=false;};
  int mace{};bool found=false;
  if(AutoMaceHolding(c,c.actor)&&sel){mace=*sel;found=true;}
  else if(!s.onlyHeld)found=ActionFind(c,c.actor,[&](uint64_t st){return ActionNameIs(st,"mace");},&mace);
  if(!found||!sel){reset();return;}
  const bool playersOnly=s.playersOnly;const float targetRange=s.targetRange;float sq{};
  uintptr_t locked=0;
  if(s.targetMode==0&&s.hasTarget)locked=AutoMaceResolve(c,s.target);
  if(locked&&CombatTargetValid(c,locked,playersOnly)){if(!AutoMaceDistance(c,locked,targetRange,&sq)){reset();return;}target=locked;}   // LockOn
  else{s.hasTarget=false;   // Retarget
   if(!s.targetOnly){uintptr_t best=0;float bestSq=3.4028234663852886e+38f;
    ForEachActor(c,[&](uintptr_t a){float d{};if(CombatTargetValid(c,a,playersOnly)&&AutoMaceDistance(c,a,targetRange,&d)&&d<bestSq){bestSq=d;best=a;}return true;});
    uint64_t id{};if(!best||!RuntimeId(best,&id)){reset();return;}s.target=id;s.hasTarget=true;target=best;}
   else{const uintptr_t a=s.hasAttacked?AutoMaceResolve(c,s.attacked):0;uint64_t id{};
    if(!a||!AutoMaceDistance(c,a,targetRange,&sq)||!RuntimeId(a,&id)){reset();return;}s.target=id;s.hasTarget=true;target=a;}}
  if(!AutoMaceFalling(c.actor)){reset();return;}
  if(mace!=*sel){   // switch to the mace; the attack waits for the next frame
   if(!s.hasOriginal){s.original=*sel;s.hasOriginal=true;}s.hasSentTarget=false;s.pending=mace;s.hasPending=true;s.lastSwitch=CombatNs();s.hasLastSwitch=true;*sel=mace;return;}
  if(!s.hasLastAttack)return;
  if((double)(CombatNs()-s.lastAttack.load())/1000000.0<=(double)s.cooldown.load()*1000.0)return;
  if(!AutoMaceDistance(c,target,s.attackRange,&sq))return;
  silent=s.silent;uint64_t id{};if(silent)ready=s.hasSentTarget&&RuntimeId(target,&id)&&s.sentTarget==id;}
 if(silent){AutoMaceRotate(c,target,ready);return;}   // rotation request outside the module lock
 AutoMaceHit(c,target);}

inline void AutoMaceCameraWork(){auto& s=AutoMace();if(!s.enabled||!s.silent)return;Context c{};if(!Local(&c))return;uintptr_t target=0;
 {std::lock_guard<std::mutex> lock(s.guard);if(!s.hasTarget||!AutoMaceFalling(c.actor))return;target=AutoMaceResolve(c,s.target);
  if(!target||!CombatTargetValid(c,target,s.playersOnly)){s.hasTarget=false;return;}}
 AutoMaceRotate(c,target,false);}

// MobEquipmentPacket echo of the pending switch: true cancels it.
inline bool AutoMaceReceiveWork(uintptr_t packet){auto& s=AutoMace();if(!s.enabled)return false;uint64_t table{},get{};
 {std::lock_guard<std::mutex> lock(s.guard);if(!s.hasPending)return false;}
 if(!Read(packet,&table)||!table||!Read(table+8,&get)||!get||reinterpret_cast<int(*)(uintptr_t)>(get)(packet)!=0x1f)return false;
 if(*(uint8_t*)(packet+0xa0))return false;Context c{};uint64_t id{};if(!Local(&c)||!RuntimeId(c.actor,&id))return false;
 std::lock_guard<std::mutex> lock(s.guard);if(!s.hasPending||*(uint64_t*)(packet+0x30)!=id||*(int32_t*)(packet+0x9c)!=s.pending)return false;s.hasPending=false;return true;}

inline void AutoMaceSendWork(uintptr_t packet){auto& s=AutoMace();if(!s.enabled)return;uint64_t table{},get{};
 {std::lock_guard<std::mutex> lock(s.guard);if(!s.hasTarget)return;}
 if(!Read(packet,&table)||!table||!Read(table+8,&get)||!get||reinterpret_cast<int(*)(uintptr_t)>(get)(packet)!=0x90)return;
 bool mine=false;{auto& m=Rotations();std::lock_guard<std::recursive_mutex> lock(m.guard);mine=m.selValid&&m.selId==kRotAutoMace;}
 std::lock_guard<std::mutex> lock(s.guard);if(mine){s.sentTarget=s.target;s.hasSentTarget=s.hasTarget;}else s.hasSentTarget=false;}

// ---- guarded entry points
inline void AutoMaceFaulted(){InterlockedExchange(&AutoMace().fault,1);if(actionLog)actionLog("Auto Mace: fault caught, module stopped until restart");}
inline void AutoMaceAttack(uintptr_t target){if(AutoMace().fault)return;__try{AutoMaceAttackWork(target);}__except(EXCEPTION_EXECUTE_HANDLER){AutoMaceFaulted();}}
inline void AutoMaceScreen(){if(AutoMace().fault)return;__try{AutoMaceScreenWork();}__except(EXCEPTION_EXECUTE_HANDLER){AutoMaceFaulted();}}
inline void AutoMaceCamera(){if(AutoMace().fault)return;__try{AutoMaceCameraWork();}__except(EXCEPTION_EXECUTE_HANDLER){AutoMaceFaulted();}}
inline bool AutoMaceReceive(uintptr_t packet){if(AutoMace().fault)return false;__try{return AutoMaceReceiveWork(packet);}__except(EXCEPTION_EXECUTE_HANDLER){AutoMaceFaulted();return false;}}
inline void AutoMaceSend(uintptr_t packet){if(AutoMace().fault)return;__try{AutoMaceSendWork(packet);}__except(EXCEPTION_EXECUTE_HANDLER){AutoMaceFaulted();}}
}
