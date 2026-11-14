#pragma once
#include "game_render.hpp"
#include "game_filter.hpp"
#include "game_visual_entities.hpp"
#include "../include/nickel/visual_nametags.hpp"
#include "../include/nickel/visual_projection.hpp"
#include <array>
#include <cfloat>
#include <cstring>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>
namespace game {
struct NametagIcon{float x,y,side,ratioX,ratioY;int maximum,remaining;};
struct NametagRow{uint64_t id;uintptr_t actor;bool player,healthKnown;float anchorX,anchorY,distance,k,health;int hurtTicks;std::string label;std::vector<NametagIcon> icons;};
struct NametagFrame{uint64_t tick=0;uintptr_t actor=0;uint32_t worldSerial=0;int dimension=-1;std::vector<NametagRow> rows;std::unordered_map<uintptr_t,size_t> index;};
struct NametagRequest{bool enabled=false,useDisplayName=false,background=false,distance=false,health=false,equipment=false,durability=false,hideVanilla=false,mobs=false;float scale=1,width=0,height=0;};
struct NametagLocalCache{uint64_t tick=0;uintptr_t actor=0;uint32_t serial=0;int dimension=0;};   // local actor / world serial / dimension, reused by every vanilla tag checked within the same tick
inline thread_local NametagLocalCache nametagLocal;
struct NametagsShared{std::mutex guard;NametagRequest request{};uint64_t generation=0;std::shared_ptr<const NametagFrame> frame;volatile long fault=0,seen=0;void(*log)(const char*)=nullptr;};
inline NametagsShared& Nametags(){static NametagsShared* shared=new NametagsShared;return *shared;}
inline bool NametagRequestSame(const NametagRequest& a,const NametagRequest& b){
 return a.enabled==b.enabled&&a.useDisplayName==b.useDisplayName&&a.background==b.background&&a.distance==b.distance&&a.health==b.health&&a.equipment==b.equipment&&a.durability==b.durability&&a.hideVanilla==b.hideVanilla&&a.mobs==b.mobs&&a.scale==b.scale&&a.width==b.width&&a.height==b.height;
}
inline void NamesWork(void* ui){
 auto& shared=Nametags();if(shared.fault)return;
 NametagRequest request{};uint64_t generation=0;{std::lock_guard<std::mutex> lock(shared.guard);request=shared.request;generation=shared.generation;}
 if(!request.enabled)return;
 if(strcmp(currentScreen,"hud_screen"))return;
 Context context{};if(!Local(&context))return;
 uint32_t serial{};{std::lock_guard<std::mutex> lock(Shared().guard);serial=Shared().worldSerial;}
 int dimension{};if(!ReadDimension(&dimension))return;
 auto& view=View();if(view.fault)return;
 float m[16],origin[3];uint64_t viewTick=0;AcquireSRWLockShared(&view.lock);memcpy(m,view.m,sizeof(m));memcpy(origin,view.origin,sizeof(origin));viewTick=view.tick;ReleaseSRWLockShared(&view.lock);
 if(!viewTick||GetTickCount64()-viewTick>250)return;
 if(!std::isfinite(request.width)||!std::isfinite(request.height)||request.width<=0||request.height<=0)return;
 uintptr_t positions{},element{};float localPos[3];
 if(!Storage(context.registry,Fnv("RenderPositionComponent"),&positions)||!InStorage(positions,context.entity,12,&element)||!Floats(element,localPos,3))return;
 static thread_local std::vector<VisualEntity> entities;if(entities.size()<512)entities.resize(512);   // reused scratch buffer: ReadVisualEntities only writes the first count entries
 const int count=ReadVisualEntities(entities.data(),512,FLT_MAX);
 auto frame=std::make_shared<NametagFrame>();frame->tick=GetTickCount64();frame->actor=context.actor;frame->worldSerial=serial;frame->dimension=dimension;
 frame->rows.reserve(count<128?count:128);
 for(int i=0;i<count;++i){const VisualEntity& entity=entities[i];
  if(!nickel::visual::nametag_target(entity.player,entity.living,request.mobs))continue;
  uintptr_t registry{};uint32_t eid{};if(!ActorEntity(entity.actor,&registry,&eid))continue;
  if(Has(registry,eid,"LocalPlayerComponent"))continue;
  VisualIdentity identity{};ReadVisualIdentity(registry,eid,entity.actor,&identity);
  std::string label=nickel::visual::nametag_name(entity.player,request.useDisplayName,identity.name,identity.display);
  if(label.empty()&&!entity.player){char fallback[128]{};EntityName(context,entity.actor,fallback,sizeof(fallback));label=nickel::visual::clean_nametag(std::string_view(fallback),false);}
  if(label.empty())continue;
  const nickel::visual::Vec3 low{entity.minimum[0],entity.minimum[1],entity.minimum[2]},high{entity.maximum[0],entity.maximum[1],entity.maximum[2]};
  const nickel::visual::Vec3 anchor=nickel::visual::nametag_anchor(low,high);const float anchorWorld[3]={anchor.x,anchor.y,anchor.z};
  nickel::visual::Point point{};if(!nickel::visual::project(m,origin,anchorWorld,request.width,request.height,&point))continue;
  const float dx=anchor.x-localPos[0],dy=anchor.y-localPos[1],dz=anchor.z-localPos[2];const float distance=std::sqrt(dx*dx+dy*dy+dz*dz);
  const float k=nickel::visual::nametag_scale(distance,request.scale);if(!(k>0))continue;
  NametagRow row{};row.id=entity.id;row.actor=entity.actor;row.player=entity.player;row.healthKnown=entity.healthKnown;
  row.anchorX=point.x;row.anchorY=point.y;row.distance=distance;row.k=k;row.health=entity.health;row.label=std::move(label);
  {uintptr_t hurtStorage{},hurtElement{};int32_t ticks=0;if(Storage(registry,0xd7a3585c,&hurtStorage)&&InStorage(hurtStorage,eid,4,&hurtElement)&&Read(hurtElement,&ticks))row.hurtTicks=ticks;}
  frame->index[entity.actor]=frame->rows.size();frame->rows.push_back(std::move(row));
 }
 if(request.equipment&&!frame->rows.empty()){
  uint64_t gui{};float size[6];
  if(Read(context.client+0x668,&gui)&&gui&&Floats(gui+0x40,size,6)&&size[0]>0&&size[1]>0&&size[4]>0&&size[5]>0){
   uint64_t game{},first{},second{},third{},screenContext{},clientOfUi{};
   if(Read(context.base+0x128c0f90,&game)&&Read(game+8,&first)&&Read(first+0x20,&second)&&Read(second+0x48,&third)&&third&&Read((uintptr_t)ui+0x10,&screenContext)&&screenContext&&Read((uintptr_t)ui+8,&clientOfUi)&&clientOfUi){
    const float ratioX=size[4]/size[0],ratioY=size[5]/size[1];alignas(16) uint8_t ctx[0x300];memset(ctx,0,sizeof(ctx));
    reinterpret_cast<void*(*)(uint8_t*,uint64_t,uint64_t,uint64_t)>(context.base+0x57e2560)(ctx,screenContext,clientOfUi,third);
    for(auto& row:frame->rows){
     uintptr_t registry{};uint32_t eid{};uint64_t stacks[5]{};int n=0;
     if(ActorEntity(row.actor,&registry,&eid)){uintptr_t storage{},equipment{};uint64_t hand{},armor{};
      if(Storage(registry,0xb06141a9u,&storage)&&InStorage(storage,eid,0x10,&equipment)&&Read(equipment,&hand)&&Read(equipment+8,&armor)){
       if(hand)if(const uint64_t get=Virtual(context,hand,0x38)){const uint64_t stack=reinterpret_cast<uint64_t(*)(uint64_t,int)>(get)(hand,0);if(ValidStack(stack))stacks[n++]=stack;}
       if(armor&&n<5){const uint64_t get=Virtual(context,armor,0x38),slotsFn=Virtual(context,armor,0xa0);if(get&&slotsFn){const int slots=reinterpret_cast<int(*)(uint64_t)>(slotsFn)(armor);for(int s=0;s<slots&&s<4&&n<5;++s){const uint64_t stack=reinterpret_cast<uint64_t(*)(uint64_t,int)>(get)(armor,s);if(ValidStack(stack))stacks[n++]=stack;}}}}}
     if(!n)continue;
     const float k=row.k,side=24*k,pitch=26*k,rowWidth=n*side+(n-1)*2*k,startX=row.anchorX-rowWidth*.5f,topY=row.anchorY-(22+3+24)*k;
     for(int i=0;i<n;++i){const float x=startX+i*pitch,y=topY;if(x+side<0||x>request.width||y+side<0||y>request.height)continue;
      RenderIcon(context,ui,ctx,row.actor,stacks[i],x*ratioX,y*ratioY,side*ratioX/16,1.f);
      NametagIcon icon{};icon.x=x;icon.y=y;icon.side=side;icon.ratioX=ratioX;icon.ratioY=ratioY;
      if(request.durability){const uint64_t item=ItemOf(stacks[i]),top=Virtual(context,item,0x120);if(top){const int maximum=reinterpret_cast<short(*)(uint64_t)>(top)(item);if(maximum>0){uint64_t data{};const int damage=Read(stacks[i]+0x10,&data)&&data?NbtInt(data,"Damage"):0;const int64_t left=(int64_t)maximum-damage;const int remaining=left<0?0:left>maximum?maximum:(int)left;icon.maximum=maximum;icon.remaining=remaining;}}}
      row.icons.push_back(icon);}
    }
    reinterpret_cast<void(*)(uint8_t*)>(context.base+0x57e29e0)(ctx);
   }
  }
 }
 {std::lock_guard<std::mutex> lock(shared.guard);if(shared.generation==generation&&shared.request.enabled&&!shared.fault)shared.frame=frame;}InterlockedExchange(&shared.seen,1);
}
inline void NametagsFault(const char* message){
 auto& shared=Nametags();if(InterlockedExchange(&shared.fault,1)!=0)return;
 void(*log)(const char*)=nullptr;{std::lock_guard<std::mutex> lock(shared.guard);log=shared.log;shared.frame.reset();}
 if(log)log(message);
}
inline void NamesGuarded(void* ui){__try{NamesWork(ui);}__except(EXCEPTION_EXECUTE_HANDLER){NametagsFault("Nametags: a fault was caught in the game render path; nametags are disabled until the game restarts");}}
inline bool NametagHiddenWork(uintptr_t actor){
 auto& shared=Nametags();if(shared.fault)return false;
 NametagRequest request{};std::shared_ptr<const NametagFrame> frame;{std::lock_guard<std::mutex> lock(shared.guard);request=shared.request;frame=shared.frame;}
 if(!request.enabled||!request.hideVanilla||!frame)return false;
 const uint64_t now=GetTickCount64();auto& local=nametagLocal;
 if(!local.tick||local.tick!=now){Context context{};if(!Local(&context))return false;uint32_t serial{};{std::lock_guard<std::mutex> lock(Shared().guard);serial=Shared().worldSerial;}
  int dimension{};if(!ReadDimension(&dimension))return false;local.tick=now;local.actor=context.actor;local.serial=serial;local.dimension=dimension;}
 if(!nickel::visual::nametag_frame_usable(request.enabled,shared.fault!=0,now,frame->tick,frame->actor,local.actor,frame->worldSerial,local.serial,frame->dimension,local.dimension))return false;
 auto found=frame->index.find(actor);if(found==frame->index.end())return false;
 const NametagRow& row=frame->rows[found->second];uint64_t id{};if(!RuntimeId(actor,&id)||id!=row.id)return false;
 return row.player||request.mobs;
}
inline bool NametagHiddenGuarded(uintptr_t actor){__try{return NametagHiddenWork(actor);}__except(EXCEPTION_EXECUTE_HANDLER){NametagsFault("Nametags: a fault was caught while checking a vanilla nametag; nametags are disabled until the game restarts");return false;}}
inline void ConfigureNametags(const NametagRequest& request,void(*log)(const char*)){
 auto& shared=Nametags();
 {std::lock_guard<std::mutex> lock(shared.guard);if(!NametagRequestSame(shared.request,request)){++shared.generation;shared.frame.reset();}shared.request=request;shared.log=log;}
 static std::once_flag once;std::call_once(once,[]{namesScreenWork.store(NamesGuarded,std::memory_order_release);customNametagHidden.store(NametagHiddenGuarded,std::memory_order_release);});
}
inline std::shared_ptr<const NametagFrame> SnapshotNametags(){auto& shared=Nametags();std::lock_guard<std::mutex> lock(shared.guard);return shared.frame;}
}
