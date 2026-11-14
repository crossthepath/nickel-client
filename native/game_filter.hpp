// Visuals: No Particle, No Render, Optimizations. Game RVAs are for Minecraft 1.26.62.01.
// Nothing is written to the game except the two fog floats.
// #240 0x6b4b0e0  particle render (rcx, rdx, float): emitter name = std::string at rcx+0x18;
// hidden = game not called.
// #218 0x666aa00  screen effect insertion (rdx = {object, type byte}): type 2 = particles (All Particles),
// type 0 = on-screen fire (Fire Overlay), type 7 with byte object+0xc9 = burning entities
// (Entity Fire). Hook in game_view.hpp.
// #235 0x5390350  fog: game first, then two floats (1,000,000 and 1,000,008) at object+0x434 depending on bytes
// object+0x96b (lava), +0x96a (powder snow), +0x968 (water), otherwise regular fog.
// #241 0x5ad3f70  rain and snow: game not called (Weather).
// #3   0x6ebb280  actor added to the render lists: No Render returns FALSE, Optimizations returns TRUE, without
// calling the game. Hook shared with Hurt Color (game_view.hpp).
// #0   0x594c5c0  actor render (7 arguments, r8 = actor, *(rdx+0x28) = context whose bytes +0x75 / +0x76
// must be zero).
// #1   0x59508a0  actor name tag (9 arguments, 5th = actor): cancelling zeroes 0x18 bytes of *rdx.
// #9   0x6a5df90  block entity render (12 arguments, r9 = entity, 7th = int3* position): Optimizations, using
// the entity's virtual calls (+0x48 true, +0x28 false).
// Actor types (component 0x4f6ba419): 93 lightning, 64 dropped item, 317 armor stand, bit 0x100 mob;
// players are never hidden.
#pragma once
#include <string>
#include "game_view.hpp"
#include <atomic>

namespace game {
inline std::atomic<bool(*)(uintptr_t)> customNametagHidden{};
struct FilterShared{
 volatile long foliage=0,foliageFault=0,foliageSeen=0;
 volatile long particles=0,particleMask=0,fireOverlay=0,entityFire=0,fog=0,waterFog=0,lavaFog=0,snowFog=0,weather=0,lightning=0,mobs=0,items=0,stands=0,noRender=0;
 volatile long optimize=0,farItems=0,farMobs=0,farBlocks=0,fault=0,particleSeen=0,fogSeen=0,actorSeen=0;float itemRadius=32,mobRadius=64,blockRadius=64;
};
inline FilterShared& Filter(){static FilterShared* shared=new FilterShared;return *shared;}
struct ParticleName{const char* name;int option;};
inline int ParticleOption(const char* name){
 static const ParticleName table[]={{"critical_hit_emitter",1},{"basic_crit_particle",1},{"magic_critical_hit_emitter",2},{"totem_particle",3},{"witchspell_emitter",4},{"mobspell_emitter",4},{"arrow_spell_emitter",4},{"cauldron_spell_emitter",4},{"splash_spell_emitter",4},
  {"basic_smoke_particle",5},{"white_smoke_particle",5},{"campfire_smoke_particle",5},{"campfire_tall_smoke_particle",5},{"basic_flame_particle",6},{"blue_flame_particle",6},{"green_flame_particle",6},{"small_flame_particle",6},{"candle_flame_particle",6},{"colored_flame_particle",6},
  {"explosion_particle",7},{"cauldron_explosion_emitter",7},{"dragon_death_explosion_emitter",7},{"death_explosion_emitter",7},{"breeze_wind_explosion_emitter",7},{"wind_explosion_emitter",7},{"huge_explosion_lab_misc_emitter",7},
  {"water_splash_particle",8},{"water_splash_particle_manual",8},{"cauldron_splash_particle",8},{"rain_splash_particle",8},{"basic_bubble_particle",9},{"bubble_column_up_particle",9},{"bubble_column_down_particle",9},{"cauldron_bubble_particle",9},
  {"basic_portal_particle",10},{"portal_reverse_particle",10},{"heart_particle",11},{"enchanting_table_particle",12},{"block_destruct",13},{"redstone_repeater_dust_particle",14},{"redstone_torch_dust_particle",14},{"redstone_ore_dust_particle",14},{"redstone_wire_dust_particle",14},{"sculk_sensor_redstone_particle",14}};
 if(strncmp(name,"minecraft:",10))return -1;for(auto& entry:table)if(!strcmp(name+10,entry.name))return entry.option;return -1;
}
using ParticleFunction=void(*)(void*,void*,float);inline ParticleFunction particleOriginal=nullptr;
inline bool ParticleHidden(void* particle){auto& shared=Filter();if(!shared.particles)return false;const long mask=shared.particleMask;if(mask&1)return true;char name[80]{};if(!Text((uintptr_t)particle+0x18,name,sizeof(name)))return false;const int option=ParticleOption(name);return option>0&&(mask>>option)&1;}
inline void ParticleDetour(void* particle,void* second,float third){if(ParticleHidden(particle)){InterlockedExchange(&Filter().particleSeen,1);return;}particleOriginal(particle,second,third);}
// Screen effects (hook #218 in game_view.hpp)
inline bool EffectHidden(void* descriptor){auto& shared=Filter();uint64_t object{};uint8_t kind{},burning{};if(!descriptor||!Read((uintptr_t)descriptor,&object)||!object||!Read((uintptr_t)descriptor+8,&kind))return false;
 if(kind==2)return shared.particles&&(shared.particleMask&1);if(!shared.noRender)return false;if(kind==0)return shared.fireOverlay!=0;if(kind==7)return shared.entityFire&&Read(object+0xc9,&burning)&&burning;return false;}
using FogFunction=uint64_t(*)(void*,void*,void*,void*);inline FogFunction fogOriginal=nullptr;
inline void FogWork(void* fog){auto& shared=Filter();if(!shared.noRender)return;uint8_t lava{},snow{},water{};if(!Read((uintptr_t)fog+0x96b,&lava)||!Read((uintptr_t)fog+0x96a,&snow)||!Read((uintptr_t)fog+0x968,&water))return;
 const bool clear=lava?shared.lavaFog!=0:snow?shared.snowFog!=0:water?shared.waterFog!=0:shared.fog!=0;if(!clear)return;const float away[2]={1000000.f,1000008.f};memcpy((void*)((uintptr_t)fog+0x434),away,8);InterlockedExchange(&shared.fogSeen,1);}
inline void FogGuarded(void* fog){__try{FogWork(fog);}__except(EXCEPTION_EXECUTE_HANDLER){InterlockedExchange(&Filter().fault,1);}}
// Extra listener on the same hook (Custom Sky fog), called after No Render
inline void(*fogListener)(void*)=nullptr;
inline uint64_t FogDetour(void* fog,void* b,void* c,void* d){const uint64_t result=fogOriginal(fog,b,c,d);if(fog&&!Filter().fault)FogGuarded(fog);if(fog&&fogListener)fogListener(fog);return result;}
using WeatherFunction=void(*)(void*,void*,void*,void*);inline WeatherFunction weatherOriginal=nullptr;
inline void WeatherDetour(void* a,void* b,void* c,void* d){auto& shared=Filter();if(shared.noRender&&shared.weather)return;weatherOriginal(a,b,c,d);}
// 0: nothing; 1: hidden by No Render; 2: too far for Optimizations
inline int ActorVerdict(void* pointer){
 auto& shared=Filter();if((!shared.noRender&&!shared.optimize)||!pointer)return 0;const uintptr_t actor=(uintptr_t)pointer;uintptr_t registry{},storage{},element{};uint32_t entity{};int32_t type{};if(!ActorEntity(actor,&registry,&entity)||Has(registry,entity,"PlayerComponent"))return 0;
 if(!Storage(registry,0x4f6ba419u,&storage)||!InStorage(storage,entity,4,&element)||!Read(element,&type))return 0;const bool mob=(type>>8)&1;
 if(shared.noRender){if(type==93){if(shared.lightning)return 1;}else if(type==64){if(shared.items)return 1;}else if(type==317){if(shared.stands)return 1;}else if(mob&&shared.mobs)return 1;}
 if(!shared.optimize||type==317||Has(registry,entity,"LocalPlayerComponent"))return 0;uint8_t keep{};if(Read(actor+0x375,&keep)&&keep)return 0;float radius=0;if(type==64){if(!shared.farItems)return 0;radius=shared.itemRadius;}else if(mob){if(!shared.farMobs)return 0;radius=shared.mobRadius;}else return 0;
 float position[3],origin[3];auto& view=View();if(!view.tick||GetTickCount64()-view.tick>500)return 0;AcquireSRWLockShared(&view.lock);memcpy(origin,view.origin,12);ReleaseSRWLockShared(&view.lock);
 if(!Storage(registry,0xe53c7221u,&storage)||!InStorage(storage,entity,12,&element)||!Floats(element,position,3))return 0;const float dx=position[0]-origin[0],dy=position[1]-origin[1],dz=position[2]-origin[2],squared=dx*dx+dy*dy+dz*dz;return std::isfinite(squared)&&squared>radius*radius?2:0;
}
inline bool QuietContext(void* holder){uint64_t ctx{};uint8_t a{},b{};return holder&&Read((uintptr_t)holder+0x28,&ctx)&&ctx&&Read(ctx+0x75,&a)&&Read(ctx+0x76,&b)&&!a&&!b;}
using ActorFunction=void(*)(void*,void*,void*,void*,void*,void*,uint8_t);inline ActorFunction actorOriginal=nullptr;
// Actor render subscribers (hook #0, ActorRenderEvent): highlight before and after the game call.
struct ActorRenderToken{uint64_t data[8];bool on=false;};
inline void(*volatile actorRenderPre)(void* holder,void* actor,ActorRenderToken* token)=nullptr;
inline void(*volatile actorRenderPost)(ActorRenderToken* token)=nullptr;
// Item Physics (game_item_physics.hpp): top of the matrix stack replaced before rendering, restored after (LIFO)
struct PhysicsToken{uint64_t w=0;float saved[16]{};bool on=false;};
inline void(*volatile actorPhysicsPre)(void* holder,void* actor,void* pos,PhysicsToken* token)=nullptr;
inline void(*volatile actorPhysicsPost)(PhysicsToken* token)=nullptr;
inline void ActorDetour(void* a,void* holder,void* actor,void* d,void* e,void* f,uint8_t g){if(QuietContext(holder)&&ActorVerdict(actor)){InterlockedExchange(&Filter().actorSeen,1);return;}
 ActorRenderToken token{};PhysicsToken physics{};if(auto pre=actorRenderPre)pre(holder,actor,&token);if(auto pp=actorPhysicsPre)pp(holder,actor,e,&physics);actorOriginal(a,holder,actor,d,e,f,g);
 if(physics.on)if(auto pq=actorPhysicsPost)pq(&physics);if(token.on)if(auto post=actorRenderPost)post(&token);}
using TagFunction=void*(*)(void*,void*,void*,void*,void*,void*,void*,float,void*);inline TagFunction tagOriginal=nullptr;
// Nick Hider: replacement string passed instead of the 6th argument (name tag text)
inline bool(*volatile tagRename)(void* actor,std::string* out)=nullptr;
inline bool TagRenameGuarded(void* actor,std::string* out){__try{auto f=tagRename;return f&&f(actor,out);}__except(EXCEPTION_EXECUTE_HANDLER){return false;}}
inline void* TagDetour(void* a,void* out,void* c,void* d,void* actor,void* f,void* g,float h,void* i){bool hide=ActorVerdict(actor);if(!hide)if(auto callback=customNametagHidden.load(std::memory_order_acquire))hide=callback((uintptr_t)actor);if(hide){if(out)memset(out,0,0x18);return out;}
 if(tagRename){std::string renamed;if(TagRenameGuarded(actor,&renamed))return tagOriginal(a,out,c,d,actor,&renamed,g,h,i);}return tagOriginal(a,out,c,d,actor,f,g,h,i);}
using BlockActorFunction=void(*)(uint64_t,uint64_t,uint64_t,uint64_t,uint64_t,uint64_t,uint64_t,uint64_t,uint64_t,uint64_t,uint64_t,uint64_t);inline BlockActorFunction blockActorOriginal=nullptr;
inline bool BlockActorFar(uint64_t holder,uint64_t entity,uint64_t positionPointer){
 auto& shared=Filter();if(!shared.optimize||!shared.farBlocks||!entity||!QuietContext((void*)holder))return false;Context context{};if(!Client(&context))return false;const uint64_t first=Virtual(context,entity,0x48),second=Virtual(context,entity,0x28);if(!first||!second)return false;
 if(!reinterpret_cast<bool(*)(uint64_t)>(first)(entity)||reinterpret_cast<bool(*)(uint64_t)>(second)(entity))return false;int32_t at[3];if(!Read(positionPointer,&at[0])||!Read(positionPointer+4,&at[1])||!Read(positionPointer+8,&at[2]))return false;
 float origin[3];auto& view=View();if(!view.tick||GetTickCount64()-view.tick>500)return false;AcquireSRWLockShared(&view.lock);memcpy(origin,view.origin,12);ReleaseSRWLockShared(&view.lock);const float dx=at[0]+.5f-origin[0],dy=at[1]+.5f-origin[1],dz=at[2]+.5f-origin[2];return dx*dx+dy*dy+dz*dz>shared.blockRadius*shared.blockRadius;
}
inline bool BlockActorGuarded(uint64_t holder,uint64_t entity,uint64_t position){__try{return BlockActorFar(holder,entity,position);}__except(EXCEPTION_EXECUTE_HANDLER){InterlockedExchange(&Filter().fault,1);return false;}}
// Walls subscriber (BlockActorRenderEvent): returns the modified world matrix to restore after drawing.
inline float* (*volatile blockActorWalls)(uint64_t params,uint64_t context,uint64_t position,float saved[16])=nullptr;
// Xray subscriber: true = block entity of an unlisted block, drawing cancelled.
inline bool(*volatile blockActorXray)(uint64_t context,uint64_t source,uint64_t position)=nullptr;
inline void BlockActorDetour(uint64_t a,uint64_t b,uint64_t c,uint64_t d,uint64_t e,uint64_t f,uint64_t g,uint64_t h,uint64_t i,uint64_t j,uint64_t k,uint64_t l){if(!Filter().fault&&BlockActorGuarded(b,d,g))return;
 if(auto xr=blockActorXray)if(xr(d,c,g))return;
 float saved[16];float* restore=nullptr;if(auto walls=blockActorWalls)restore=walls(b,d,f,saved);
 blockActorOriginal(a,b,c,d,e,f,g,h,i,j,k,l);if(restore)memcpy(restore,saved,64);}

inline int InstallParticleHook(void(*log)(const char*)){static int state=0;if(state)return state;const uint8_t bytes[16]={0x55,0x41,0x57,0x41,0x56,0x41,0x55,0x41,0x54,0x56,0x57,0x53,0x48,0x81,0xec,0x98};state=InstallOne(0x6b4b0e0,bytes,16,(void*)ParticleDetour,(void**)&particleOriginal,"No Particle",log);return state;}
inline int InstallFogHook(void(*log)(const char*)){static int state=0;if(state)return state;const uint8_t bytes[16]={0x55,0x41,0x56,0x56,0x57,0x53,0x48,0x81,0xec,0xa0,0x00,0x00,0x00,0x48,0x8d,0xac};state=InstallOne(0x5390350,bytes,16,(void*)FogDetour,(void**)&fogOriginal,"No Render (fog)",log);return state;}
inline int InstallWeatherHook(void(*log)(const char*)){static int state=0;if(state)return state;const uint8_t bytes[16]={0x55,0x41,0x57,0x41,0x56,0x41,0x55,0x41,0x54,0x56,0x57,0x53,0x48,0x81,0xec,0x38};state=InstallOne(0x5ad3f70,bytes,16,(void*)WeatherDetour,(void**)&weatherOriginal,"No Render (weather)",log);return state;}
inline int InstallActorHooks(void(*log)(const char*)){static int state=0;if(state)return state;state=-1;
 const uint8_t a[16]={0x55,0x41,0x57,0x41,0x56,0x41,0x55,0x41,0x54,0x56,0x57,0x53,0x48,0x81,0xec,0xe8},b[16]={0x55,0x41,0x57,0x41,0x56,0x41,0x55,0x41,0x54,0x56,0x57,0x53,0x48,0x81,0xec,0x38},c[10]={0x41,0x56,0x56,0x57,0x55,0x53,0x48,0x83,0xec,0x40};
 if(!addOriginal&&InstallOne(0x6ebb280,c,10,(void*)AddDetour,(void**)&addOriginal,"Actor lists",log)!=1)return state;if(InstallOne(0x594c5c0,a,16,(void*)ActorDetour,(void**)&actorOriginal,"Actor render",log)!=1)return state;
 if(InstallOne(0x59508a0,b,16,(void*)TagDetour,(void**)&tagOriginal,"Actor name tag",log)!=1)return state;state=1;return state;}
inline int InstallBlockActorHook(void(*log)(const char*)){static int state=0;if(state)return state;const uint8_t bytes[16]={0x55,0x41,0x57,0x41,0x56,0x41,0x55,0x41,0x54,0x56,0x57,0x53,0x48,0x81,0xec,0x38};state=InstallOne(0x6a5df90,bytes,16,(void*)BlockActorDetour,(void**)&blockActorOriginal,"Optimizations (block entities)",log);return state;}
// No Render Foliage: block+0x68 is the BlockLegacy; its material byte at +0x28 is 8 for foliage.
inline bool FoliageHidden(void* block){
 if(!Filter().noRender||!Filter().foliage||Filter().foliageFault||!block)return false;
 uint64_t legacy{};uint8_t material{};return Read((uintptr_t)block+0x68,&legacy)&&legacy&&Read(legacy+0x28,&material)&&material==8;
}
using TessellateFunction=bool(*)(void*,void*,void*,void*,uint8_t);
using TessellateExtraFunction=void(*)(void*,void*,void*,void*,uint64_t);
inline TessellateFunction tessellateOriginal=nullptr;inline TessellateExtraFunction tessellateExtraOriginal=nullptr;
// Xray subscriber (BlockTessellateEvent): true = block hidden; also resets the tessellator byte +0x7a to 0
// (+0x7a and +0xec are saved and restored around the call).
inline bool(*volatile tessellateXray)(void* tessellator,void* block)=nullptr;inline bool(*volatile tessellateXrayOn)()=nullptr;   // Xray module state: the byte save/restore only runs while Xray is on
inline bool TessellateXray(void* a,void* block,uint8_t saved[5]){auto x=tessellateXray;if(!x)return false;memcpy(saved,(uint8_t*)a+0x7a,1);memcpy(saved+1,(uint8_t*)a+0xec,4);return x(a,block);}
inline void TessellateRestore(void* a,const uint8_t saved[5]){memcpy((uint8_t*)a+0x7a,saved,1);memcpy((uint8_t*)a+0xec,saved+1,4);}
inline bool TessellateDetour(void* a,void* b,void* block,void* d,uint8_t e){if(FoliageHidden(block)){InterlockedExchange(&Filter().foliageSeen,1);return false;}
 uint8_t saved[5];const auto xrayOn=tessellateXrayOn;const bool xr=tessellateXray!=nullptr&&xrayOn&&xrayOn();if(xr&&TessellateXray(a,block,saved)){TessellateRestore(a,saved);return false;}const bool r=tessellateOriginal(a,b,block,d,e);if(xr)TessellateRestore(a,saved);return r;}
inline void TessellateExtraDetour(void* a,void* b,void* block,void* d,uint64_t e){if(FoliageHidden(block)){InterlockedExchange(&Filter().foliageSeen,1);return;}
 uint8_t saved[5];const auto xrayOn=tessellateXrayOn;const bool xr=tessellateXray!=nullptr&&xrayOn&&xrayOn();if(xr&&TessellateXray(a,block,saved)){TessellateRestore(a,saved);return;}tessellateExtraOriginal(a,b,block,d,e);if(xr)TessellateRestore(a,saved);}
inline int InstallFoliageHooks(void(*log)(const char*)){
 static int state=0;if(state)return state;state=-1;
 const uint8_t first[16]={0x41,0x57,0x41,0x56,0x41,0x55,0x41,0x54,0x56,0x57,0x55,0x53,0x48,0x81,0xec,0xb8};
 const uint8_t second[16]={0x41,0x57,0x41,0x56,0x41,0x55,0x41,0x54,0x56,0x57,0x53,0x48,0x83,0xec,0x60,0x0f};
 if(InstallOne(0x681b6d0,first,16,(void*)TessellateDetour,(void**)&tessellateOriginal,"Foliage (tessellate)",log)!=1)return state;
 if(InstallOne(0x68902c0,second,16,(void*)TessellateExtraDetour,(void**)&tessellateExtraOriginal,"Foliage (extra tessellate)",log)!=1)return state;
 state=1;return state;
}

}
