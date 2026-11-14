#pragma once
#include "game_render.hpp"
#include "game_filter.hpp"
#include "game_visual_entities.hpp"
#include "../include/nickel/visual_filter_rules.hpp"
#include <limits>
#include <memory>
#include <mutex>
#include <unordered_set>
#include <vector>
namespace game {
struct FoliageRefreshShared{std::mutex guard;nickel::visual::FoliageRefreshState state{};bool working=false;volatile long fault=0;void(*log)(const char*)=nullptr;};
inline FoliageRefreshShared& FoliageRefresh(){static FoliageRefreshShared* shared=new FoliageRefreshShared;return *shared;}
inline void RelationsFaultReport(){
 auto& shared=VisualRelations();if(InterlockedExchange(&shared.fault,1)!=0)return;
 void(*log)(const char*)=nullptr;{std::lock_guard<std::mutex> lock(shared.guard);shared.frame.reset();log=shared.log;}
 if(log)log("Anti Bot: a fault was caught while reading the player roster; bot filtering is disabled until the game restarts");
}
inline void RosterWork(void*){
 auto& shared=VisualRelations();if(shared.fault)return;
 bool enabled=false;uint64_t generation=0;
 {std::lock_guard<std::mutex> lock(shared.guard);enabled=shared.enabled&&!shared.fault;generation=shared.generation;}
 if(!enabled||!strcmp(currentScreen,"debug_screen")||!strcmp(currentScreen,"toast_screen"))return;
 Context context{};if(!Local(&context))return;
 uint32_t serial{};{std::lock_guard<std::mutex> lock(Shared().guard);serial=Shared().worldSerial;}
 int dimension{};if(!ReadDimension(&dimension))return;
 // Rebuilt at most every 50 ms (consumers accept 250 ms), but at once when the actor, world or dimension changed or there is no frame.
 static uint64_t lastTick=0;const uint64_t now=GetTickCount64();
 {std::shared_ptr<const VisualRosterFrame> current;{std::lock_guard<std::mutex> lock(shared.guard);current=shared.frame;}
  if(current&&now-lastTick<50&&current->actor==context.actor&&current->worldSerial==serial&&current->dimension==dimension)return;}
 uint64_t container=0;nickel::visual::VisualRoster roster{};
 if(PlayerContainer(context,&container))roster=nickel::visual::read_visual_roster(container,[](uintptr_t at,uint64_t* value){return Read(at,value);});
 auto frame=std::make_shared<VisualRosterFrame>();frame->roster=std::move(roster);frame->tick=now;frame->actor=context.actor;frame->worldSerial=serial;frame->dimension=dimension;
 {std::lock_guard<std::mutex> lock(shared.guard);if(shared.enabled&&!shared.fault&&shared.generation==generation){shared.frame=frame;lastTick=now;}}
}
inline void RosterGuarded(void* ui){__try{RosterWork(ui);}__except(EXCEPTION_EXECUTE_HANDLER){RelationsFaultReport();}}
inline void FoliageWork(void*){
 auto& shared=FoliageRefresh();uint64_t generation=0;
 {std::lock_guard<std::mutex> lock(shared.guard);if(shared.fault||shared.working||!nickel::visual::foliage_refresh_pending(shared.state))return;generation=shared.state.generation;shared.working=true;}
 auto finish=[&](bool ack){std::lock_guard<std::mutex> lock(shared.guard);if(ack)nickel::visual::acknowledge_foliage(shared.state,generation);shared.working=false;};
 struct Probe{unsigned char b[16];};static const unsigned char expected[16]={0x55,0x41,0x57,0x41,0x56,0x41,0x55,0x41,0x54,0x56,0x57,0x53,0x48,0x83,0xec,0x78};
 uintptr_t base{};uint32_t size{};Probe probe{};
 if(!Supported(&base,&size)||!Read(base+0x7921b70,&probe)||std::memcmp(probe.b,expected,16))return finish(false);
 Context context{};if(!Client(&context))return finish(false);
 const uint64_t rendererOf=Virtual(context,context.client,0x5c8);if(!rendererOf)return finish(false);
 const uint64_t renderer=reinterpret_cast<uint64_t(*)(uint64_t)>(rendererOf)(context.client);if(!renderer)return finish(false);
 const auto targets=nickel::visual::read_foliage_targets(renderer,[](uintptr_t at,uint64_t* value){return Read(at,value);});
 if(!targets.known)return finish(false);
 const auto refresh=reinterpret_cast<void(*)(uintptr_t,uint32_t,uint32_t)>(base+0x7921b70);
 for(uintptr_t target:targets.values)refresh(target,1,1);
 finish(true);
}
inline void FoliageFaultReport(){
 auto& shared=FoliageRefresh();{std::lock_guard<std::mutex> lock(shared.guard);shared.working=false;}
 if(InterlockedExchange(&shared.fault,1)!=0)return;
 InterlockedExchange(&Filter().foliageFault,1);
 void(*log)(const char*)=nullptr;{std::lock_guard<std::mutex> lock(shared.guard);log=shared.log;}
 if(log)log("No Render foliage: a fault was caught while refreshing foliage; foliage filtering is disabled until the game restarts");
}
inline void FoliageGuarded(void*){__try{FoliageWork(nullptr);}__except(EXCEPTION_EXECUTE_HANDLER){FoliageFaultReport();}}
inline void FiltersScreenWork(void* ui){RosterGuarded(ui);FoliageGuarded(ui);}
inline void ConfigureVisualFilters(bool antiBot,bool effectiveFoliage,void(*log)(const char*)){
 {auto& shared=VisualRelations();std::lock_guard<std::mutex> lock(shared.guard);if(shared.enabled!=antiBot){++shared.generation;shared.frame.reset();}shared.enabled=antiBot;shared.log=log;}
 {auto& shared=FoliageRefresh();std::lock_guard<std::mutex> lock(shared.guard);nickel::visual::request_foliage(shared.state,effectiveFoliage);shared.log=log;}
 static std::once_flag once;std::call_once(once,[]{visualFiltersScreenWork.store(FiltersScreenWork,std::memory_order_release);});
}
}
