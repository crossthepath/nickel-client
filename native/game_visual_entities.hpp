// Read-only entity snapshot used by Visuals (layouts shared with game_events.hpp ReadVitals/ActorByRuntimeId;
// bounds at actor+0x248, RenderPosition and offsets). No game calls, hooks or writes. Team classification is unknown.
// Invisible flag: ActorDataFlagComponent (stride 0x18), bit 5.
#pragma once
#include <algorithm>
#include <vector>
#include "game_events.hpp"
#include "../include/nickel/visual_entity_rules.hpp"
#include "../include/nickel/visual_world.hpp"
#include "../include/nickel/visual_filter_rules.hpp"
#include <array>
#include <memory>
namespace game {
struct VisualIdentity{char name[256]{},display[1024]{};};
struct VisualEntity { uint64_t id; uintptr_t actor; bool player,invisible,living,teammate,healthKnown; float minimum[3],maximum[3],distance,health,maxHealth; };
inline bool RawText(uintptr_t address,char* out,size_t capacity){return nickel::visual::read_visual_text(address,out,capacity,[](uintptr_t at,auto* value){return Read(at,value);});}
inline bool ReadVisualFeet(const Context& context,float out[3]){
 uintptr_t positions{},element{};float position[3],offset=0,raw{};
 if(!Storage(context.registry,Fnv("RenderPositionComponent"),&positions)||!InStorage(positions,context.entity,12,&element)||!Floats(element,position,3))return false;
 if(Element(context.registry,context.entity,"OffsetsComponent",0x44,&element)){if(!Floats(element,&raw,1)||!std::isfinite(raw))return false;offset=raw;}
 const nickel::visual::Vec3 feet=nickel::visual::feet_position({position[0],position[1],position[2]},offset);
 if(!std::isfinite(feet.x)||!std::isfinite(feet.y)||!std::isfinite(feet.z))return false;
 out[0]=feet.x;out[1]=feet.y;out[2]=feet.z;return true;
}
inline bool ReadVisualIdentity(uintptr_t registry,uint32_t entity,uintptr_t actor,VisualIdentity* out){
 if(!out)return false;out->name[0]=out->display[0]=0;
 if(actor)RawText(actor+0xbe8,out->name,sizeof(out->name));
 uintptr_t element{};uint64_t begin{},end{};
 if(Element(registry,entity,"SynchedActorDataComponent",0x48,&element)&&Read(element,&begin)&&Read(element+8,&end)&&end>=begin&&(end-begin)%8==0&&(end-begin)>=0x28&&(end-begin)<=0x8000){uint64_t item{};if(Read(begin+0x20,&item)&&item)RawText(item+0x10,out->display,sizeof(out->display));}
 return true;
}
struct VisualRosterFrame{nickel::visual::VisualRoster roster;uint64_t tick=0;uintptr_t actor=0;uint32_t worldSerial=0;int dimension=-1;};
struct VisualRelationsShared{std::mutex guard;bool enabled=false;uint64_t generation=0;std::shared_ptr<const VisualRosterFrame> frame;volatile long fault=0;void(*log)(const char*)=nullptr;};
inline VisualRelationsShared& VisualRelations(){static VisualRelationsShared* shared=new VisualRelationsShared;return *shared;}
inline std::shared_ptr<const VisualRosterFrame> SnapshotVisualRoster(const Context& context){
 auto& shared=VisualRelations();std::shared_ptr<const VisualRosterFrame> frame;
 {std::lock_guard<std::mutex> lock(shared.guard);if(shared.enabled&&!shared.fault)frame=shared.frame;}
 if(!frame)return nullptr;
 uint32_t serial{};{std::lock_guard<std::mutex> lock(Shared().guard);serial=Shared().worldSerial;}
 int dimension{};if(!ReadDimension(&dimension))return nullptr;
 if(!nickel::visual::world_frame_fresh(GetTickCount64(),frame->tick,frame->actor,context.actor,frame->worldSerial,serial,frame->dimension,dimension))return nullptr;
 return frame;
}
// Optional pre-resolved storages (0 = component storage missing) so loops do not re-resolve them per entity.
struct VisualBotStorages{uintptr_t local=0,uid=0;};
inline VisualBotStorages ResolveVisualBotStorages(uintptr_t registry){VisualBotStorages s;if(!Storage(registry,Fnv("LocalPlayerComponent"),&s.local))s.local=0;if(!Storage(registry,Fnv("ActorUniqueIDComponent"),&s.uid))s.uid=0;return s;}
inline bool IsVisualBot(const Context& context,uint32_t entity,bool player,const std::shared_ptr<const VisualRosterFrame>& frame,const VisualBotStorages* storages=nullptr){
 if(!frame||!player)return false;
 bool enabled=false;{std::lock_guard<std::mutex> lock(VisualRelations().guard);enabled=VisualRelations().enabled&&!VisualRelations().fault;}
 if(!enabled)return false;
 uintptr_t element{};uint64_t uid=0;bool local,known;
 if(storages){uint32_t packed{};local=storages->local&&Packed(storages->local,entity,&packed);known=storages->uid&&InStorage(storages->uid,entity,8,&element)&&Read(element,&uid);}
 else{local=Has(context.registry,entity,"LocalPlayerComponent");known=Element(context.registry,entity,"ActorUniqueIDComponent",8,&element)&&Read(element,&uid);}
 return nickel::visual::visual_bot(true,player,local,frame->roster,known,uid);
}
inline int ReadVisualEntities(VisualEntity* out,int capacity,float range,const nickel::visual::TeamRules* teams=nullptr) {
 if(!out||capacity<=0||!std::isfinite(range)||range<=0)return 0;
 Context context{};uintptr_t owners{},positions{},mine{};float origin[3];
 if(!Local(&context)||!Storage(context.registry,Fnv("ActorOwnerComponent"),&owners)||!Storage(context.registry,Fnv("RenderPositionComponent"),&positions)||!InStorage(positions,context.entity,12,&mine)||!Floats(mine,origin,3))return 0;
 VisualIdentity self{};if(teams&&teams->enabled)ReadVisualIdentity(context.registry,context.entity,context.actor,&self);
 uint64_t begin{},end{};if(!Read(owners+0x20,&begin)||!Read(owners+0x28,&end)||end<begin||(end-begin)%4||(end-begin)>4*65536)return 0;
 const std::shared_ptr<const VisualRosterFrame> roster=SnapshotVisualRoster(context);
 // Component storages resolved once per call (0 = missing: the lookup then fails for every entity, as before).
 uintptr_t offsetsS{},flagsS{},playerS{},typeS{},attributesS{},runtimeS{};
 if(!Storage(context.registry,Fnv("OffsetsComponent"),&offsetsS))offsetsS=0;if(!Storage(context.registry,Fnv("ActorDataFlagComponent"),&flagsS))flagsS=0;if(!Storage(context.registry,Fnv("PlayerComponent"),&playerS))playerS=0;
 if(!Storage(context.registry,Fnv("ActorTypeComponent"),&typeS))typeS=0;if(!Storage(context.registry,Fnv("AttributesComponent"),&attributesS))attributesS=0;if(!Storage(context.registry,Fnv("RuntimeIDComponent"),&runtimeS))runtimeS=0;
 const VisualBotStorages botStorages=ResolveVisualBotStorages(context.registry);
 int count=0;for(uint64_t at=begin;at<end&&count<capacity;at+=4) {
  uint32_t entity{};uintptr_t slot{},element{};uint64_t actor{},box{};float position[3],bounds[6],offset{};
  if(!Read(at,&entity))break;if(entity>=0xfffc0000u||entity==context.entity)continue;
  if(!InStorage(positions,entity,12,&element)||!Floats(element,position,3))continue;
  const float dx=position[0]-origin[0],dy=position[1]-origin[1],dz=position[2]-origin[2];const float square=dx*dx+dy*dy+dz*dz;
  if(!nickel::visual::within_visual_range(square,range))continue;
  if(!InStorage(owners,entity,8,&slot)||!Read(slot,&actor)||!actor||!Read(actor+0x248,&box)||!box||!Floats(box,bounds,6))continue;
  {float raw{};if(offsetsS&&InStorage(offsetsS,entity,0x44,&element)){if(!Floats(element,&raw,1))continue;offset=raw;}}
  const float width=bounds[3]-bounds[0],height=bounds[4]-bounds[1],depth=bounds[5]-bounds[2];
  if(width<=0||height<=0||depth<=0||width>128||height>128||depth>128)continue;
  VisualEntity item{};item.actor=actor;uint64_t flags{};if(flagsS&&InStorage(flagsS,entity,0x18,&element)&&Read(element,&flags))item.invisible=(flags&(1ull<<5))!=0;{uint32_t packed{};item.player=playerS&&Packed(playerS,entity,&packed);}
  {int32_t type{};if(typeS&&InStorage(typeS,entity,4,&element)&&Read(element,&type))item.living=(type&0x100)!=0;}
  {float health{},maxHealth{};if(attributesS&&InStorage(attributesS,entity,0x50,&element)&&AttributeAt(element,6,&health,&maxHealth)){item.health=health;item.maxHealth=maxHealth;item.healthKnown=true;}}
  if(IsVisualBot(context,entity,item.player,roster,&botStorages))continue;
  if(!runtimeS||!InStorage(runtimeS,entity,8,&element)||!Read(element,&item.id))continue;
  item.minimum[0]=position[0]-width*.5f;item.minimum[1]=position[1]-offset;item.minimum[2]=position[2]-depth*.5f;
  item.maximum[0]=item.minimum[0]+width;item.maximum[1]=item.minimum[1]+height;item.maximum[2]=item.minimum[2]+depth;item.distance=std::sqrt(square);
  if(teams&&teams->enabled&&item.player){VisualIdentity other{};ReadVisualIdentity(context.registry,entity,actor,&other);item.teammate=nickel::visual::teammate(*teams,self.name,self.display,other.name,other.display);}
  out[count++]=item;
 }
 return count;
}
// True Sight clears bit 5 of ActorDataFlagComponent. It runs at the start of level render preparation
// (game_world.hpp, levelPrepStart) rather than on LevelTick to avoid flicker, and never from the overlay thread.
struct SightState { volatile long enabled=0,playersOnly=1,fault=0,seen=0; std::vector<uint32_t> cleared; };
inline SightState& Sight(){static SightState* state=new SightState;return *state;}
// On disable, the cleared invisibility is restored on entities still present: the server only resends the flag
// when it changes, so without this they would stay visible.
inline void SightRestore(const Context& context,uintptr_t flags){
 auto& state=Sight();for(uint32_t entity:state.cleared){uintptr_t element{};uint64_t value{};if(InStorage(flags,entity,0x18,&element)&&Read(element,&value))InterlockedOr64((volatile LONG64*)element,32ll);}
 state.cleared.clear();
}
inline void SightWork(){
 auto& state=Sight();if(state.fault)return;if(!state.enabled&&state.cleared.empty())return;
 Context context{};uintptr_t owners{},flags{};if(!Local(&context)||!Storage(context.registry,Fnv("ActorOwnerComponent"),&owners)||!Storage(context.registry,Fnv("ActorDataFlagComponent"),&flags))return;
 if(!state.enabled){SightRestore(context,flags);return;}
 uint64_t begin{},end{};if(!Read(owners+0x20,&begin)||!Read(owners+0x28,&end)||end<begin||(end-begin)%4||(end-begin)>4*65536)return;
 const auto roster=SnapshotVisualRoster(context);
 uintptr_t localS{},playerS{};if(!Storage(context.registry,Fnv("LocalPlayerComponent"),&localS))localS=0;if(!Storage(context.registry,Fnv("PlayerComponent"),&playerS))playerS=0;
 const VisualBotStorages botStorages=ResolveVisualBotStorages(context.registry);
 for(uint64_t at=begin;at<end;at+=4){uint32_t entity{},packed{};uintptr_t element{};uint64_t value{};if(!Read(at,&entity))return;if(entity>=0xfffc0000u||entity==context.entity||(localS&&Packed(localS,entity,&packed)))continue;
  const bool player=playerS&&Packed(playerS,entity,&packed);
  if(state.playersOnly&&!player)continue;
  if(IsVisualBot(context,entity,player,roster,&botStorages))continue;
  if(!InStorage(flags,entity,0x18,&element)||!Read(element,&value)||!(value&32))continue;
  InterlockedAnd64((volatile LONG64*)element,~32ll);InterlockedExchange(&state.seen,1);
  if(state.cleared.size()<65536&&std::find(state.cleared.begin(),state.cleared.end(),entity)==state.cleared.end())state.cleared.push_back(entity);
 }
}
inline void SightGuarded(){__try{SightWork();}__except(EXCEPTION_EXECUTE_HANDLER){InterlockedExchange(&Sight().fault,1);}}

}
