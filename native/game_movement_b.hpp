// Movement modules, group B: Air Jump, Long Jump, Reverse Step, Safe Walk, Jesus, Parkour.
//
// - Air Jump (LevelTick, client level): when the jump bit (bit 25 of MoveInput+0x10, true for one tick on a key press) is
//   set while the player is NOT on the ground (OnGroundFlagComponent 0xc29078a0), an empty LocalPlayerJumpRequestComponent
//   (0xda6293f1) is added to the local entity in the game's EnTT registry, exactly like a ground jump request. The sparse
//   set insertion (pages of 0x800 entries at +8/+0x10/+0x18, packed array +0x20/+0x28/+0x30, deletion policy byte +0x40,
//   head +0x48) uses the game's allocator. The storage must already exist (the game creates it on the first ground jump).
// - Long Jump: LocalPlayerJump (#16, RVA 0xad34000) before the game: with input, state = {Speed, Air Ticks, active, !left};
//   after the game's jump the velocity (arg8 +0x18 / +0x1c / +0x20) becomes vx = dirX * Speed, vz = dirZ * Speed, vy += Height.
//   LevelTick: in the air and active: left = 1; while ticks > 0: ticks--, speed *= Retention / 100 and the horizontal
//   velocity is set to dir * speed. On the ground: if left the state is cleared; then Auto Jump with input and a direction
//   requests a normal jump. Direction = (W - S, D - A) of MoveInput bits 13..16 rotated by yaw + 90 degrees
//   (x = cos * f - sin * s, z = cos * s + sin * f), none when MoveInput+0x60 bit 0x40 is set or no key is held.
// - Reverse Step: SprintIntent (#19). On the ground it remembers the feet Y. On the first airborne tick (input present,
//   region present) with vy <= 0, no jump / sneak input, not sneaking / gliding / flying / in water or lava, and the block at
//   the feet is not a ladder, vine or scaffolding: BlockSource vtable +0xb0 (fetch collision shapes) is asked for the shapes
//   under the player down to (rememberedY - Height - 0.001); the highest top face found within reach gives the drop. If the drop
//   is at least 0.1 and the block there is not a liquid, Motion sets vy = -(drop + 0.01) and Position teleports the player
//   down by the drop (position, previous position, AABB, zero velocity).
// - Parkour: SprintIntent (#19), local player's own event only. Guards: external input data, region, on the ground, no
//   jump / sneak input (MoveInput+0x10 bits 7 / 0), actor data flags present with sneak (bit 1) clear, not flying
//   (abilities +4 bit 0), not in water or lava, Sprint Only -> sprinting (flag 3), horizontal velocity^2 > 1e-6, block at
//   the feet centre not a ladder / vine / scaffolding. Threshold = 1.001 (Minimum Drop) else MaxAutoStep (component
//   0x9aae5d7f) + 0.001, or 0.5635 without it. The movement simulator (game_movement_sim.hpp) runs up to 20 ticks with
//   the current move input: a failed tick -> nothing; still on the ground after the first tick -> nothing; first tick
//   back on the ground after leaving it -> jump when startY - landingY > threshold; never landing within 20 ticks ->
//   jump. Jump = a jump request consumed on the next SprintIntent (JumpRequest / JumpInput, game_actions.hpp).
// - Safe Walk: SneakMovement (game RVA 0xb3298f0, 8 arguments). Before the game: MoveInput bit 0 (sneak) is forced on, actor
//   flag 1 set and flag 6 cleared; after the game the sneak bit, the 24 bytes of actor flags and byte +0x55 of arg7 are
//   restored. The game therefore applies its edge protection without the player really sneaking. Only entities that carry
//   the component of arg6 are affected.
// - Jesus: FetchCollisionShapes (game RVA 0x360a1f0, 15 arguments), client thread only, after the game: for the local
//   player, every liquid block overlapped by the query box (and down by the requested Y move) adds a solid box
//   (x, y, z, x + 1, top, z + 1) to the result's shape vector (+0x58) plus a zero element in the parallel vector
//   (+0x70); the vectors grow like MSVC vectors with the game's allocator. Liquid = BlockLegacy flag byte +0x29 bit 1 of the
//   block returned by BlockSource vtable +0x28; surface = game function RVA 0x3534c60(block, pos).
#pragma once
#include "game_movement.hpp"
#include "game_movement3.hpp"
#include "game_movement_sim.hpp"
#include "game_sticky_aim.hpp"
#include "game_map.hpp"
#include <MinHook.h>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>

namespace game {
struct MovementB{
 std::atomic<bool> airJump{false},longJump{false},reverseStep{false},safeWalk{false},jesus{false};
 std::atomic<bool> wasLongJump{false};
 std::atomic<long> clientThread{0};
 std::mutex guard;
 // Long Jump settings (Speed, Height, Retention in percent, Air Ticks, Auto Jump) and state (speed, ticks, active, left).
 float ljSpeed=.6f,ljHeight=0,ljRetention=98;int ljAirTicks=10;bool ljAutoJump=false;
 float ljVelocity=0;int ljTicks=0;bool ljActive=false,ljLeft=false;
 bool ljPending=false;float ljPendingX=0,ljPendingZ=0;
 // Reverse Step settings and state (remembered feet Y, was on the ground, game-style vector of 0x30-byte shapes).
 std::atomic<bool> rsRelease{false};float rsHeight=1;bool rsPosition=false;float rsY=0;bool rsGround=false;uint64_t rsShapes[3]{};
 // Parkour settings and simulator collision buffer (freed on the game thread after the module is turned off).
 std::atomic<bool> parkour{false},pkMinimumDrop{true},pkSprintOnly{false},pkRelease{false};MsShapes pkShapes;
 void(*log)(const char*)=nullptr;
};
inline MovementB& MoveB(){static auto* m=new MovementB;return *m;}

// ---- the game's allocator and MSVC vector / EnTT helpers -------------------------------------------------------
struct MoveBAllocator{void*(*allocate)(size_t)=nullptr;void(*release)(void*)=nullptr;};
inline MoveBAllocator& MoveBHeap(){
 static MoveBAllocator heap=[]{MoveBAllocator h;HMODULE redirect=GetModuleHandleW(L"MallocRedirect.dll");
  if(redirect){h.allocate=(void*(*)(size_t))GetProcAddress(redirect,"malloc");h.release=(void(*)(void*))GetProcAddress(redirect,"free");}
  if(!h.allocate||!h.release){HMODULE crt=GetModuleHandleW(L"ucrtbase.dll");if(crt){h.allocate=(void*(*)(size_t))GetProcAddress(crt,"malloc");h.release=(void(*)(void*))GetProcAddress(crt,"free");}}
  if(!h.allocate||!h.release){h.allocate=::malloc;h.release=::free;}return h;}();
 return heap;
}
// MSVC allocation: below 0x1000 bytes plain; otherwise 32-byte aligned with the raw pointer stored just before it.
inline uint64_t MoveBAllocate(uint64_t bytes){
 if(!bytes)return 0;auto& h=MoveBHeap();
 if(bytes<0x1000)return (uint64_t)h.allocate((size_t)bytes);
 void* raw=h.allocate((size_t)(bytes+0x27));if(!raw)return 0;const uint64_t aligned=((uint64_t)raw+0x27)&~0x1full;*(uint64_t*)(aligned-8)=(uint64_t)raw;return aligned;
}
inline void MoveBFree(uint64_t pointer,uint64_t bytes){
 if(!pointer)return;auto& h=MoveBHeap();
 if(bytes>=0x1000){const uint64_t raw=*(uint64_t*)(pointer-8);if(pointer-8-raw>0x1f)return;h.release((void*)raw);return;}
 h.release((void*)pointer);
}
// Appends one element to an MSVC std::vector (begin, end, capacity) whose elements are `size` bytes.
inline bool MoveBPush(uint64_t vector,const void* element,uint32_t size){
 uint64_t begin=*(uint64_t*)vector,end=*(uint64_t*)(vector+8),capacity=*(uint64_t*)(vector+0x10);
 if(end!=capacity){memcpy((void*)end,element,size);*(uint64_t*)(vector+8)=end+size;return true;}
 const uint64_t count=(end-begin)/size,limit=((~0ull)>>1)/size;if(count+1>limit)return false;
 uint64_t grown=count+(count>>1);if(grown<count+1)grown=count+1;if(grown>limit)grown=limit;
 const uint64_t fresh=MoveBAllocate(grown*size);if(!fresh)return false;
 memcpy((void*)(fresh+count*size),element,size);if(count)memcpy((void*)fresh,(void*)begin,(size_t)(count*size));
 if(begin)MoveBFree(begin,capacity-begin);
 *(uint64_t*)vector=fresh;*(uint64_t*)(vector+8)=fresh+(count+1)*size;*(uint64_t*)(vector+0x10)=fresh+grown*size;return true;
}
// Resizes a vector of pointers (the EnTT page table) to `count` entries, new entries zero.
inline bool MoveBResizePointers(uint64_t vector,uint64_t count){
 const uint64_t begin=*(uint64_t*)vector,end=*(uint64_t*)(vector+8),capacity=*(uint64_t*)(vector+0x10);
 if(count<=(capacity-begin)/8){if(count>(end-begin)/8)memset((void*)end,0,(size_t)((count-(end-begin)/8)*8));*(uint64_t*)(vector+8)=begin+count*8;return true;}
 const uint64_t old=(capacity-begin)/8;uint64_t grown=old+(old>>1);if(grown<count)grown=count;
 const uint64_t fresh=MoveBAllocate(grown*8);if(!fresh)return false;
 const uint64_t used=(end-begin)/8;if(used)memcpy((void*)fresh,(void*)begin,(size_t)(used*8));memset((void*)(fresh+used*8),0,(size_t)((grown-used)*8));
 if(begin)MoveBFree(begin,capacity-begin);
 *(uint64_t*)vector=fresh;*(uint64_t*)(vector+8)=fresh+count*8;*(uint64_t*)(vector+0x10)=fresh+grown*8;return true;
}
// EnTT basic_sparse_set::push for an entity that is not in the set (deletion policy: 0 swap-and-pop, 1 in-place, 2 swap-only).
inline bool MoveBSparsePush(uint64_t set,uint32_t entity){
 const uint64_t pageIndex=(entity&0x3ffff)>>11;const uint32_t slot=entity&0x7ff;
 uint64_t begin=*(uint64_t*)(set+8),end=*(uint64_t*)(set+0x10);
 if(pageIndex>=(end-begin)/8&&!MoveBResizePointers(set+8,pageIndex+1))return false;
 begin=*(uint64_t*)(set+8);uint64_t page=*(uint64_t*)(begin+pageIndex*8);
 if(!page){const uint64_t raw=(uint64_t)MoveBHeap().allocate(0x2027);if(!raw)return false;page=(raw+0x27)&~0x1full;*(uint64_t*)(page-8)=raw;*(uint64_t*)(begin+pageIndex*8)=page;memset((void*)page,0xff,0x2000);}
 const uint8_t policy=*(uint8_t*)(set+0x40);
 auto sparseSlot=[&](uint32_t e)->uint32_t*{const uint64_t pages=*(uint64_t*)(set+8);const uint64_t p=*(uint64_t*)(pages+(uint64_t)((e&0x3ffff)>>11)*8);return (uint32_t*)(p+(uint64_t)(e&0x7ff)*4);};
 auto appendPacked=[&]()->bool{if(!MoveBPush(set+0x20,&entity,4))return false;const uint64_t packedBegin=*(uint64_t*)(set+0x20),packedEnd=*(uint64_t*)(set+0x28);
  ((uint32_t*)page)[slot]=(entity&0xfffc0000u)|((uint32_t)((packedEnd-packedBegin)/4-1)&0x3ffff);return true;};
 if(policy==0)return appendPacked();
 if(policy==1){const uint64_t head=*(uint64_t*)(set+0x48);
  if(head!=0x3ffff){((uint32_t*)page)[slot]=(entity&0xfffc0000u)|((uint32_t)head&0x3ffff);uint32_t* packed=(uint32_t*)*(uint64_t*)(set+0x20);const uint32_t previous=packed[head];packed[head]=entity;*(uint64_t*)(set+0x48)=previous&0x3ffff;return true;}
  return appendPacked();}
 if(policy==2){const uint32_t found=((uint32_t*)page)[slot]&0x3ffff;
  if(found==0x3ffff){if(!appendPacked())return false;}
  else{((uint32_t*)page)[slot]=(entity&0xfffc0000u)|found;((uint32_t*)*(uint64_t*)(set+0x20))[found]=entity;}
  const uint64_t head=*(uint64_t*)(set+0x48);*(uint64_t*)(set+0x48)=head+1;
  const uint32_t position=((uint32_t*)page)[slot]&0x3ffff;uint32_t* packed=(uint32_t*)*(uint64_t*)(set+0x20);
  uint32_t first=packed[position];*sparseSlot(first)=(first&0xfffc0000u)|((uint32_t)head&0x3ffff);
  uint32_t second=packed[head];*sparseSlot(second)=(second&0xfffc0000u)|position;
  const uint32_t swap=packed[position];packed[position]=packed[head];packed[head]=swap;return true;}
 return false;
}

// ---- shared reads ------------------------------------------------------------------------------------------------
inline bool MoveBClientLevel(const Context& c){uint64_t level{};return Read(c.actor+0x200,&level)&&(uint64_t)currentTickLevel==level;}
// Wanted horizontal direction from the keys (nothing when MoveInput+0x60 bit 0x40 is set or no key is held).
inline bool MoveBDirection(const Context& c,float* x,float* z){
 const uintptr_t input=Move3Input(c);if(!input)return false;uint32_t bits{};uint8_t flags{};if(!Read(input+0x10,&bits)||!Read(input+0x60,&flags)||(flags&0x40))return false;
 const float f=(float)((bits>>13)&1)-(float)((bits>>14)&1),s=(float)((bits>>16)&1)-(float)((bits>>15)&1);const float sum=f*f+s*s;if(sum==0.f||std::isnan(sum))return false;
 float pitch{},yaw{};if(!RotRead(c.actor,&pitch,&yaw))return false;const float n=std::sqrt(sum),r=(yaw+90.f)*0.017453292f,co=std::cos(r),si=std::sin(r),a=f/n,b=s/n;
 *x=co*a-si*b;*z=co*b+si*a;return true;
}

// ---- Air Jump -----------------------------------------------------------------------------------------------------
inline void AirJumpWork(const Context& c){
 uintptr_t input=Move3Input(c);uint32_t bits{};if(!input||!Read(input+0x10,&bits)||!((bits>>25)&1))return;
 if(LocalHas(c,0xc29078a0u))return;
 uintptr_t registry{};uint32_t entity{};if(!ActorEntity(c.actor,&registry,&entity))return;
 uintptr_t storage{};if(!Storage(registry,0xda6293f1u,&storage))return;   // created by the game on the first ground jump
 uint32_t packed{};if(Packed(storage,entity,&packed))return;
 uint8_t policy{};if(!Read(storage+0x40,&policy)||policy>2)return;
 MoveBSparsePush(storage,entity);
}

// ---- Long Jump ----------------------------------------------------------------------------------------------------
// LocalPlayerJump, before the game's jump (arg7, arg8 unused here).
inline void LongJumpPreWork(){
 auto& m=MoveB();if(!m.longJump)return;Context c{};if(!Local(&c)||!StickyHasInput(c))return;float x{},z{};if(!MoveBDirection(c,&x,&z))return;
 std::lock_guard<std::mutex> lock(m.guard);m.ljVelocity=m.ljSpeed;m.ljTicks=m.ljAirTicks;m.ljActive=true;m.ljLeft=false;m.ljPending=true;m.ljPendingX=x*m.ljSpeed;m.ljPendingZ=z*m.ljSpeed;
}
inline void LongJumpPre(uint64_t,uint64_t){static volatile long stopped_=0;if(!stopped_)__try{LongJumpPreWork();}__except(game::FeatureFault("LongJumpPre",&stopped_,GetExceptionInformation())){}}
// After the game's jump: arg8 is the movement state (velocity x +0x18, y +0x1c, z +0x20).
inline void LongJumpPostWork(uint64_t arg8){
 auto& m=MoveB();if(!m.longJump||!arg8)return;float x,z,height;
 {std::lock_guard<std::mutex> lock(m.guard);if(!m.ljPending)return;m.ljPending=false;x=m.ljPendingX;z=m.ljPendingZ;height=m.ljHeight;}
 *(float*)(arg8+0x18)=x;*(float*)(arg8+0x20)=z;*(float*)(arg8+0x1c)+=height;
}
inline void LongJumpPost(uint64_t arg8){static volatile long stopped_=0;if(!stopped_)__try{LongJumpPostWork(arg8);}__except(game::FeatureFault("LongJumpPost",&stopped_,GetExceptionInformation())){}}
inline void LongJumpTickWork(const Context& c){
 auto& m=MoveB();std::lock_guard<std::mutex> lock(m.guard);
 if(!LocalHas(c,0xc29078a0u)){
  if(!m.ljActive)return;m.ljLeft=true;if(m.ljTicks<=0)return;--m.ljTicks;m.ljVelocity*=m.ljRetention/100.f;
  float x{},z{};uint64_t state{};if(MoveBDirection(c,&x,&z)&&Read(c.actor+0x240,&state)&&state){*(float*)(state+0x18)=x*m.ljVelocity;*(float*)(state+0x20)=z*m.ljVelocity;}
  return;}
 if(m.ljLeft){m.ljVelocity=0;m.ljTicks=0;m.ljActive=false;m.ljLeft=false;}
 float x{},z{};if(m.ljAutoJump&&StickyHasInput(c)&&MoveBDirection(c,&x,&z))JumpRequest(c.actor);
}

// ---- LevelTick (client level only): Air Jump, Long Jump; also records the client thread for Jesus ---------------
inline void MoveBTickWork(){
 auto& m=MoveB();
 if(m.wasLongJump&&!m.longJump){std::lock_guard<std::mutex> lock(m.guard);m.ljVelocity=0;m.ljTicks=0;m.ljActive=false;m.ljLeft=false;m.ljPending=false;}
 m.wasLongJump=m.longJump.load();
 Context c{};if(!Local(&c)||!MoveBClientLevel(c))return;
 m.clientThread.store((long)GetCurrentThreadId());
 if(m.airJump)AirJumpWork(c);
 if(m.longJump)LongJumpTickWork(c);
}
inline void MoveBTick(){static volatile long stopped_=0;if(!stopped_)__try{MoveBTickWork();}__except(game::FeatureFault("MoveBTick",&stopped_,GetExceptionInformation())){}}
// True when the level tick subscriber is needed (the menu keeps it installed while this holds).
inline bool MoveBNeedsTick(){auto& m=MoveB();return m.airJump||m.longJump||m.wasLongJump||m.jesus;}

// ---- Reverse Step (SprintIntent: input +0x10 bits, state = actor+0x240) ------------------------------------------
inline bool ReverseStepAllowed(const Context& c,uint64_t input,float vy){
 if(!(vy<=0.f))return false;uint32_t bits{};if(!Read(input+0x10,&bits)||((bits>>7)&1)||(bits&1))return false;
 uintptr_t flagsElement{};uint64_t flags{};if(!Element(c.registry,c.entity,"ActorDataFlagComponent",0x18,&flagsElement)||!Read(flagsElement,&flags)||(flags&2)||(flags&0x20))return false;
 if(const uintptr_t abilities=Move3Abilities(c)){uint8_t word{};if(Read(abilities+4,&word)&&(word&1))return false;}
 if(LocalHas(c,0x78e89f39u)||LocalHas(c,0x832a2768u))return false;return true;
}
inline bool MoveBContains(const std::string& name,const char* part){return name.find(part)!=std::string::npos;}
inline bool MoveBClimbable(const Context& c,uint64_t region,const float* bb){
 const int32_t pos[3]={(int32_t)std::floor((bb[0]+bb[3])*.5f),(int32_t)std::floor(bb[1]+.001f),(int32_t)std::floor((bb[2]+bb[5])*.5f)};
 const uint64_t block=RegionBlock(c,region,pos);std::string name;if(!block||!XrayBlockName(block,&name))return false;
 return MoveBContains(name,"ladder")||MoveBContains(name,"vine")||MoveBContains(name,"scaffolding");
}
inline bool MoveBLiquid(const Context& c,uint64_t region,const int32_t* pos){
 auto liquid=[](uint64_t block){uint8_t a{},b{};return block&&BlockFlags(block,&a,&b)&&(b&2);};
 if(liquid(RegionBlock(c,region,pos)))return true;
 const uint64_t layer=Virtual0(c,region,0x28);return layer&&liquid(reinterpret_cast<uint64_t(*)(uint64_t,const int32_t*)>(layer)(region,pos));
}
// Highest top face of the collision shapes under the player within [limit, minY + 0.001]; false when none or a flagged shape.
inline bool ReverseStepFloor(const Context& c,uint64_t region,const float* bb,float limit,float* found){
 auto& m=MoveB();const uint64_t fetch=Virtual0(c,region,0xb0);if(!fetch)return false;
 constexpr uint64_t elements=0x2000,stride=0x30;
 if(!m.rsShapes[0]||m.rsShapes[2]-m.rsShapes[0]<elements*stride){if(m.rsShapes[0])MoveBFree(m.rsShapes[0],m.rsShapes[2]-m.rsShapes[0]);
  const uint64_t buffer=MoveBAllocate(elements*stride);if(!buffer){m.rsShapes[0]=m.rsShapes[1]=m.rsShapes[2]=0;return false;}m.rsShapes[0]=m.rsShapes[1]=buffer;m.rsShapes[2]=buffer+elements*stride;}
 m.rsShapes[1]=m.rsShapes[0];
 const float box[6]={bb[0],limit,bb[2],bb[3],bb[1],bb[5]};
 reinterpret_cast<void(*)(uint64_t,void*,const float*,int,uint64_t,uint64_t)>(fetch)(region,m.rsShapes,box,1,0,0);   // 6 arguments, as the movement simulator calls it
 bool any=false;float best=0;
 for(uint64_t e=m.rsShapes[0];e!=m.rsShapes[1];e+=stride){
  uint8_t flagged{};float minX{},minZ{},maxX{},maxY{},maxZ{};
  if(!Read(e+0x2c,&flagged)||!Read(e+8,&minX)||!Read(e+0x10,&minZ)||!Read(e+0x14,&maxX)||!Read(e+0x18,&maxY)||!Read(e+0x1c,&maxZ))return false;
  if(flagged)return false;
  if(maxY<=bb[1]+.001f&&limit<=maxY&&minX<bb[3]&&bb[0]<maxX&&minZ<bb[5]&&bb[2]<maxZ){if(!any||maxY>best)best=maxY;any=true;}
 }
 if(!any)return false;*found=best;return true;
}
inline void ReverseStepWork(uint64_t input,uint64_t state,uint64_t){
 auto& m=MoveB();
 if(m.rsRelease.exchange(false)){if(m.rsShapes[0])MoveBFree(m.rsShapes[0],m.rsShapes[2]-m.rsShapes[0]);m.rsShapes[0]=m.rsShapes[1]=m.rsShapes[2]=0;m.rsGround=false;}
 if(!m.reverseStep||!input||!state)return;
 Context c{};uint64_t mine{},boxPointer{};if(!Local(&c)||!Read(c.actor+0x240,&mine)||mine!=state||!Read(c.actor+0x248,&boxPointer)||!boxPointer)return;
 float bb[6];for(int i=0;i<6;++i)if(!Read(boxPointer+i*4,&bb[i]))return;
 float height;bool position;
 {std::lock_guard<std::mutex> lock(m.guard);height=m.rsHeight;position=m.rsPosition;}
 if(LocalHas(c,0xc29078a0u)){m.rsY=bb[1];m.rsGround=true;return;}
 const float y0=m.rsY;const bool was=m.rsGround;m.rsGround=false;if(!was)return;
 if(!StickyHasInput(c))return;const uint64_t region=ActorRegion(c);if(!region)return;
 float vy{};if(!Read(state+0x1c,&vy)||!ReverseStepAllowed(c,input,vy)||MoveBClimbable(c,region,bb))return;
 const float limit=(y0-height)-.001f;float floorY{};if(!ReverseStepFloor(c,region,bb,limit,&floorY))return;
 if(bb[1]-floorY<.1f)return;
 const int32_t pos[3]={(int32_t)std::floor((bb[0]+bb[3])*.5f),(int32_t)std::floor(floorY+.001f),(int32_t)std::floor((bb[2]+bb[5])*.5f)};if(MoveBLiquid(c,region,pos))return;
 const float drop=bb[1]-floorY;float* sv=(float*)state;
 if(position){
  const float oldY=sv[1],newY=oldY-drop,dy=newY-oldY;const float x=sv[0],z=sv[2];
  sv[1]=newY;sv[3]=x;sv[4]=newY;sv[5]=z;sv[6]=0;sv[7]=0;sv[8]=0;   // position, previous position, zero velocity
  float* box=(float*)boxPointer;box[1]+=dy;box[4]+=dy;sv[7]=0;}
 else sv[7]=-(drop+.01f);
}
inline void ReverseStepSprint(uint64_t input,uint64_t state,uint64_t flags){static volatile long stopped_=0;if(!stopped_)__try{ReverseStepWork(input,state,flags);}__except(game::FeatureFault("ReverseStepSprint",&stopped_,GetExceptionInformation())){}}
// Frees the shape buffer when the module is switched off.
// Menu thread: only requests the release; the buffer is freed on the game thread at the next SprintIntent.
inline void ReverseStepRelease(){MoveB().rsRelease=true;}

// ---- Parkour (SprintIntent: input = MoveInput, state = position state of the event's actor) -------------------------
inline float ParkourThreshold(const Context& c){
 if(MoveB().pkMinimumDrop)return 1.0010000467300415f;
 uintptr_t storage{},element{};float step{};
 if(Storage(c.registry,0x9aae5d7fu,&storage)&&InStorage(storage,c.entity,4,&element)&&Read(element,&step))return step+0.0010000000474974513f;
 return 0.5634999871253967f;
}
// Simulated jump decision (see the header comment).
inline bool ParkourSimulate(const Context& c,uint64_t region,float threshold){
 MsSim sim;if(!MsInit(c,region,&MoveB().pkShapes,sim))return false;
 float startY;memcpy(&startY,sim.state.m+0x50,4);
 for(unsigned i=0;;++i){
  if(0x13<i)return true;
  MsOutput out;MsTick(sim.env,out,sim.state,sim.input);
  if(MsOutFailed(out))return false;
  const bool ground=MsOutGround(out);
  if(ground&&i!=0)return threshold<startY-MsOutPos(out)[1];
  if(i==0&&ground)return false;
 }
}
inline void ParkourWork(uint64_t input,uint64_t state){
 auto& m=MoveB();
 if(m.pkRelease.exchange(false))MsShapesFree(m.pkShapes);
 if(!m.parkour||!input||!state)return;
 Context c{};uint64_t mine{},boxPointer{};if(!Local(&c)||!Read(c.actor+0x240,&mine)||mine!=state)return;
 if(!StickyHasInput(c))return;const uint64_t region=ActorRegion(c);if(!region||!LocalHas(c,0xc29078a0u))return;
 uint32_t bits{};if(!Read(input+0x10,&bits)||((bits>>7)&1)||(bits&1))return;
 uintptr_t flagsElement{};uint64_t flags{};if(!Element(c.registry,c.entity,"ActorDataFlagComponent",0x18,&flagsElement)||!Read(flagsElement,&flags)||((flags>>1)&1))return;
 if(const uintptr_t abilities=Move3Abilities(c)){uint8_t word{};if(Read(abilities+4,&word)&&(word&1))return;}
 if(LocalHas(c,0x78e89f39u)||LocalHas(c,0x832a2768u))return;
 if(m.pkSprintOnly&&!((flags>>3)&1))return;
 float vx{},vz{};if(!Read(state+0x18,&vx)||!Read(state+0x20,&vz)||!(1.0000001111620804e-06f<vx*vx+vz*vz))return;
 if(!Read(c.actor+0x248,&boxPointer)||!boxPointer)return;float bb[6];for(int i=0;i<6;++i)if(!Read(boxPointer+i*4,&bb[i]))return;
 if(MoveBClimbable(c,region,bb))return;
 if(ParkourSimulate(c,region,ParkourThreshold(c)))JumpRequest(c.actor);
}
inline void ParkourSprint(uint64_t input,uint64_t state,uint64_t){static volatile long stopped_=0;if(!stopped_)__try{ParkourWork(input,state);}__except(game::FeatureFault("ParkourSprint",&stopped_,GetExceptionInformation())){}}
// Menu thread: the collision buffer is freed on the game thread at the next SprintIntent.
inline void ParkourRelease(){MoveB().pkRelease=true;}

// Slot assigned to the SprintIntent subscriber: Strafe (game_movement.hpp) first, then Reverse Step, then Parkour.
inline void MovementSprintB(uint64_t input,uint64_t state,uint64_t flags){MovementSprint(input,state,flags);ReverseStepSprint(input,state,flags);ParkourSprint(input,state,flags);}

// ---- Safe Walk (SneakMovement hook, RVA 0xb3298f0) ---------------------------------------------------------------
struct alignas(16) MoveBCopy16{uint8_t bytes[16];};
struct SafeWalkSaved{uint64_t input;uint32_t sneak;uint64_t flagsAt;uint64_t flags[3];uint64_t arg7;};
using SafeWalkFunction=void(*)(uint64_t,uint64_t,uint64_t,uint64_t,const void*,const void*,uint64_t,uint64_t);
inline SafeWalkFunction safeWalkOriginal=nullptr;
inline bool SafeWalkBeforeWork(uint64_t flags,const void* inputRef,const void* memberRef,uint64_t arg7,SafeWalkSaved* saved){
 uint64_t memberStorage{},inputStorage{};uint32_t memberEntity{},inputEntity{},packed{};
 if(!flags||!Read((uintptr_t)memberRef,&memberStorage)||!memberStorage||!Read((uintptr_t)memberRef+8,&memberEntity)||!Packed(memberStorage,memberEntity,&packed))return false;
 uintptr_t input{};if(!Read((uintptr_t)inputRef,&inputStorage)||!inputStorage||!Read((uintptr_t)inputRef+8,&inputEntity)||!InStorage(inputStorage,inputEntity,100,&input))return false;
 uint32_t word{};if(!Read(input,&word))return false;
 saved->input=input;saved->sneak=word&1;saved->flagsAt=flags;saved->arg7=arg7;for(int i=0;i<3;++i)if(!Read(flags+i*8,&saved->flags[i]))return false;
 *(uint32_t*)input=word|1;uint64_t* bits=(uint64_t*)flags;bits[0]|=1ull<<1;bits[0]&=~(1ull<<6);return true;
}
inline void SafeWalkAfterWork(const SafeWalkSaved& saved){
 *(uint32_t*)saved.input=(*(uint32_t*)saved.input&~1u)|saved.sneak;
 for(int i=0;i<3;++i)((uint64_t*)saved.flagsAt)[i]=saved.flags[i];
 if(saved.arg7)*(uint8_t*)(saved.arg7+0x55)=(uint8_t)saved.sneak;
}
inline bool SafeWalkBefore(uint64_t flags,const void* a,const void* b,uint64_t g,SafeWalkSaved* s){__try{return SafeWalkBeforeWork(flags,a,b,g,s);}__except(EXCEPTION_EXECUTE_HANDLER){return false;}}
inline void SafeWalkAfter(const SafeWalkSaved& s){static volatile long stopped_=0;if(!stopped_)__try{SafeWalkAfterWork(s);}__except(game::FeatureFault("SafeWalkAfter",&stopped_,GetExceptionInformation())){}}
inline void SafeWalkDetour(uint64_t a,uint64_t flags,uint64_t c,uint64_t d,const void* inputRef,const void* memberRef,uint64_t g,uint64_t h){
 SafeWalkSaved saved{};const bool armed=MoveB().safeWalk&&SafeWalkBefore(flags,inputRef,memberRef,g,&saved);
 // The game receives the addresses of local copies of the two 16-byte references.
 MoveBCopy16 first{},second{};const bool copied=Read((uintptr_t)inputRef,&first)&&Read((uintptr_t)memberRef,&second);
 safeWalkOriginal(a,flags,c,d,copied?(const void*)&first:inputRef,copied?(const void*)&second:memberRef,g,h);
 if(armed)SafeWalkAfter(saved);
}
// 1: installed; -1: refused (unexpected image, prologue mismatch or MinHook failure). Attempted once.
inline int InstallSafeWalkHook(void(*log)(const char*)){
 static int state=0;if(state)return state;state=-1;uintptr_t base{};uint32_t size{};if(!Supported(&base,&size))return state;
 const uint8_t bytes[16]={0x41,0x57,0x41,0x56,0x56,0x57,0x53,0x48,0x81,0xec,0xb0,0x00,0x00,0x00,0x44,0x0f};
 for(int i=0;i<16;++i){uint8_t b{};if(!Read(base+0xb3298f0+i,&b)||b!=bytes[i]){if(log)log("Safe Walk: unexpected code at the sneak movement function, no hook installed");return state;}}
 void* target=(void*)(base+0xb3298f0);
 if(MH_CreateHook(target,(void*)SafeWalkDetour,(void**)&safeWalkOriginal)!=MH_OK){if(log)log("Safe Walk: hook not created");return state;}
 if(MH_EnableHook(target)!=MH_OK){MH_RemoveHook(target);if(log)log("Safe Walk: hook not enabled");return state;}
 if(log)log("Safe Walk: hook installed");state=1;return state;
}

// ---- Jesus (FetchCollisionShapes hook, RVA 0x360a1f0) -------------------------------------------------------------
inline void(*volatile shapesPhase)(uint64_t entity,uint64_t box,uint64_t list)=nullptr;   // Phase (game_movement_c.hpp) shares this hook
using JesusFunction=void(*)(uint64_t,uint64_t,uint64_t,const void*,uint64_t,const void*,uint64_t,uint64_t,uint64_t,uint64_t,uint64_t,uint64_t,uint64_t,uint64_t,uint64_t);
inline JesusFunction jesusOriginal=nullptr;
// entity: EntityId*; box: query AABB (min xyz, max xyz); result: +0x40 requested Y move, shape vectors at +0x58 and +0x70.
inline void JesusWork(uint64_t entity,uint64_t box,uint64_t result,uint64_t region){
 auto& m=MoveB();if(!m.jesus||!region||(long)GetCurrentThreadId()!=m.clientThread.load())return;
 Context c{};if(!Local(&c))return;uint32_t mine{};int32_t asked{};if(!Read(c.actor+0x18,&mine)||!Read(entity,&asked)||(int32_t)mine!=asked)return;
 float bb[6];for(int i=0;i<6;++i)if(!Read(box+i*4,&bb[i]))return;float move{};if(!Read(result+0x40,&move))return;const float dy=move<=0.f?move:0.f;
 const int x0=(int)std::floor(bb[0]+.001f),x1=(int)std::floor(bb[3]-.001f),z0=(int)std::floor(bb[2]+.001f),z1=(int)std::floor(bb[5]-.001f);
 const int yHigh=(int)std::floor(bb[1]-.001f),yLow=(int)std::floor((dy+bb[1])-.001f);
 const uint64_t layer=Virtual0(c,region,0x28);if(!layer)return;
 auto blockAt=reinterpret_cast<uint64_t(*)(uint64_t,const int32_t*)>(layer);auto surface=reinterpret_cast<float(*)(uint64_t,const int32_t*)>(c.base+0x3534c60);
 if(x0>x1||z0>z1||yLow>yHigh)return;
 for(int x=x0;x<=x1;++x)for(int z=z0;z<=z1;++z)for(int y=yHigh;y>=yLow;--y){
  const int32_t pos[3]={x,y,z};const uint64_t block=blockAt(region,pos);uint8_t a{},b{};if(!block||!BlockFlags(block,&a,&b)||!(b&2))continue;
  const float h=surface(block,pos);
  // The game ignores a box the player already overlaps: at a shore level with the water (feet at the surface) a top at
  // surface + 0.05 was skipped and the player sank, so the top stops at the surface whenever the feet are not above it.
  if(bb[1]<h-.001f)continue;   // already below the surface: swimming stays possible
  const float top=bb[1]>=h+.05f?h+.05f:h;
  const float shape[6]={(float)x,(float)y,(float)z,(float)(x+1),top,(float)(z+1)};const uint8_t zero[0x18]{};
  if(!MoveBPush(result+0x58,shape,0x18)||!MoveBPush(result+0x70,zero,0x18))return;
 }
}
inline void JesusGuarded(uint64_t entity,uint64_t box,uint64_t result,uint64_t region){static volatile long stopped_=0;if(!stopped_)__try{JesusWork(entity,box,result,region);}__except(game::FeatureFault("JesusGuarded",&stopped_,GetExceptionInformation())){}}
inline void JesusDetour(uint64_t a,uint64_t b,uint64_t c,const void* d,uint64_t e,const void* f,uint64_t g,uint64_t h,uint64_t i,uint64_t j,uint64_t k,uint64_t l,uint64_t m,uint64_t n,uint64_t o){
 // The game receives the addresses of local copies of the two 16-byte references (arg4 and arg6).
 MoveBCopy16 first{},second{};const bool copied=Read((uintptr_t)d,&first)&&Read((uintptr_t)f,&second);
 jesusOriginal(a,b,c,copied?(const void*)&first:d,e,copied?(const void*)&second:f,g,h,i,j,k,l,m,n,o);
 if(MoveB().jesus)JesusGuarded(a,b,e,j);
 if(auto p=shapesPhase)p(a,b,e);
}
inline int InstallJesusHook(void(*log)(const char*)){
 static int state=0;if(state)return state;state=-1;uintptr_t base{};uint32_t size{};if(!Supported(&base,&size))return state;
 const uint8_t bytes[16]={0x55,0x41,0x57,0x41,0x56,0x41,0x55,0x41,0x54,0x56,0x57,0x53,0x48,0x81,0xec,0xd8};
 for(int i=0;i<16;++i){uint8_t v{};if(!Read(base+0x360a1f0+i,&v)||v!=bytes[i]){if(log)log("Jesus: unexpected code at the collision shapes function, no hook installed");return state;}}
 void* target=(void*)(base+0x360a1f0);
 if(MH_CreateHook(target,(void*)JesusDetour,(void**)&jesusOriginal)!=MH_OK){if(log)log("Jesus: hook not created");return state;}
 if(MH_EnableHook(target)!=MH_OK){MH_RemoveHook(target);if(log)log("Jesus: hook not enabled");return state;}
 if(log)log("Collision shapes (Jesus, Phase): hook installed");state=1;return state;
}
}
