// Sound ESP. SoundPlayEvent hooks Minecraft RVA 0xae8ae0: the event (+0x40 = rdx) is raised BEFORE the game call
// and never cancelled. rdx = sound description: name std::string +0, position float3 +0x20, volume float +0x30.
// Sounds are classified by prefix (34 entries {prefix, category, label, suffix}): 0 Footsteps, 1 Blocks, 2 Containers,
// 3 Eating, 4 Projectiles, 5 Combat; then "block.*.step/.fall/.hit/.break/.place", "mob.*" (Mobs) and the rest
// (Other); "random.bow" below 0.6 volume = "Throw". Up to 256 sounds are kept for the render thread to draw.
#pragma once
#include "game_events.hpp"
#include <MinHook.h>
#include <mutex>
#include <string>
#include <vector>

namespace game {
struct HeardSound{std::string name;float pos[3];float volume;uint64_t tick;};
struct SoundShared{std::mutex guard;std::vector<HeardSound> heard;volatile long enabled=0,fault=0,seen=0;};
inline SoundShared& Sounds(){static auto* s=new SoundShared;return *s;}
using SoundFunction=void(*)(void*,void*);inline SoundFunction soundOriginal=nullptr;
inline void SoundWork(void* description){
 char name[128]{};float p[3],v=1;if(!description||!Text((uintptr_t)description,name,sizeof(name))||!name[0]||!Floats((uintptr_t)description+0x20,p,3))return;Floats((uintptr_t)description+0x30,&v,1);
 auto& s=Sounds();std::lock_guard<std::mutex> lock(s.guard);if(s.heard.size()>=256)s.heard.erase(s.heard.begin());s.heard.push_back({name,{p[0],p[1],p[2]},v,GetTickCount64()});InterlockedExchange(&s.seen,1);}
inline void SoundGuarded(void* description){__try{SoundWork(description);}__except(EXCEPTION_EXECUTE_HANDLER){InterlockedExchange(&Sounds().fault,1);}}
inline void SoundDetour(void* self,void* description){auto& s=Sounds();if(s.enabled&&!s.fault){Context c{};if(Local(&c))SoundGuarded(description);}soundOriginal(self,description);}
inline int InstallSoundHook(void(*log)(const char*)){
 static int state=0;if(state)return state;state=-1;uintptr_t base{};uint32_t size{};if(!Supported(&base,&size)){if(log)log("Sound ESP: not the supported Minecraft image, no hook installed");return state;}
 const uint8_t expected[16]={0x55,0x41,0x57,0x41,0x56,0x56,0x57,0x53,0x48,0x83,0xec,0x68,0x48,0x8d,0x6c,0x24};
 if(!CodeIs(base+Rva(0xae8ae0),expected,16)){if(log)log("Sound ESP: unexpected bytes at the sound function, no hook installed");return state;}
 if(MH_CreateHook((void*)(base+Rva(0xae8ae0)),(void*)SoundDetour,(void**)&soundOriginal)!=MH_OK||MH_EnableHook((void*)(base+Rva(0xae8ae0)))!=MH_OK){if(log)log("Sound ESP: MinHook could not install the sound hook");return state;}
 if(log)log("Sound ESP: sound play hook installed");state=1;return state;}
// Classification: category (0..7: Footsteps, Blocks, Containers, Eating, Projectiles, Combat, Mobs, Other) and label.
struct SoundRule{const char* prefix;int category;const char* label;bool suffix;};
inline const SoundRule soundRules[34]={{"step.",0,"Footsteps",0},{"land.",0,"Landing",0},{"jump.",0,"Jump",0},{"fall.",0,"Fall",0},{"damage.fall",0,"Fall",0},{"dig.",1,"Block",1},{"hit.",1,"Mining",1},{"use.",1,"Block Use",1},
 {"random.door",1,"Door",0},{"random.chestopen",2,"Chest Open",0},{"random.chestclosed",2,"Chest Close",0},{"random.enderchestopen",2,"Ender Chest Open",0},{"random.enderchestclosed",2,"Ender Chest Close",0},
 {"random.shulkerboxopen",2,"Shulker Open",0},{"random.shulkerboxclosed",2,"Shulker Close",0},{"block.barrel.open",2,"Barrel Open",0},{"block.barrel.close",2,"Barrel Close",0},{"random.eat",3,"Eating",0},{"random.drink",3,"Drinking",0},
 {"random.burp",3,"Burp",0},{"random.bowhit",4,"Arrow Hit",0},{"random.bow",4,"Bow",0},{"crossbow.shoot",4,"Crossbow",0},{"crossbow.",4,"Crossbow Loading",0},{"item.trident.throw",4,"Trident",0},{"item.trident.riptide",4,"Riptide",0},
 {"mob.endermen.portal",4,"Teleport",0},{"mob.shulker.teleport",4,"Teleport",0},{"game.player.attack",5,"Attack",0},{"game.player.hurt",5,"Hurt",0},{"game.player.die",5,"Death",0},{"item.shield.block",5,"Shield Block",0},{"random.totem",5,"Totem",0},{"random.explode",5,"Explosion",0}};
inline std::string SoundWord(std::string s){for(auto& ch:s)if(ch=='_'||ch=='.')ch=' ';bool up=true;for(auto& ch:s){if(ch==' ')up=true;else if(up){ch=(char)toupper((unsigned char)ch);up=false;}}return s;}
inline int ClassifySound(const std::string& n,float volume,std::string& label){
 for(const auto& r:soundRules){const size_t l=strlen(r.prefix);if(n.compare(0,l,r.prefix)==0){label=r.label;if(!strcmp(r.prefix,"random.bow")&&n.size()==l&&volume<.6f)label="Throw";if(r.suffix&&n.size()>l)label+=" ("+SoundWord(n.substr(l))+")";return r.category;}}
 auto ends=[&](const char* e){const size_t l=strlen(e);return n.size()>l&&n.compare(n.size()-l,l,e)==0;};
 if(n.compare(0,6,"block.")==0){const std::string mid=n.substr(6,n.rfind('.')-6);if(ends(".step")){label="Footsteps";return 0;}if(ends(".fall")){label="Fall";return 0;}if(ends(".hit")){label="Mining ("+SoundWord(mid)+")";return 1;}
  if(ends(".break")||ends(".place")){label="Block ("+SoundWord(mid)+")";return 1;}}
 if(n.compare(0,4,"mob.")==0){const size_t dot=n.find('.',4);const std::string who=SoundWord(n.substr(4,dot==std::string::npos?std::string::npos:dot-4));std::string act=dot==std::string::npos?"":n.substr(dot+1);
  static const char* acts[]={"idle","step","walk","hurt","death","attack","ambient"};for(auto a:acts)if(act==a){label=who+" "+SoundWord(act);return 6;}label=who;return 6;}
 label=SoundWord(n);return 7;}
}
