// Auto Crossbow: fires every charged crossbow of the hotbar once, then turns itself off.
// - The module has no event subscription: everything happens when it is enabled. The enable request is handled on the
//   action thread (screen render hook, where sequences run). Without a local player, or without any charged crossbow
//   in hotbar slots 0..8, nothing is submitted and the module turns itself off.
// - A charged crossbow is a stack with a non-zero count (stack+0x22), item name "crossbow", and user data
//   (CompoundTag at stack+0x10) holding a non-empty "chargedItem" compound.
// - One sequence is submitted (Silent Use = silent switching): for each charged slot, in hotbar order,
//   [wait Delay, except before the first shot] -> check the slot still holds a charged crossbow (else cancel)
//   -> switch to the slot (cancelled when outside the hotbar, min(size, 9)) -> [wait Switch Delay, only without
//   Silent Use] -> use the held item. After the last shot: switch back to the original slot.
// - When the sequence ends (finished or cancelled) the module turns itself off. Disabling cancels the sequence.
#pragma once
#include "game_actions.hpp"
#include "game_sus.hpp"
#include <atomic>
#include <utility>

namespace game {
constexpr int kAutoCrossbow=13;
struct AutoCrossbowShared{std::atomic<bool> enabled{false},silent{true},request{false},cancel{false},done{false};
 std::atomic<int> delay{150},switchDelay{50};volatile long fault=0;};
inline AutoCrossbowShared& AutoCrossbow(){static auto* s=new AutoCrossbowShared;return *s;}

// Charged crossbow test on an item stack.
inline bool CrossbowCharged(const Context& c,uint64_t stack){uint8_t count{};uint64_t user{},size{};
 if(!stack||!Read(stack+0x22,&count)||!count||!ActionNameIs(stack,"crossbow"))return false;
 if(!Read(stack+0x10,&user)||!user)return false;const uint64_t tag=SusTag(c,user,"chargedItem",10);
 return tag&&Read(tag+0x10,&size)&&size!=0;}
// Step: the slot still holds a charged crossbow -> next, else cancel.
inline ActionStep StepCrossbowCharged(int slot){return [slot](ActionCtx& x){return CrossbowCharged(*x.c,ActionStackAt(*x.c,ActionContainer(x.actor),slot))?1:2;};}
// Step: switch to a hotbar slot; cancelled when the slot is outside min(container size, 9).
inline ActionStep StepSwitchSlot(int slot){return [slot](ActionCtx& x){if(slot<0)return 2;const int n=ActionSize(*x.c,ActionContainer(x.actor));
 if(slot>=(n<9?n:9))return 2;ActionSwitch(x,slot);return 1;};}

inline void AutoCrossbowStart(){auto& s=AutoCrossbow();Context c{};if(!Local(&c)){s.done=true;return;}
 const bool silent=s.silent;const double switchDelay=silent?0.0:(double)s.switchDelay.load();const int delay=s.delay;
 ActionSequence q;q.module=kAutoCrossbow;q.silent=silent?1:0;bool any=false;const uint64_t box=ActionContainer(c.actor);
 for(int i=0;i<9;++i){if(!CrossbowCharged(c,ActionStackAt(c,box,i)))continue;
  if(any&&delay>0)q.steps.push_back(StepWait((double)delay));
  q.steps.push_back(StepCrossbowCharged(i));q.steps.push_back(StepSwitchSlot(i));
  if(switchDelay>0.0)q.steps.push_back(StepWait(switchDelay));
  q.steps.push_back(StepUse());any=true;}
 if(!any){s.done=true;return;}
 q.steps.push_back(StepSwitchBack());
 q.done=[](int){AutoCrossbow().done=true;};
 ActionSubmit(std::move(q));}
// Action thread (screen render): disable first, then a pending enable.
inline void AutoCrossbowWork(){auto& s=AutoCrossbow();if(s.cancel.exchange(false))ActionCancel(kAutoCrossbow);
 if(s.request.exchange(false)&&s.enabled)AutoCrossbowStart();}
inline void AutoCrossbowTick(){auto& s=AutoCrossbow();if(s.fault)return;__try{AutoCrossbowWork();}__except(EXCEPTION_EXECUTE_HANDLER){InterlockedExchange(&s.fault,1);
 if(actionLog)actionLog("Auto Crossbow: fault caught, module stopped until restart");}}
}
