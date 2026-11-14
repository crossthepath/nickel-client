// Game event hooks for the HUD modules that need them (Reach Display, Combo Counter, Target HUD, Totem Pop Counter).
// Game RVAs are for Minecraft 1.26.62.01:
// #26  0x37692b0  GameMode attack: rcx = mode (player at +8), rdx = target actor, r8b, r9; returns a bool
// #193 0x23a7320  ActorEventPacket receive (id 0x1b): r9 = shared_ptr<Packet>&; packet +0x30 actor runtime id,
// +0x38 event type (2 hurt, 3 death, 0x41 totem used)
// #206 0x23a6fb0  StartGamePacket receive (id 0x0b): entering a world
// The detours only read (under SEH) and always call the game function with its arguments unchanged; nothing is cancelled
// or modified. Hooks are installed only if the game image has the expected size and the first 16 bytes of every target
// match the expected prologue. Reach is measured in the attack hook; the "local player attack" test compares mode+8
// with the local actor.
#pragma once
#include "game_read.hpp"
#include "game_packet_hooks.hpp"
#include <MinHook.h>
#include <mutex>
#include <vector>
#include <unordered_map>

namespace game {
inline bool ActorEntity(uintptr_t actor,uintptr_t* registry,uint32_t* entity){uint64_t found{};uint32_t id{};if(!actor||!Read(actor+0x10,&found)||!found||!Read(actor+0x18,&id)||(id&0x3ffff)==0x3ffff)return false;*registry=found;*entity=id;return true;}
inline bool InStorage(uintptr_t storage,uint32_t entity,uint32_t stride,uintptr_t* element){uint32_t packed{};uint64_t pages{},page{};if(!Packed(storage,entity,&packed)||!Read(storage+0x50,&pages)||!Read(pages+((packed>>4)&0x3ff8),&page)||!page)return false;*element=page+(packed&0x7f)*stride;return true;}
inline bool Has(uintptr_t registry,uint32_t entity,const char* component){uintptr_t storage{};uint32_t packed{};return Storage(registry,Fnv(component),&storage)&&Packed(storage,entity,&packed);}
inline bool RuntimeId(uintptr_t actor,uint64_t* id){uintptr_t registry{},element{};uint32_t entity{};return ActorEntity(actor,&registry,&entity)&&Element(registry,entity,"RuntimeIDComponent",8,&element)&&Read(element,id);}
// Actor for a runtime id: walks the packed RuntimeIDComponent entities ([storage+0x20, +0x28), 4 bytes each),
// then resolves ActorOwnerComponent.
inline bool ActorByRuntimeId(uintptr_t registry,uint64_t id,uintptr_t* actor){
 uintptr_t ids{},owners{};uint64_t begin{},end{};if(!Storage(registry,Fnv("RuntimeIDComponent"),&ids)||!Storage(registry,Fnv("ActorOwnerComponent"),&owners)||!Read(ids+0x20,&begin)||!Read(ids+0x28,&end)||end<begin||(end-begin)%4||(end-begin)>4*65536)return false;
 // The packed array and the element pages are parallel (element i = pages[i>>7]+(i&0x7f)*8), so the id is read straight by index; the page pointer is cached per 128 entries.
 uint64_t pages{};if(!Read(ids+0x50,&pages))return false;uint64_t cachedPage{},page{};bool havePage=false;
 for(uint64_t at=begin;at<end;at+=4){uint32_t entity{};uintptr_t slot{};uint64_t value{},found{};if(!Read(at,&entity))return false;if((entity&0x3ffff)==0x3ffff)continue;
  const uint64_t index=(at-begin)/4,block=index>>7;if(!havePage||block!=cachedPage){havePage=true;cachedPage=block;if(!Read(pages+block*8,&page))page=0;}
  if(!page||!Read(page+(index&0x7f)*8,&value)||value!=id)continue;
  if(!InStorage(owners,entity,8,&slot)||!Read(slot,&found)||!found)return false;*actor=found;return true;}
 return false;
}
inline bool Floats(uintptr_t address,float* out,int count){for(int i=0;i<count;++i)if(!Read(address+i*4,&out[i])||!std::isfinite(out[i])||std::fabs(out[i])>=1e8f)return false;return true;}
// MSVC std::string (16-byte inline buffer, size at +0x10, capacity at +0x18); non-printable characters are replaced
inline bool Text(uintptr_t address,char* out,size_t capacity){uint64_t size{},room{},again{};if(!Read(address+0x10,&size)||!Read(address+0x18,&room)||size>room||size>=capacity)return false;uint64_t data=address;if(room>15&&(!Read(address,&data)||!data))return false;
 for(uint64_t i=0;i<size;++i){char byte{};if(!Read(data+i,&byte))return false;out[i]=(unsigned char)byte>=32?byte:'?';}out[size]=0;return Read(address+0x10,&again)&&again==size;}
// Actor attribute (AttributesComponent, see ReadHealth): 6 = health, 0xd = absorption
inline bool AttributeAt(uintptr_t element,uint32_t wanted,float* current,float* maximum){
 uint64_t begin{},end{},table{};if(!Read(element,&begin)||!Read(element+8,&end)||!Read(element+0x18,&table)||end<begin||!table)return false;const uint64_t count=(end-begin)/4;if(!count||count>256)return false;
 for(uint64_t i=0;i<count;++i){uint32_t id{};if(!Read(begin+i*4,&id))return false;if(id!=wanted)continue;return Read(table+i*0x88+0x7c,current)&&Read(table+i*0x88+0x78,maximum)&&std::isfinite(*current)&&std::isfinite(*maximum);}
 return false;
}
inline bool Attribute(uintptr_t registry,uint32_t entity,uint32_t wanted,float* current,float* maximum){uintptr_t element{};return Element(registry,entity,"AttributesComponent",0x50,&element)&&AttributeAt(element,wanted,current,maximum);}
// Hit distance: from the player's eye (RenderPosition, y - heightOffset + 1.62001) to the closest point of the target's
// box (centred on its RenderPosition, AABB dimensions at actor+0x248).
inline bool Reach(uintptr_t local,uintptr_t target,float* distance){
 uintptr_t lr{},tr{},lp{},tp{},lo{},to{};uint32_t le{},te{};uint64_t box{};float eye[3],pos[3],lift{},drop{},b[6];
 if(!ActorEntity(local,&lr,&le)||!ActorEntity(target,&tr,&te)||!Element(lr,le,"RenderPositionComponent",12,&lp)||!Element(tr,te,"RenderPositionComponent",12,&tp)||!Element(lr,le,"OffsetsComponent",0x44,&lo)||!Element(tr,te,"OffsetsComponent",0x44,&to))return false;
 if(!Read(target+0x248,&box)||!box||!Floats(lp,eye,3)||!Floats(tp,pos,3)||!Floats(lo,&lift,1)||!Floats(to,&drop,1)||!Floats(box,b,6))return false;
 eye[1]=eye[1]-lift+1.62001f;const float halfX=(b[3]-b[0])/2,halfZ=(b[5]-b[2])/2,low=pos[1]-drop,high=low+(b[4]-b[1]);
 const float x=std::fmin(std::fmax(eye[0],pos[0]-halfX),pos[0]+halfX)-eye[0],y=std::fmin(std::fmax(eye[1],low),high)-eye[1],z=std::fmin(std::fmax(eye[2],pos[2]-halfZ),pos[2]+halfZ)-eye[2];
 *distance=std::sqrt(x*x+y*y+z*z);return std::isfinite(*distance)&&*distance<64;
}

struct PacketEvent{uint64_t actor;uint8_t type;uint64_t tick;};
struct Events{std::mutex guard;uint64_t attackTarget=0,attackTick=0;uint32_t attackSerial=0,worldSerial=0;float reach=-1;std::vector<PacketEvent> packets;};
inline Events& Shared(){static Events* events=new Events;return *events;}
using AttackFunction=bool(*)(void*,void*,bool,void*);using PacketFunction=void(*)(void*,void*,void*,void*);
inline AttackFunction attackOriginal=nullptr;inline PacketFunction actorEventOriginal=nullptr,startGameOriginal=nullptr;
inline void(*volatile combatAttack)()=nullptr;   // Combat: an attack went through (Reach redraws its reach)
inline bool(*volatile combatCancel)(uintptr_t local,uintptr_t target)=nullptr;   // Combat: true cancels the attack (Hit Select)
inline bool(*cancelAttack)(uintptr_t local,uintptr_t target)=nullptr;inline bool(*volatile scriptAttack)(uintptr_t local,uintptr_t target)=nullptr;   // Luau scripts: attack event
// Predicted health (Target HUD): listeners installed by InstallPrediction()
inline void(*predictAttack)(uintptr_t local,uintptr_t target)=nullptr;inline void(*predictActorEvent)(uint64_t id,uint8_t type)=nullptr;inline void(*predictLevelEvent)(uint64_t packet)=nullptr;
inline bool AttackDetour(void* mode,void* target,bool flag,void* extra){
 uint64_t player{},id{};Context context{};
 if(Read((uintptr_t)mode+8,&player)&&player&&Local(&context)&&player==context.actor){
  if(cancelAttack&&cancelAttack(context.actor,(uintptr_t)target))return false;
  if(auto sa=scriptAttack)if(sa(context.actor,(uintptr_t)target))return false;
  if(auto cc=combatCancel)if(cc(context.actor,(uintptr_t)target))return false;
  if(auto predict=predictAttack)predict(context.actor,(uintptr_t)target);
  if(auto ca=combatAttack)ca();
  if(RuntimeId((uintptr_t)target,&id)){float reach=-1;if(!Reach(context.actor,(uintptr_t)target,&reach))reach=-1;auto& events=Shared();std::lock_guard<std::mutex> lock(events.guard);events.attackTarget=id;events.attackTick=GetTickCount64();events.reach=reach;++events.attackSerial;}}
 return attackOriginal(mode,target,flag,extra);
}
inline void ActorEventDetour(void* self,void* sender,void* callback,void* shared){
 if(ReceiveFilter(shared))return;   // PacketReceiveEvent cancelled by a Utility subscriber
 uint64_t packet{},actor{};uint8_t type{};
 if(Read((uintptr_t)shared,&packet)&&packet&&Read(packet+0x30,&actor)&&Read(packet+0x38,&type)&&(type==2||type==3)){if(auto predict=predictActorEvent)predict(actor,type);}
 if(Read((uintptr_t)shared,&packet)&&packet&&Read(packet+0x30,&actor)&&Read(packet+0x38,&type)&&(type==2||type==3||type==0x41)){auto& events=Shared();std::lock_guard<std::mutex> lock(events.guard);if(events.packets.size()<512)events.packets.push_back({actor,type,GetTickCount64()});}
 actorEventOriginal(self,sender,callback,shared);
}
inline PacketFunction levelEventOriginal=nullptr;
// #195 0x23a7260: LevelEventPacket receive (id 0x19); packet +0x30 event id, +0x34 position, +0x40 data
inline void LevelEventDetour(void* self,void* sender,void* callback,void* shared){if(ReceiveFilter(shared))return;uint64_t packet{};if(auto predict=predictLevelEvent)if(Read((uintptr_t)shared,&packet)&&packet)predict(packet);levelEventOriginal(self,sender,callback,shared);}
inline void StartGameDetour(void* self,void* sender,void* callback,void* shared){if(ReceiveFilter(shared))return;{auto& events=Shared();std::lock_guard<std::mutex> lock(events.guard);++events.worldSerial;events.packets.clear();}startGameOriginal(self,sender,callback,shared);}
// 1: hooks installed; -1: refused (wrong game build, unexpected bytes or MinHook failure). Only tried once.
inline int InstallEvents(void(*log)(const char*)){
 static int state=0;if(state)return state;state=-1;uintptr_t base{};uint32_t size{};if(!Supported(&base,&size)){if(log)log("Game events: not the supported Minecraft image, no game hook installed");return state;}
 struct Hook{uint32_t rva;uint8_t bytes[16];void* detour;void** original;};
 const Hook hooks[4]={
  {0x37692b0,{0x55,0x41,0x57,0x41,0x56,0x41,0x54,0x56,0x57,0x53,0x48,0x81,0xec,0xf0,0x01,0x00},(void*)AttackDetour,(void**)&attackOriginal},
  {0x23a7320,{0x4c,0x89,0xc1,0x4d,0x8b,0x01,0x48,0x8b,0x01,0x48,0x8b,0x80,0xd8,0x06,0x00,0x00},(void*)ActorEventDetour,(void**)&actorEventOriginal},
  {0x23a6fb0,{0x4c,0x89,0xc1,0x4d,0x8b,0x01,0x48,0x8b,0x01,0x48,0x8b,0x80,0x98,0x07,0x00,0x00},(void*)StartGameDetour,(void**)&startGameOriginal},
  {0x23a7260,{0x4c,0x89,0xc1,0x4d,0x8b,0x01,0x48,0x8b,0x01,0x48,0x8b,0x80,0x08,0x07,0x00,0x00},(void*)LevelEventDetour,(void**)&levelEventOriginal}};
 for(auto& hook:hooks)for(int i=0;i<16;++i){uint8_t byte{};if(!Read(base+hook.rva+i,&byte)||byte!=hook.bytes[i]){if(log)log("Game events: unexpected bytes at a hook target, no game hook installed");return state;}}
 for(auto& hook:hooks)if(MH_CreateHook((void*)(base+hook.rva),hook.detour,hook.original)!=MH_OK){if(log)log("Game events: MinHook could not create a hook");return state;}
 for(auto& hook:hooks)if(MH_EnableHook((void*)(base+hook.rva))!=MH_OK){if(log)log("Game events: MinHook could not enable a hook");return state;}
 if(log)log("Game events: attack, ActorEvent, StartGame and LevelEvent hooks installed");state=1;return state;
}

// ---- Player list (Player List module): object = *(local player+0x200); container = object->vtable[0x9f8](object)
// (a game call, guarded like Ping); linked list: head at +8, size at +0x10; next node = *node; name = std::string at
// node+0x38, kept when non-empty. If the function is a plain accessor ("lea rax,[rcx+d]; ret" or "mov rax,[rcx+d]; ret"),
// the result is computed without calling it. `rva` is the target function, for the log.
struct PlayerNames{int total;int count;char names[50][64];};
inline int ReadPlayerNames(PlayerNames* out,uint64_t* rva){
 Context context{};out->total=out->count=0;*rva=0;if(!Local(&context))return -1;uint64_t level{},table{},getter{},container{},head{},size{},node{};
 if(!Read(context.actor+0x200,&level)||!level||!Read(level,&table)||table<context.base||table>=context.base+context.size||!Read(table+0x9f8,&getter)||!InCode(context,getter))return -1;*rva=getter-context.base;
 uint8_t code[8]{};for(int i=0;i<8;++i)if(!Read(getter+i,&code[i]))return -1;int32_t offset{};memcpy(&offset,code+3,4);
 if(code[0]==0x48&&code[1]==0x8d&&code[2]==0x81&&code[7]==0xc3)container=level+offset;
 else if(code[0]==0x48&&code[1]==0x8b&&code[2]==0x81&&code[7]==0xc3){if(!Read(level+offset,&container))return -1;}
 else if(!CallGetter(getter,level,&container))return -1;
 if(!container||!Read(container+8,&head)||!head||!Read(container+0x10,&size)||size>4096||!Read(head,&node))return -1;out->total=(int)size;
 for(int guard=0;guard<4096&&node&&node!=head;++guard){uint64_t length{};if(!Read(node+0x48,&length))return -1;if(length&&out->count<50&&Text(node+0x38,out->names[out->count],sizeof(out->names[0])))++out->count;if(!Read(node,&node))return -1;}
 return 1;
}

// ---- Mobs around the player (MiniMap dots), read-only: packed ActorOwnerComponent entities, ActorTypeComponent
// (int32: bit 0x100 = mob, 0xb00 = hostile, 0x13f = player), RenderPositionComponent. Players are deliberately excluded
// (PlayerComponent or type 0x13f): showing other players' positions would be an unfair advantage.
// Invisible entities (ActorDataFlagComponent) are not filtered.
struct Mob{float dx,dz;bool hostile;int type;};
inline int ReadMobs(Mob* out,int capacity,float range){
 Context context{};uintptr_t owners{},types{},positions{},players{},mine{};float origin[3];if(!Local(&context)||!Storage(context.registry,Fnv("ActorOwnerComponent"),&owners)||!Storage(context.registry,Fnv("ActorTypeComponent"),&types)||!Storage(context.registry,Fnv("RenderPositionComponent"),&positions))return 0;
 if(!InStorage(positions,context.entity,12,&mine)||!Floats(mine,origin,3))return 0;const bool hasPlayers=Storage(context.registry,Fnv("PlayerComponent"),&players);
 uint64_t begin{},end{};if(!Read(owners+0x20,&begin)||!Read(owners+0x28,&end)||end<begin||(end-begin)%4||(end-begin)>4*65536)return 0;int count=0;
 for(uint64_t at=begin;at<end&&count<capacity;at+=4){uint32_t entity{},packed{};uintptr_t element{};int32_t type{};float position[3];if(!Read(at,&entity))return count;if(entity>=0xfffc0000u||entity==context.entity)continue;
  if(!InStorage(types,entity,4,&element)||!Read(element,&type)||type==0x13f||!((type>>8)&1)||(hasPlayers&&Packed(players,entity,&packed)))continue;
  if(!InStorage(positions,entity,12,&element)||!Floats(element,position,3))continue;const float dx=position[0]-origin[0],dz=position[2]-origin[2];if(std::fabs(dx)>range||std::fabs(dz)>range)continue;out[count++]={dx,dz,(type&0xb00)==0xb00,type};}
 return count;
}

// ---- Health of nearby entities (Damage Indicators): damage is detected from the health difference between two frames.
// Read-only: runtime id, health (attribute 6) + absorption (0xd), centre (RenderPosition), top of the box.
struct Vital{uint64_t id;float x,top,z,health;bool player;};
inline int ReadVitals(Vital* out,int capacity,float range){
 Context context{};uintptr_t owners{},positions{},mine{};float origin[3];if(!Local(&context)||!Storage(context.registry,Fnv("ActorOwnerComponent"),&owners)||!Storage(context.registry,Fnv("RenderPositionComponent"),&positions)||!InStorage(positions,context.entity,12,&mine)||!Floats(mine,origin,3))return 0;
 uint64_t begin{},end{};if(!Read(owners+0x20,&begin)||!Read(owners+0x28,&end)||end<begin||(end-begin)%4||(end-begin)>4*65536)return 0;int count=0;
 for(uint64_t at=begin;at<end&&count<capacity;at+=4){uint32_t entity{};uintptr_t element{},slot{};uint64_t actor{},box{};float position[3],health{},maximum{},extra{},unused{},drop{},b[6];if(!Read(at,&entity))return count;if(entity>=0xfffc0000u||entity==context.entity)continue;
  if(!InStorage(positions,entity,12,&element)||!Floats(element,position,3)||std::fabs(position[0]-origin[0])>range||std::fabs(position[1]-origin[1])>range||std::fabs(position[2]-origin[2])>range)continue;
  if(!Attribute(context.registry,entity,6,&health,&maximum)||!(maximum>0))continue;if(Attribute(context.registry,entity,0xd,&extra,&unused)&&extra>0)health+=extra;
  Vital vital{};if(!Element(context.registry,entity,"RuntimeIDComponent",8,&element)||!Read(element,&vital.id))continue;vital.x=position[0];vital.z=position[2];vital.top=position[1]+.25f;vital.health=health;vital.player=Has(context.registry,entity,"PlayerComponent");
  if(InStorage(owners,entity,8,&slot)&&Read(slot,&actor)&&actor&&Read(actor+0x248,&box)&&box&&Floats(box,b,6)&&Element(context.registry,entity,"OffsetsComponent",0x44,&element)&&Floats(element,&drop,1))vital.top=position[1]-drop+(b[4]-b[1]);
  out[count++]=vital;}
 return count;
}
// Player dimension (DimensionTypeComponent 0xaf993816: 0 Overworld, 1 Nether, 2 End)
inline bool ReadDimension(int* dimension){Context context{};uintptr_t storage{},element{};int32_t value{};if(!Local(&context)||!Storage(context.registry,0xaf993816u,&storage)||!InStorage(storage,context.entity,4,&element)||!Read(element,&value))return false;*dimension=value;return true;}

// ---- Per-frame reads for the HUD
struct Target{bool player;char name[96];float health,maximum,absorption,hurt,distance;uintptr_t actor;};
// Virtual function of a game object, if its vtable is in the image and the function in code (0 otherwise)
inline uint64_t Virtual0(const Context& context,uint64_t object,uint32_t offset){uint64_t table{},function{};return object&&Read(object,&table)&&table>=context.base&&table<context.base+context.size&&Read(table+offset,&function)&&InCode(context,function)?function:0;}
inline bool LocalIdentity(uint64_t* id,char* name,size_t capacity){Context context{};if(!Local(&context))return false;uintptr_t element{};if(!Element(context.registry,context.entity,"RuntimeIDComponent",8,&element)||!Read(element,id))return false;if(name&&!Text(context.actor+0xbe8,name,capacity))name[0]=0;return true;}
// Name of a PLAYER from its runtime id (std::string at actor+0xbe8); false for any other entity
inline bool PlayerName(uint64_t id,char* name,size_t capacity){Context context{};uintptr_t actor{},registry{};uint32_t entity{};return Local(&context)&&ActorByRuntimeId(context.registry,id,&actor)&&ActorEntity(actor,&registry,&entity)&&Has(registry,entity,"PlayerComponent")&&Text(actor+0xbe8,name,capacity)&&name[0];}
// Player list container: level->vtable[0x9f8](level), read without a call when it is a plain accessor
// (see ReadPlayerNames)
inline bool PlayerContainer(const Context& context,uint64_t* container){
 uint64_t level{},table{},getter{};if(!Read(context.actor+0x200,&level)||!level||!Read(level,&table)||table<context.base||table>=context.base+context.size||!Read(table+0x9f8,&getter)||!InCode(context,getter))return false;
 uint8_t code[8]{};for(int i=0;i<8;++i)if(!Read(getter+i,&code[i]))return false;int32_t offset{};memcpy(&offset,code+3,4);
 if(code[0]==0x48&&code[1]==0x8d&&code[2]==0x81&&code[7]==0xc3)*container=level+offset;else if(code[0]==0x48&&code[1]==0x8b&&code[2]==0x81&&code[7]==0xc3){if(!Read(level+offset,container))return false;}else if(!CallGetter(getter,level,container))return false;
 return *container!=0;
}
// Name of a non-player entity: key = actor->vtable[0xf0](actor, &string);
// name = i18n->vtable[0x80](i18n, &string, &key, &empty shared_ptr), i18n = object at game RVA 0x128d1290.
// Two game calls. The returned strings belong to the game's allocator and are never freed (a few dozen bytes); the result
// is cached per entity TYPE (actor vtable), so there is one call per type.
inline void EntityNameCall(uint64_t key,uint64_t i18n,uint64_t translate,uint64_t actor,char* out,size_t capacity){
 __try{uint64_t first[4]{0,0,0,15},second[4]{0,0,0,15},empty[2]{};reinterpret_cast<void*(*)(uint64_t,uint64_t*)>(key)(actor,first);
  reinterpret_cast<void*(*)(uint64_t,uint64_t*,uint64_t*,uint64_t*)>(translate)(i18n,second,first,empty);Text((uintptr_t)second,out,capacity);}__except(EXCEPTION_EXECUTE_HANDLER){out[0]=0;}
}
inline void EntityName(const Context& context,uint64_t actor,char* out,size_t capacity){
 struct Known{uint64_t table;char name[64];};static Known known[32]{};static int next=0;out[0]=0;uint64_t table{};if(!Read(actor,&table)||!table)return;
 for(auto& entry:known)if(entry.table==table){strncpy_s(out,capacity,entry.name,_TRUNCATE);return;}
 const uint64_t key=Virtual0(context,actor,0xf0),i18n=context.base+0x128d1290,translate=Virtual0(context,i18n,0x80);Known& slot=known[next++%32];slot.table=table;slot.name[0]=0;
 if(key&&translate)EntityNameCall(key,i18n,translate,actor,slot.name,sizeof(slot.name));strncpy_s(out,capacity,slot.name,_TRUNCATE);
}
inline bool FillTarget(const Context& context,uintptr_t actor,Target* target){
 uintptr_t registry{},element{};uint32_t entity{};if(!actor||actor==context.actor||!ActorEntity(actor,&registry,&entity)||Has(registry,entity,"LocalPlayerComponent"))return false;target->actor=actor;
 target->player=Has(registry,entity,"PlayerComponent");target->name[0]=0;if(target->player){if(!Text(actor+0xbe8,target->name,sizeof(target->name)))target->name[0]=0;}else EntityName(context,actor,target->name,sizeof(target->name));
 if(!Attribute(registry,entity,6,&target->health,&target->maximum)||!(target->maximum>0)||!(target->health>0))return false;float unused{};target->absorption=0;if(Attribute(registry,entity,0xd,&target->absorption,&unused))target->absorption=std::fmax(0.f,target->absorption);else target->absorption=0;
 int32_t hurt{};target->hurt=Element(registry,entity,"MobHurtTimeComponent",4,&element)&&Read(element,&hurt)?std::fmin(std::fmax(hurt/10.f,0.f),1.f):0;
 uint64_t mine{},theirs{};float a[3],b[3];if(!Read(context.actor+0x240,&mine)||!Read(actor+0x240,&theirs)||!mine||!theirs||!Floats(mine,a,3)||!Floats(theirs,b,3))return false;
 target->distance=std::sqrt((a[0]-b[0])*(a[0]-b[0])+(a[1]-b[1])*(a[1]-b[1])+(a[2]-b[2])*(a[2]-b[2]));return true;
}
inline bool ReadTarget(uint64_t id,Target* target){Context context{};uintptr_t actor{};return Local(&context)&&ActorByRuntimeId(context.registry,id,&actor)&&FillTarget(context,actor,target);}
// Entity under the crosshair (Crosshair Target setting): hit = level->vtable[0xa78](level) (guarded game call);
// if *(int*)(hit+0x18) == 1: registry at hit+0x38 (storages at +0x30), u32 entity at hit+0x48, then ActorOwnerComponent
// -> actor. The weak reference is not locked: it is read immediately, under SEH.
// ---------------------------------------------------------------- Predicted health
// Local player hits record the expected damage; ActorEventPacket 2 / 3 and LevelEventPacket 0x7d2 (thrown healing potion,
// colour ~#F82423 within 12) update the records; stale records are cleaned up; the displayed value depends on
// Health Mode (0 Auto, 1 Server, 2 Predicted).
struct Prediction{float health=0,max=20,damage=0;bool pending=false,attacked=false,hurt=false,server=false;uint64_t attackNs=0,hurtNs=0;};
struct Predictor{std::mutex guard;std::unordered_map<uint64_t,Prediction> map;volatile long fault=0;};
inline Predictor& Predict(){static auto* p=new Predictor;return *p;}
inline uint64_t PredictNow(){LARGE_INTEGER f{},c{};QueryPerformanceFrequency(&f);QueryPerformanceCounter(&c);return (uint64_t)((double)c.QuadPart*1e9/(double)f.QuadPart);}
inline int EffectLevel(uintptr_t registry,uint32_t entity,int id){uintptr_t element{};uint64_t begin{},end{};if(!Element(registry,entity,"MobEffectsComponent",0x18,&element)||!Read(element,&begin)||!Read(element+8,&end)||end<begin||(end-begin)>0xA8*64)return -1;
 for(uint64_t at=begin;at<end;at+=0xA8){int32_t got{},amp{};if(!Read(at,&got)||!Read(at+0x20,&amp))return -1;if(got==id)return amp;}return -1;}
// Node of a CompoundTag std::map<std::string,Tag> (same walk as NbtInt): node address or 0
inline uint64_t NbtNode(uint64_t compound,const char* key){uint64_t head{},stack[64]{};int depth=0;if(!Read(compound+8,&head)||!head||!Read(head+8,&stack[0])||!stack[0])return 0;depth=1;
 for(int visited=0;depth>0&&visited<256;++visited){const uint64_t node=stack[--depth];uint8_t nil{};if(!node||node==head||!Read(node+0x19,&nil)||nil)continue;uint64_t size{},room{},data=node+0x20,left{},right{};if(!Read(node+0x30,&size)||!Read(node+0x38,&room))return 0;if(room>15&&!Read(node+0x20,&data))return 0;
  bool same=size==strlen(key);for(uint64_t i=0;same&&i<size;++i){char byte{};if(!Read(data+i,&byte)||byte!=key[i])same=false;}if(same)return node;if(Read(node,&left)&&Read(node+0x10,&right)&&depth<62){stack[depth++]=left;stack[depth++]=right;}}return 0;}
// Level of an enchantment on an ItemStack ("ench": list of compounds {id: short, lvl: short}), -1 if absent
inline int EnchantLevel(const Context& c,uint64_t stack,int16_t id){uint64_t tag{};if(!Read(stack+0x10,&tag)||!tag)return -1;const uint64_t list=NbtNode(tag,"ench");uint8_t type{};uint64_t b{},e{};if(!list||!Read(list+0x68,&type)||type!=9||!Read(list+0x48,&b)||!Read(list+0x50,&e)||e<b||(e-b)>8*256)return -1;
 for(uint64_t at=b;at<e;at+=8){uint64_t entry{};if(!Read(at,&entry)||!entry)continue;const uint64_t getId=Virtual0(c,entry,0x28);if(!getId)continue;uint64_t kind{};if(!CallGetter(getId,entry,&kind)||(uint8_t)kind!=10)continue;
  const uint64_t idNode=NbtNode(entry,"id"),lvlNode=NbtNode(entry,"lvl");uint8_t t1{},t2{};int16_t v1{},v2{};if(!idNode||!lvlNode||!Read(idNode+0x68,&t1)||t1!=2||!Read(lvlNode+0x68,&t2)||t2!=2||!Read(idNode+0x48,&v1)||v1!=id||!Read(lvlNode+0x48,&v2))continue;return v2;}return -1;}
inline uint64_t StackItem(uint64_t stack){uint64_t holder{},item{};return stack&&Read(stack+8,&holder)&&holder&&Read(holder,&item)?item:0;}
inline uint64_t StackAt(uint64_t function,uint64_t container,int slot){__try{return reinterpret_cast<uint64_t(*)(uint64_t,int)>(function)(container,slot);}__except(EXCEPTION_EXECUTE_HANDLER){return 0;}}
// Expected damage of a hit from "attacker" on "target"
inline float PredictDamage(const Context& c,uintptr_t attacker,uintptr_t target){
 uintptr_t ar{},tr{};uint32_t ae{},te{};if(!ActorEntity(attacker,&ar,&ae)||!ActorEntity(target,&tr,&te))return 0;float base=1,unused{};if(!Attribute(ar,ae,0xc,&base,&unused))base=1;
 float weapon=0;uint64_t inventory{};if(Read(attacker+0x5e0,&inventory)&&inventory){uint64_t container{};int32_t slot{};if(Read(inventory+0xb8,&container)&&container&&Read(inventory+0x10,&slot)){const uint64_t getItem=Virtual0(c,container,0x38);
  const uint64_t stack=getItem?StackAt(getItem,container,slot):0;const uint64_t item=StackItem(stack);
  if(item){const int sharp=EnchantLevel(c,stack,9);weapon=sharp>=0?sharp*1.25f:0.f;const uint64_t dmg=Virtual0(c,item,0x128),bonus=Virtual0(c,item,0x130);
   if(dmg)weapon+=(float)reinterpret_cast<int(*)(uint64_t)>(dmg)(item);if(bonus)weapon+=reinterpret_cast<float(*)(uint64_t,uintptr_t)>(bonus)(item,attacker);}}}
 float damage=base+weapon;const int strength=EffectLevel(ar,ae,5);if(strength>=0)for(int i=0;i<=strength;++i)damage=damage*1.3f+1;
 float out=damage;const int weakness=EffectLevel(ar,ae,0x12);if(weakness>=0){for(int i=weakness+2;i>1;--i){damage=damage*.8f-.5f;out=0;if(damage<0)break;out=damage;}}
 {uintptr_t element{},storage{};uint32_t packed{};float fall=0;const bool falling=Storage(ar,0xce6b34f6u,&storage)&&InStorage(storage,ae,4,&element)&&Read(element,&fall)&&fall>0;
  const bool ground=Storage(ar,0xc29078a0u,&storage)&&Packed(storage,ae,&packed),water=Storage(ar,0x78e89f39u,&storage)&&Packed(storage,ae,&packed);
  if(falling&&!ground&&!water&&EffectLevel(ar,ae,0xf)==-1)out*=1.5f;}
 const float hit=truncf(out);if(!(hit>0))return 0;
 float armor=0,toughness=2,protect=0;{uintptr_t equipment{};uint64_t armorBox{};if(Element(tr,te,"ActorEquipmentComponent",0x10,&equipment)&&Read(equipment+8,&armorBox)&&armorBox){const uint64_t sizeOf=Virtual0(c,armorBox,0xa0),getItem=Virtual0(c,armorBox,0x38);
  int n=0;uint64_t count{};if(sizeOf&&CallGetter(sizeOf,armorBox,&count))n=(int)(uint32_t)count;float tough=0;int levels=0;
  for(int i=0;i<n&&i<8&&getItem;++i){const uint64_t stack=StackAt(getItem,armorBox,i);const uint64_t item=StackItem(stack);if(!item)continue;const uint64_t av=Virtual0(c,item,0x1d0),tv=Virtual0(c,item,0x1d8);
   uint64_t v{};if(av&&CallGetter(av,item,&v))armor+=(float)(int32_t)v;if(tv&&CallGetter(tv,item,&v))tough+=(float)(int32_t)v;const int pl=EnchantLevel(c,stack,0);levels+=pl>0?pl:0;}
  if(n>0){toughness=tough*.25f+2;protect=(float)std::min(levels,20)*.04f;}}}
 const float d=armor-hit/toughness;const float reduce=d<=20?std::max(armor*.2f,d)/25:.8f;float resist=1;const int r=EffectLevel(tr,te,0xb);if(r>=0)resist=r<4?(r+1)*-.2f+1:0.f;
 return (1-protect)*hit*(1-reduce)*resist;}
inline void PredictAttackWork(uintptr_t local,uintptr_t target){Context c{};uint64_t id{};if(!Local(&c)||!RuntimeId(target,&id))return;uintptr_t tr{};uint32_t te{};float max=20,cur{};
 if(!(ActorEntity(target,&tr,&te)&&Attribute(tr,te,6,&cur,&max)&&std::isfinite(max)&&max>0))max=20;const float damage=PredictDamage(c,local,target);auto& p=Predict();std::lock_guard<std::mutex> lock(p.guard);
 const bool fresh=p.map.find(id)==p.map.end();Prediction& r=p.map[id];if(fresh)r.health=max;r.max=max;r.damage=damage;r.pending=true;r.attackNs=PredictNow();r.attacked=true;}
inline void PredictAttackGuarded(uintptr_t local,uintptr_t target){if(Predict().fault)return;__try{PredictAttackWork(local,target);}__except(EXCEPTION_EXECUTE_HANDLER){InterlockedExchange(&Predict().fault,1);}}
inline void PredictActorEvent(uint64_t id,uint8_t type){auto& p=Predict();std::lock_guard<std::mutex> lock(p.guard);auto f=p.map.find(id);if(f==p.map.end())return;Prediction& r=f->second;const uint64_t now=PredictNow();
 if(type==3){r.health=0;r.pending=false;r.hurtNs=now;r.hurt=true;return;}
 const bool pending=r.pending;r.pending=false;if(!pending)return;if(r.attacked&&(double)(now-r.attackNs)/1e6>1000.0)return;if(r.health<=0)r.health=r.max;
 const float x=r.damage,y=r.health;float applied=x<y?std::floor(x):x;if(!(1.f<x))applied=x;   // Damage actually applied to the predicted health
 r.health=std::max(0.f,r.health-applied);r.hurtNs=now;r.hurt=true;}
inline void PredictLevelWork(uint64_t packet){int32_t event{};uint32_t data{};float pos[3];if(!Read(packet+0x30,&event)||event!=0x7d2||!Read(packet+0x40,&data)||!Floats(packet+0x34,pos,3))return;
 if(std::abs((int)((data>>16)&0xff)-0xf8)>12||std::abs((int)((data>>8)&0xff)-0x24)>12||std::abs((int)(data&0xff)-0x23)>12)return;Context c{};if(!Local(&c))return;auto& p=Predict();std::lock_guard<std::mutex> lock(p.guard);
 for(auto& e:p.map){Prediction& r=e.second;if(r.server||!(r.health>0))continue;float heal=0;uintptr_t actor{};uint64_t sv{};float at[3];if(ActorByRuntimeId(c.registry,e.first,&actor)&&actor&&Read(actor+0x240,&sv)&&sv&&Floats(sv,at,3)){const float dx=pos[0]-at[0],dy=pos[1]-at[1],dz=pos[2]-at[2],dist=std::sqrt(dx*dx+dy*dy+dz*dz);heal=dist<4?(dist*-.25f+1)*8:0;}
  r.health=std::min(r.health+heal,r.max);}}
inline void PredictLevelGuarded(uint64_t packet){if(Predict().fault)return;__try{PredictLevelWork(packet);}__except(EXCEPTION_EXECUTE_HANDLER){InterlockedExchange(&Predict().fault,1);}}
// Without a local player everything is cleared; otherwise a record hit more than 30 s ago is removed
inline void PredictCleanup(bool local){auto& p=Predict();std::lock_guard<std::mutex> lock(p.guard);if(!local){p.map.clear();return;}const uint64_t now=PredictNow();
 for(auto it=p.map.begin();it!=p.map.end();){const Prediction& r=it->second;if(r.hurt&&(double)(now-r.hurtNs)/1e6>30000.0&&r.attacked&&(double)(now-r.attackNs)/1e6>30000.0)it=p.map.erase(it);else ++it;}}
// Displayed health by Health Mode (0 Auto: server health once it has dropped, otherwise the prediction;
// 1 Server; 2 Predicted: prediction as soon as a hit is recorded)
inline float PredictedHealth(uint64_t id,float serverHealth,float max,int mode){if(mode==1)return serverHealth;auto& p=Predict();std::lock_guard<std::mutex> lock(p.guard);auto f=p.map.find(id);
 if(mode==0){if(max>serverHealth){p.map[id].server=true;return serverHealth;}if(f==p.map.end()||f->second.server)return serverHealth;}
 if(f==p.map.end())return mode==2?max:serverHealth;const float h=f->second.health;return std::max(0.f,max<h?max:h);}
inline void InstallPrediction(){predictAttack=PredictAttackGuarded;predictActorEvent=PredictActorEvent;predictLevelEvent=PredictLevelGuarded;}
inline bool ReadCrosshairTarget(Target* target){
 Context context{};uint64_t level{},hit{},holder{},actor{};int32_t kind{};uint32_t entity{};uintptr_t owners{},slot{};if(!Local(&context)||!Read(context.actor+0x200,&level)||!level)return false;
 const uint64_t getter=Virtual0(context,level,0xa78);if(!getter||!CallGetter(getter,level,&hit)||!hit||!Read(hit+0x18,&kind)||kind!=1||!Read(hit+0x38,&holder)||!holder||!Read(hit+0x48,&entity))return false;
 if(!Storage(holder+0x30,Fnv("ActorOwnerComponent"),&owners)||!InStorage(owners,entity,8,&slot)||!Read(slot,&actor)||!actor)return false;return FillTarget(context,actor,target);
}
// Debug Overlay values:
// - pitch = *(actor+0x250)[0];
// - Entities = live entries (< 0xfffc0000) of the ActorOwnerComponent set (0x85b93800);
// - Game Mode = ActorGameTypeComponent (0x88d3eddf, int); Health / Hunger = attributes 6 and 1 (+0x7c);
// - Ground = OnGroundFlagComponent; Sneak / Sprint = bits 1 / 3 of ActorDataFlagComponent (0xc67426f3,
// 0x18-byte element);
// Fly = bit 0 of the u32 at +4 of component 0x6de78dc5 (0x10-byte element);
// - GUI: GuiData = *(client+0x668), floats at +0x40: [0],[1] = display size, [0]/[4] = UI scale;
// - target: hit = level->vtable[0xa78]; type 0 = block (face +0x1c, position +0x20), name = region->vtable[0x10](pos)
// -> block+0x68 -> name +0xe8; type 1 = entity (player name +0xbe8, otherwise translated name),
// distance between the +0x240 positions.
struct DebugInfo{bool pitchKnown=false;float pitch=0;int entities=-1;int gameMode=-2;bool healthKnown=false,hungerKnown=false;float health=0,hunger=0;
 bool ground=false,sneak=false,sprint=false,fly=false,flagsKnown=false;bool gui=false;float display[2]{},guiScale=0;
 int target=0;int32_t block[3]{};uint8_t face=0;char blockName[96]{};char entityName[64]{};float entityDistance=0;};
inline uint64_t DebugBlockAt(uint64_t function,uint64_t region,const int32_t* pos){__try{return reinterpret_cast<uint64_t(*)(uint64_t,const int32_t*)>(function)(region,pos);}__except(EXCEPTION_EXECUTE_HANDLER){return 0;}}
inline uint64_t DebugRegion(uint64_t function,uint64_t owner){__try{return reinterpret_cast<uint64_t(*)(uint64_t)>(function)(owner);}__except(EXCEPTION_EXECUTE_HANDLER){return 0;}}
inline bool ReadDebug(DebugInfo* out,bool wantTarget){
 Context context{};if(!Local(&context))return false;uint64_t rotation{};float value{};
 if(Read(context.actor+0x250,&rotation)&&rotation&&Read(rotation,&value)&&std::isfinite(value)){out->pitch=value;out->pitchKnown=true;}
 {uintptr_t owners{};uint64_t begin{},end{};if(Storage(context.registry,0x85b93800u,&owners)&&Read(owners+0x20,&begin)&&Read(owners+0x28,&end)&&end>=begin&&(end-begin)%4==0&&(end-begin)<=4ull*1048576){int count=0;for(uint64_t at=begin;at<end;at+=4){uint32_t entity{};if(!Read(at,&entity)){count=-1;break;}if(entity<0xfffc0000u)++count;}out->entities=count;}}
 {uintptr_t storage{},element{};int32_t type{};out->gameMode=Storage(context.registry,0x88d3eddfu,&storage)&&InStorage(storage,context.entity,4,&element)&&Read(element,&type)?type:-1;}
 float top{};out->healthKnown=Attribute(context.registry,context.entity,6,&out->health,&top);out->hungerKnown=Attribute(context.registry,context.entity,1,&out->hunger,&top);
 {uintptr_t storage{},element{};uint32_t packed{};out->ground=Storage(context.registry,0xc29078a0u,&storage)&&Packed(storage,context.entity,&packed);
  uint64_t bits{};if(Storage(context.registry,0xc67426f3u,&storage)&&InStorage(storage,context.entity,0x18,&element)&&Read(element,&bits)){out->sneak=(bits>>1)&1;out->sprint=(bits>>3)&1;}
  uint32_t word{};out->fly=Storage(context.registry,0x6de78dc5u,&storage)&&InStorage(storage,context.entity,0x10,&element)&&Read(element+4,&word)&&(word&1);out->flagsKnown=true;}
 {uint64_t gui{};float g[5]{};if(Read(context.client+0x668,&gui)&&gui&&Floats(gui+0x40,g,5)){out->gui=true;out->display[0]=g[0];out->display[1]=g[1];out->guiScale=g[4]>0?g[0]/g[4]:0;}}
 if(!wantTarget)return true;
 uint64_t level{},hit{};int32_t kind{};if(!Read(context.actor+0x200,&level)||!level)return true;const uint64_t getter=Virtual0(context,level,0xa78);if(!getter||!CallGetter(getter,level,&hit)||!hit||!Read(hit+0x18,&kind))return true;
 if(kind==0){if(!Read(hit+0x1c,&out->face)||!Read(hit+0x20,&out->block[0])||!Read(hit+0x24,&out->block[1])||!Read(hit+0x28,&out->block[2]))return true;out->target=1;
  uint64_t owner{},legacy{};if(Read(context.actor+0x1f0,&owner)&&owner){const uint64_t regionOf=Virtual0(context,owner,0x68);const uint64_t region=regionOf?DebugRegion(regionOf,owner):0;const uint64_t getBlock=region?Virtual0(context,region,0x10):0;
   const uint64_t block=getBlock?DebugBlockAt(getBlock,region,out->block):0;if(block&&Read(block+0x68,&legacy)&&legacy&&!Text(legacy+0xe8,out->blockName,sizeof(out->blockName)))out->blockName[0]=0;}}
 else if(kind==1){uint64_t holder{},actor{},mine{},theirs{};uint32_t entity{};uintptr_t owners{},slot{},registry{};float a[3],b[3];
  if(!Read(hit+0x38,&holder)||!holder||!Read(hit+0x48,&entity)||!Storage(holder+0x30,Fnv("ActorOwnerComponent"),&owners)||!InStorage(owners,entity,8,&slot)||!Read(slot,&actor)||!actor)return true;
  if(!Read(context.actor+0x240,&mine)||!Read(actor+0x240,&theirs)||!mine||!theirs||!Floats(mine,a,3)||!Floats(theirs,b,3))return true;
  out->entityDistance=std::sqrt((b[0]-a[0])*(b[0]-a[0])+(b[1]-a[1])*(b[1]-a[1])+(b[2]-a[2])*(b[2]-a[2]));uint32_t id{};
  if(ActorEntity(actor,&registry,&id)&&Has(registry,id,"PlayerComponent")){if(!Text(actor+0xbe8,out->entityName,sizeof(out->entityName)))out->entityName[0]=0;}else EntityName(context,actor,out->entityName,sizeof(out->entityName));out->target=2;}
 return true;
}
// Player head: player-list node whose *(node+0x20) equals the target's ActorUniqueIDComponent; skin = *(node+0xc0):
// int format +0x80 == 4, width +0x84 (multiple of 64), height +0x88, RGBA pixels *(+0xa0), size +0xa8. Face f = width/8,
// scaled nearest-neighbour to 64 x 64 (k = 64/f, at least 1), hat layer (5f, f) blended on top.
// `skin` receives the skin address (the caller's cache key). Read-only.
inline int ReadHead(uintptr_t actor,uint8_t* rgba,int capacity,uint64_t* skin){
 Context context{};uintptr_t registry{},element{};uint32_t entity{};uint64_t unique{},container{},head{},node{},found=0;*skin=0;
 if(!Local(&context)||!ActorEntity(actor,&registry,&entity)||!Element(registry,entity,"ActorUniqueIDComponent",8,&element)||!Read(element,&unique)||!PlayerContainer(context,&container)||!Read(container+8,&head)||!head||!Read(head,&node))return 0;
 for(int guard=0;guard<4096&&node&&node!=head;++guard){uint64_t key{};if(!Read(node+0x20,&key))return 0;if(key==unique){found=node;break;}if(!Read(node,&node))return 0;}
 int32_t kind{};uint32_t w{},h{};uint64_t data{},size{};if(!found||!Read(found+0xc0,skin)||!*skin||!Read(*skin+0x80,&kind)||kind!=4||!Read(*skin+0x84,&w)||!Read(*skin+0x88,&h)||w<64||w%64||w>1024||h<w/4||!Read(*skin+0xa0,&data)||!data||!Read(*skin+0xa8,&size)||size<(uint64_t)w*h*4)return 0;
 const int f=(int)w/8,k=std::max(1,64/f),n=k*f;if(n*n*4>capacity)return 0;
 for(int i=0;i<n;++i)for(int j=0;j<n;++j){const int u=j/k,v=i/k;uint32_t facePixel{},hatPixel{};if(!Read(data+((uint64_t)(f+v)*w+f+u)*4,&facePixel)||!Read(data+((uint64_t)(f+v)*w+5*f+u)*4,&hatPixel))return 0;uint8_t face[4],hat[4];memcpy(face,&facePixel,4);memcpy(hat,&hatPixel,4);
  const float a=hat[3]/255.f;for(int c=0;c<3;++c)rgba[(i*n+j)*4+c]=(uint8_t)std::lround(face[c]*(1-a)+hat[c]*a);rgba[(i*n+j)*4+3]=255;}
 return n;
}
}
