// Movement modules built on hooks that are already installed:
// - No Jump Delay: LocalPlayerJump (#16, game RVA 0xad34000) before the game: *(int*)(arg7+0x10) = 0;
// - High Jump: deferred after the game's jump: Y velocity (*(float*)(arg8+0x1c)) += Strength, for the local player only;
// - Anti Immobile: LevelTick: bit 16 of the player's ActorDataFlagComponent (0x18 bytes) is cleared;
// - No Fall, mode Glide (Nickel's own, the reference packet mode does not stop the damage): while the fall distance
//   (FallDistanceComponent) is above 2 and the ground under the feet is closer than the next tick's drop plus 1.5
//   (or cannot be read: chunk not loaded yet, e.g. after a fast flight)
//   blocks, LevelTick caps the downward velocity at 0.4 block per tick and every PlayerAuthInputPacket (0x90) carries
//   StartGliding (input bit 32) with a pitch (+0x30) of at most 30 degrees; the first packet after landing carries
//   StopGliding (bit 33). Servers that trust the client's glide state reset the fall on a slow gliding tick, so the
//   landing deals no damage. Servers that simulate the player themselves (vanilla) ignore it.
//   Runs last in the level tick, after the flight modules (Jetpack sets the velocity every tick and would undo the
//   cap). While a module lifts the player (Jetpack held, Flight, Air Jump, High Jump, Long Jump, Spider, Glide, Elytra
//   Fly, Boat Fly, Look TP), the fall distance condition is dropped: the client's counter restarts after every climb
//   while the server counts from the highest point, so any descent close to the ground glides.
// - No Fall, mode Packet: every other PlayerAuthInputPacket stores Y (+0x3c); on the other one, if block (x, y+1, z)
//   has a zero BlockLegacy byte +0x28: Y = stored Y + 0.1, movement (+0x48, +0x50) zeroed, input bits 10..13 (+0x88)
//   cleared; Falling Only: Y velocity < 0, not flying (abilities 0x6de78dc5 +4 bit 0), not gliding (actor flag 32),
//   not in water or lava.
#pragma once
#include "game_actions.hpp"
#include <atomic>
#include <cmath>
#include <cstring>
#include <mutex>

namespace game {
struct Movement2{std::mutex guard;bool noJumpDelay=false,highJump=false,antiImmobile=false,noFall=false,fallingOnly=false;float strength=.1f;bool flip=false;float savedY=0;
 int noFallMode=0;std::atomic<bool> glideWindow{false},glideSent{false};
 std::atomic<bool> flyAssist{false};};   // noFallMode: 0 Glide, 1 Packet; flyAssist: a module lifts the player
inline Movement2& Move2(){static auto* m=new Movement2;return *m;}
inline void Move2JumpPre(uint64_t arg7,uint64_t){static volatile long stopped_=0;auto& m=Move2();if(m.noJumpDelay&&arg7)if(!stopped_)__try{*(int32_t*)(arg7+0x10)=0;}__except(game::FeatureFault("Move2JumpPre",&stopped_,GetExceptionInformation())){game::FeatureFlush();}}
inline void Move2JumpPost(uint64_t arg8){static volatile long stopped_=0;auto& m=Move2();if(!m.highJump||!arg8)return;Context c{};if(!Local(&c))return;if(!stopped_)__try{*(float*)(arg8+0x1c)+=m.strength;}__except(game::FeatureFault("Move2JumpPost",&stopped_,GetExceptionInformation())){game::FeatureFlush();}}
inline bool Move2Has(const Context& c,uint32_t component){uintptr_t storage{};uint32_t packed{};return Storage(c.registry,component,&storage)&&Packed(storage,c.entity,&packed);}
inline float Move2FallDistance(const Context& c){uintptr_t storage{},element{};float fall=0;if(Storage(c.registry,0xce6b34f6u,&storage)&&InStorage(storage,c.entity,8,&element))Read(element,&fall);return fall;}   // FallDistanceComponent
// Distance from the feet down to the first non-air block under the player's box (every column the box covers: a box on
// a block edge lands on the neighbouring column), searched over "depth" blocks: 1 found, 0 only air, -1 unknown (a block
// that cannot be read: chunk not loaded yet after a fast flight, no region).
inline int Move2GroundBelow(const Context& c,float depth,float* distance){uint64_t bb{};float b[6]{};if(!Read(c.actor+0x248,&bb)||!bb||!Floats(bb,b,6))return -1;
 const float feet=b[1];const uint64_t region=ActorRegion(c);if(!region)return -1;
 const int32_t x0=(int32_t)std::floor(b[0]+1e-3f),x1=(int32_t)std::floor(b[3]-1e-3f),z0=(int32_t)std::floor(b[2]+1e-3f),z1=(int32_t)std::floor(b[5]-1e-3f);if(x1-x0>2||z1-z0>2)return -1;
 for(int32_t y=(int32_t)std::floor(feet-0.001f);y>=(int32_t)std::floor(feet-depth);--y)for(int32_t x=x0;x<=x1;++x)for(int32_t z=z0;z<=z1;++z){const int32_t pos[3]={x,y,z};const uint64_t block=RegionBlock(c,region,pos);uint64_t legacy{};char name[96]{};
  if(!block||!Read(block+0x68,&legacy)||!legacy||!Text(legacy+0xe8,name,sizeof(name)))return -1;if(!strstr(name,"air")){*distance=feet-(float)(y+1);return 1;}}
 return 0;}
inline void Move2GlideTick(const Context& c){auto& m=Move2();uint64_t st{};float vy{};if(!Read(c.actor+0x240,&st)||!st||!Read(st+0x1c,&vy)){m.glideWindow=false;return;}
 float ground{};int below=0;const bool falling=vy<0&&(m.flyAssist||Move2FallDistance(c)>2.f);if(falling)below=Move2GroundBelow(c,-vy+1.5f,&ground);
 const bool closeToGround=falling&&(below<0||(below>0&&ground<=-vy+1.5f));m.glideWindow=closeToGround;   // unknown ground: glide anyway
 if(closeToGround&&vy<-.4f)*(float*)(st+0x1c)=-.4f;}
inline void Move2GlideSend(uintptr_t packet){auto& m=Move2();uint64_t& bits=*(uint64_t*)(packet+0x88);
 if(m.glideWindow){bits|=1ull<<32;float& pitch=*(float*)(packet+0x30);if(pitch>30.f)pitch=30.f;m.glideSent=true;}
 else if(m.glideSent){bits|=1ull<<33;m.glideSent=false;}}
inline void Move2TickWork(){auto& m=Move2();const bool glide=m.noFall&&m.noFallMode==0;if(!glide)m.glideWindow=false;if(!m.antiImmobile&&!glide)return;Context c{};if(!Local(&c))return;
 if(glide)Move2GlideTick(c);
 if(!m.antiImmobile)return;uintptr_t f{};if(Element(c.registry,c.entity,"ActorDataFlagComponent",0x18,&f)){uint64_t v{};if(Read(f,&v)&&(v&(1ull<<16)))*(uint64_t*)f=v&~(1ull<<16);}}
inline void Move2Tick(){static volatile long stopped_=0;if(!stopped_)__try{Move2TickWork();}__except(game::FeatureFault("Move2Tick",&stopped_,GetExceptionInformation())){game::FeatureFlush();}}
inline void Move2SendWork(uintptr_t packet){auto& m=Move2();if(!m.noFall||PacketIdOf(packet)!=0x90)return;if(m.noFallMode==0){Move2GlideSend(packet);return;}const bool b=m.flip;m.flip=!b;if(!b){Read(packet+0x3c,&m.savedY);return;}
 Context c{};if(!Local(&c))return;const uint64_t region=ActorRegion(c);float x{},y{},z{};Read(packet+0x38,&x);Read(packet+0x3c,&y);Read(packet+0x40,&z);
 if(region){const int32_t pos[3]={(int32_t)std::floor(x),(int32_t)std::floor(y+1.f),(int32_t)std::floor(z)};const uint64_t block=RegionBlock(c,region,pos);uint64_t legacy{};uint8_t flag{};if(block&&Read(block+0x68,&legacy)&&legacy&&Read(legacy+0x28,&flag)&&flag)return;}
 if(m.fallingOnly){float dy{};Read(packet+0x4c,&dy);if(dy>=0)return;uint32_t fly{};  {uintptr_t storage{},el{};if(Storage(c.registry,0x6de78dc5u,&storage)&&InStorage(storage,c.entity,0x10,&el)&&Read(el+4,&fly)&&(fly&1))return;}
  uintptr_t f{};uint64_t flags{};if(Element(c.registry,c.entity,"ActorDataFlagComponent",0x18,&f)&&Read(f,&flags)&&(flags&(1ull<<32)))return;
  if(Move2Has(c,0x78e89f39u)||Move2Has(c,0x832a2768u))return;}
 *(float*)(packet+0x3c)=m.savedY+.1f;*(uint64_t*)(packet+0x48)=0;*(uint32_t*)(packet+0x50)=0;*(uint64_t*)(packet+0x88)&=~(0xfull<<10);}
inline bool Move2Send(uintptr_t packet){static volatile long stopped_=0;if(!stopped_)__try{Move2SendWork(packet);}__except(game::FeatureFault("Move2Send",&stopped_,GetExceptionInformation())){game::FeatureFlush();}return false;}
}
