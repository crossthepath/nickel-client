// Auto Crystal: places and breaks end crystals where the player aims while the right button is held.
// - Can act: local player with external input data, right button held, and with Require Crystal an
//   "end_crystal" in the selected slot.
// - Aim target (level->vtable[0xa78] hit result, kind at +0x18):
//   * block (kind 0, cell at +0x20): the block must be "minecraft:bedrock" or "minecraft:obsidian" and the cell
//     above "minecraft:air". The ActorOwnerComponent storage is walked from its last entity to its first; the
//     first actor whose AABB (*(actor+0x248)) overlaps the cell above (strict bounds) blocks the place:
//     an end crystal (ActorTypeComponent 0x47) becomes the break target, any other actor gives a valid target
//     with nothing to do. Without one: place target on the clicked block.
//   * entity (kind 1): the hit actor, if it is an end crystal, is the break target.
// - GameUpdate (run on the action thread, see game_actions.hpp): while no sequence is running, with a valid
//   target, the module becomes busy, the switch timer restarts and one sequence is submitted:
//   without Require Crystal: find "end_crystal" (Ghost Switch = silent switching), the crystal loop, switch back;
//   with Require Crystal: the crystal loop only (no switching). Its end clears the busy flag.
// - Crystal loop (one step, repeated every frame): stops when the player cannot act. A valid target restarts
//   the switch timer; without one, Require Crystal stops at once, otherwise it stops once Switch Delay has
//   passed since the last valid target. Then, between silent begin/end: place target and Place Delay passed
//   -> item used on the top face of the block (transaction type 0, no swing); break target and Break Delay
//   passed -> arm swing (kind 4) and attack transaction. Timers restart when they fire (strictly greater).
// - BuildBlock: the game's own placement is cancelled while the module is busy.
// - Disabling cancels the module's sequence, restarts the three timers and clears the busy flag.
#pragma once
#include "game_actions.hpp"
#include "game_auto_anchor.hpp"
#include "game_combat.hpp"
#include "game_sticky_aim.hpp"
#include "game_utility_packets.hpp"
#include "game_xray.hpp"
#include <atomic>
#include <cstring>
#include <string>

namespace game {
constexpr int kAutoCrystal=14;   // action module id
struct AutoCrystalShared{std::atomic<bool> enabled{false},requireCrystal{false},ghost{true},rightHeld{false},busy{false},resetPending{false};
 std::atomic<int> placeDelay{50},breakDelay{50},switchDelay{150};
 std::atomic<int64_t> placeAt{0},breakAt{0},switchAt{0};volatile long fault=0;};
inline AutoCrystalShared& AutoCrystal(){static auto* s=[]{auto* p=new AutoCrystalShared;const int64_t now=ActionNs();p->placeAt=now;p->breakAt=now;p->switchAt=now;return p;}();return *s;}

// Timer: fires (and restarts) when more than `delay` ms have passed.
inline bool AutoCrystalTimer(std::atomic<int64_t>& at,int delay){if(!((double)delay<ActionMs(at.load())))return false;at=ActionNs();return true;}

inline bool AutoCrystalIsCrystal(uintptr_t actor){uintptr_t registry{},element{};uint32_t entity{};int32_t kind{};
 return ActorEntity(actor,&registry,&entity)&&Element(registry,entity,"ActorTypeComponent",4,&element)&&Read(element,&kind)&&kind==0x47;}
inline bool AutoCrystalBlockIs(const Context& c,uint64_t region,const int32_t* pos,const char* want){std::string name;const uint64_t block=RegionBlock(c,region,pos);return block&&XrayBlockName(block,&name)&&name==want;}
// First actor (last to first in the ActorOwnerComponent storage) whose AABB overlaps the cell above `pos`.
inline uintptr_t AutoCrystalOccupant(const Context& c,const int32_t* pos){uintptr_t owners{};uint64_t begin{},end{};
 if(!Storage(c.registry,Fnv("ActorOwnerComponent"),&owners)||!Read(owners+0x20,&begin)||!Read(owners+0x28,&end)||end<begin||(end-begin)%4||(end-begin)>4*65536)return 0;
 const float x=(float)pos[0],y=(float)(pos[1]+1),z=(float)pos[2];const float x1=x+1.0f,y1=y+1.0f,z1=z+1.0f;
 for(uint64_t at=end;at>begin;){at-=4;uint32_t entity{};if(!Read(at,&entity))break;if(entity>=0xfffc0000u)continue;
  uintptr_t slot{};uint64_t actor{},box{};if(!InStorage(owners,entity,8,&slot)||!Read(slot,&actor)||!actor||!Read(actor+0x248,&box)||!box)continue;
  float b[6]{};bool ok=true;for(int i=0;i<6&&ok;++i)ok=Read(box+i*4,&b[i]);if(!ok)continue;
  if(!(b[0]<x1)||!(b[1]<y1)||!(b[2]<z1))continue;if(b[5]<=z)continue;if(b[4]<=y||b[3]<=x)continue;return (uintptr_t)actor;}
 return 0;}

struct AutoCrystalTarget{uintptr_t actor=0;int32_t pos[3]{};bool place=false,valid=false;};
inline void AutoCrystalAim(const Context& c,AutoCrystalTarget* t){*t=AutoCrystalTarget{};uint64_t hit{};int32_t kind=-1;if(!CrosshairHit(c,&hit,&kind))return;
 if(kind==0){const uint64_t region=ActorRegion(c);if(!region)return;int32_t pos[3]{};if(!Read(hit+0x20,&pos[0])||!Read(hit+0x24,&pos[1])||!Read(hit+0x28,&pos[2]))return;
  if(!AutoCrystalBlockIs(c,region,pos,"minecraft:bedrock")&&!AutoCrystalBlockIs(c,region,pos,"minecraft:obsidian"))return;
  const int32_t above[3]={pos[0],pos[1]+1,pos[2]};if(!AutoCrystalBlockIs(c,region,above,"minecraft:air"))return;
  if(const uintptr_t blocker=AutoCrystalOccupant(c,pos)){t->actor=AutoCrystalIsCrystal(blocker)?blocker:0;t->place=false;t->valid=true;return;}
  memcpy(t->pos,pos,12);t->place=true;t->valid=true;return;}
 if(kind==1){const uintptr_t actor=CrosshairActor(c);if(!actor||!AutoCrystalIsCrystal(actor))return;t->actor=actor;t->place=false;t->valid=true;}}

inline bool AutoCrystalCanAct(const Context& c){auto& s=AutoCrystal();if(!StickyHasInput(c)||!s.rightHeld)return false;
 return !s.requireCrystal||ActionNameIs(ActionHeld(c,c.actor),"end_crystal");}

// The crystal loop step: 0 = run again next frame, 1 = finished.
inline ActionStep StepAutoCrystal(){return [](ActionCtx& x){auto& s=AutoCrystal();const Context& c=*x.c;if(!AutoCrystalCanAct(c))return 1;
 AutoCrystalTarget t{};AutoCrystalAim(c,&t);
 if(t.valid)s.switchAt=ActionNs();
 else{if(s.requireCrystal)return 1;if((double)s.switchDelay.load()<ActionMs(s.switchAt.load()))return 1;}
 ActionSilentBegin(x);AutoCrystalAim(c,&t);
 if(t.valid){if(!t.actor){if(t.place&&AutoCrystalTimer(s.placeAt,s.placeDelay))ActionUseOnBlock(c,x.actor,t.pos,1);}
  else if(AutoCrystalTimer(s.breakAt,s.breakDelay)){ActionSwing(c,x.actor,4);AttackActor(c,t.actor);}}
 ActionSilentEnd(x);return 0;};}

inline void AutoCrystalReset(){auto& s=AutoCrystal();ActionCancel(kAutoCrystal);const int64_t now=ActionNs();s.placeAt=now;s.breakAt=now;s.switchAt=now;s.busy=false;}
inline void AutoCrystalWork(){auto& s=AutoCrystal();if(s.resetPending.exchange(false))AutoCrystalReset();if(!s.enabled||s.busy)return;
 Context c{};if(!Local(&c)||!AutoCrystalCanAct(c))return;AutoCrystalTarget t{};AutoCrystalAim(c,&t);if(!t.valid)return;
 s.busy=true;s.switchAt=ActionNs();const bool require=s.requireCrystal;
 ActionSequence q;q.module=kAutoCrystal;q.silent=(!require&&s.ghost)?1:0;
 if(!require)q.steps.push_back(StepFindName("end_crystal"));
 q.steps.push_back(StepAutoCrystal());
 if(!require)q.steps.push_back(StepSwitchBack());
 q.done=[](int){AutoCrystal().busy=false;};
 ActionSubmit(std::move(q));}
inline void AutoCrystalTick(){auto& s=AutoCrystal();if(s.fault)return;__try{AutoCrystalWork();}__except(EXCEPTION_EXECUTE_HANDLER){InterlockedExchange(&s.fault,1);if(actionLog)actionLog("Auto Crystal: fault caught, module stopped until restart");}}

// BuildBlock: true cancels the game's own placement.
inline bool AutoCrystalBuildCancel(){auto& s=AutoCrystal();return s.enabled&&s.busy;}
inline bool AutoCrystalBuildCancelGuarded(){if(AutoCrystal().fault)return false;__try{return AutoCrystalBuildCancel();}__except(EXCEPTION_EXECUTE_HANDLER){InterlockedExchange(&AutoCrystal().fault,1);return false;}}
}
