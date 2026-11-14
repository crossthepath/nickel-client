// Auto Anchor: charges and detonates a respawn anchor right after the player places it.
// - BuildBlock: with "respawn_anchor" held, the placed cell (clicked block + face offset) is remembered with the time,
//   unless it is the anchor being handled or already remembered.
// - Screen render (action thread): without a local player with external input data, everything is cancelled and
//   forgotten. Cells older than 1000 ms are dropped. While no anchor is being handled, the first remembered cell that
//   holds a respawn anchor is taken and one sequence is submitted (Ghost Use = silent switching, 1000 ms limit):
//   find glowstone, check the cell, use the item on its top face, wait until it is charged, wait Switch Delay, switch
//   back to the original item, check it is still charged, use the item on its top face (detonation).
// - Use on a block: arm swing (kind 3) unless silent, then an ItemUseInventoryTransaction of type 0 on the cell, the
//   click point random within +-0.2 of the face centre.
// - Disabling cancels the module's sequence and forgets everything.
#pragma once
#include "game_actions.hpp"
#include "game_sticky_aim.hpp"
#include "game_sus.hpp"
#include "game_utility_packets.hpp"
#include <algorithm>
#include <array>
#include <random>
#include <mutex>
#include <vector>

namespace game {
constexpr int kAutoAnchor=12;
struct AutoAnchorShared{std::mutex guard;std::atomic<bool> enabled{false},ghost{true},resetPending{false};std::atomic<int> switchDelay{50};
 struct Cell{int32_t pos[3];int64_t at;};std::vector<Cell> cells;bool active=false;int32_t target[3]{};volatile long fault=0;};
inline AutoAnchorShared& AutoAnchor(){static auto* s=new AutoAnchorShared;return *s;}

// Block of the local region at a cell.
inline uint64_t AnchorBlock(const Context& c,const int32_t* pos){const uint64_t region=ActorRegion(c);return region?RegionBlock(c,region,pos):0;}
inline bool AnchorIs(const Context& c,const int32_t* pos){uint64_t legacy{};char name[96]{};const uint64_t block=AnchorBlock(c,pos);
 return block&&Read(block+0x68,&legacy)&&legacy&&Text(legacy+0xe8,name,sizeof(name))&&strstr(name,"respawn_anchor");}
// respawn_anchor_charge of the block's states; false when the state is missing.
inline bool AnchorCharge(const Context& c,const int32_t* pos,int32_t* charge){const uint64_t block=AnchorBlock(c,pos);if(!block)return false;
 const uint64_t states=SusTag(c,block+0xf0,"states",10);const uint64_t tag=states?SusTag(c,states,"respawn_anchor_charge",3):0;return tag&&Read(tag+8,charge);}
inline bool AnchorCharged(const Context& c,const int32_t* pos){int32_t charge{};return AnchorIs(c,pos)&&AnchorCharge(c,pos,&charge)&&charge>0;}

// Use the held item on a block face (game transaction type 0), as the original's use-on-block helper.
inline void ActionUseOnBlock(const Context& c,uintptr_t actor,const int32_t* pos,uint8_t face){int32_t* sel=ActionSelected(actor);float me[3]{};if(!sel||!ActionPosition(actor,me))return;
 static const float n[6][3]={{0,-1,0},{0,1,0},{0,0,-1},{0,0,1},{-1,0,0},{1,0,0}};static std::mt19937 rng{std::random_device{}()};float rel[3]{};
 for(int k=0;k<3;++k){const float v=face<6?n[face][k]:0.0f;const float r=(float)(rng()>>8)*5.9604645e-08f*0.4f-0.2f;rel[k]=v*0.5f+0.5f+(1.0f-std::fabs(v))*r;}
 const uint64_t stack=ActionHeld(c,actor);int32_t runtime{};if(const uint64_t block=AnchorBlock(c,pos))Read(block+0x114,&runtime);
 uint8_t* t=NewUseTransaction(c,stack);*(int32_t*)(t+0x68)=0;memcpy(t+0x70,pos,12);*(int32_t*)(t+0x7c)=runtime;*(int32_t*)(t+0x84)=*sel;*(uint8_t*)(t+0x88)=0;*(uint8_t*)(t+0x80)=face;
 memcpy(t+0xf0,me,12);memcpy(t+0xfc,rel,12);SendUseTransaction(c,actor,t);}
inline ActionStep StepUseOnBlock(std::array<int32_t,3> pos,uint8_t face){return [pos,face](ActionCtx& x){ActionSilentBegin(x);if(x.silent!=1)ActionSwing(*x.c,x.actor,3);
 ActionUseOnBlock(*x.c,x.actor,pos.data(),face);ActionSilentEnd(x);return 1;};}

inline bool AnchorSame(const int32_t* a,const int32_t* b){return a[0]==b[0]&&a[1]==b[1]&&a[2]==b[2];}
inline void AutoAnchorBuild(const int32_t* clicked,uint8_t face){auto& s=AutoAnchor();if(!s.enabled)return;Context c{};if(!Local(&c)||!ActionNameIs(ActionHeld(c,c.actor),"respawn_anchor"))return;
 static const int dx[6]={0,0,0,0,-1,1},dy[6]={-1,1,0,0,0,0},dz[6]={0,0,-1,1,0,0};
 const int32_t pos[3]={clicked[0]+(face<6?dx[face]:0),clicked[1]+(face<6?dy[face]:0),clicked[2]+(face<6?dz[face]:0)};
 std::lock_guard<std::mutex> lock(s.guard);if(s.active&&AnchorSame(s.target,pos))return;for(auto& cell:s.cells)if(AnchorSame(cell.pos,pos))return;
 s.cells.push_back({{pos[0],pos[1],pos[2]},ActionNs()});}
inline void AutoAnchorBuildGuarded(const int32_t* pos,uint8_t face){if(AutoAnchor().fault)return;__try{AutoAnchorBuild(pos,face);}__except(EXCEPTION_EXECUTE_HANDLER){InterlockedExchange(&AutoAnchor().fault,1);}}

inline void AutoAnchorForget(){auto& s=AutoAnchor();ActionCancel(kAutoAnchor);std::lock_guard<std::mutex> lock(s.guard);s.cells.clear();s.active=false;}
inline void AutoAnchorWork(){auto& s=AutoAnchor();if(s.resetPending.exchange(false))AutoAnchorForget();if(!s.enabled)return;
 Context c{};if(!Local(&c)||!StickyHasInput(c)){AutoAnchorForget();return;}
 std::array<int32_t,3> target{};
 {std::lock_guard<std::mutex> lock(s.guard);const int64_t now=ActionNs();
  s.cells.erase(std::remove_if(s.cells.begin(),s.cells.end(),[&](const AutoAnchorShared::Cell& cell){return (double)(now-cell.at)/1000000.0>1000.0;}),s.cells.end());
  if(s.active||s.cells.empty())return;
  auto it=std::find_if(s.cells.begin(),s.cells.end(),[&](const AutoAnchorShared::Cell& cell){return AnchorIs(c,cell.pos);});if(it==s.cells.end())return;
  target={it->pos[0],it->pos[1],it->pos[2]};s.cells.erase(it);memcpy(s.target,target.data(),12);s.active=true;}
 ActionSequence q;q.module=kAutoAnchor;q.silent=s.ghost?1:0;
 q.conditions.push_back([](uintptr_t,int64_t start){const double e=ActionMs(start);return e<1000.0||e==1000.0;});
 auto stillTarget=[target](){auto& a=AutoAnchor();std::lock_guard<std::mutex> lock(a.guard);return a.active&&AnchorSame(a.target,target.data());};
 q.steps.push_back(StepFindName("glowstone"));
 q.steps.push_back([target,stillTarget](ActionCtx& x){return stillTarget()&&AnchorIs(*x.c,target.data())?1:2;});
 q.steps.push_back(StepUseOnBlock(target,1));
 q.steps.push_back([target](ActionCtx& x){return !AnchorIs(*x.c,target.data())||AnchorCharged(*x.c,target.data())?1:0;});
 if(const int delay=s.switchDelay;delay>0)q.steps.push_back(StepWait((double)delay));
 q.steps.push_back(StepSwitchBack());
 q.steps.push_back([target,stillTarget](ActionCtx& x){return stillTarget()&&AnchorCharged(*x.c,target.data())?1:2;});
 q.steps.push_back(StepUseOnBlock(target,1));
 q.done=[target](int){auto& a=AutoAnchor();std::lock_guard<std::mutex> lock(a.guard);if(a.active&&AnchorSame(a.target,target.data()))a.active=false;};
 ActionSubmit(std::move(q));}
inline void AutoAnchorTick(){auto& s=AutoAnchor();if(s.fault)return;__try{AutoAnchorWork();}__except(EXCEPTION_EXECUTE_HANDLER){InterlockedExchange(&s.fault,1);if(actionLog)actionLog("Auto Anchor: fault caught, module stopped until restart");}}
}
