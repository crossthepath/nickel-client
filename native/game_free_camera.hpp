// Freecam: native adapter for the nickel::visual core (include/nickel/visual_free_camera.hpp).
// Hooks owned by other modules are not installed twice: level tick (#249), render prep (#21) and held-item
// render (#230) expose subscription points (levelTickListener, levelRenderPrep, handRenderCancel). The
// seven own hooks are installed only after their prologues are verified; any failure removes every hook
// created so far.
#pragma once
#include "game_packet_hooks.hpp"
#include "game_map.hpp"
#include "game_world.hpp"
#include "../include/nickel/visual_free_camera.hpp"
#include <MinHook.h>
#include <cmath>
#include <cstring>
#include <mutex>

namespace game {
// Shared state (0x30-byte component layout): camera.target +0x00, smoothed camera.position +0x0c,
// yaw/pitch +0x18/+0x1c, capturedActor +0x20, captured +0x28, camera.initialized +0x29.
struct FreeCameraShared {
 std::mutex guard;nickel::visual::CameraSettings settings{};nickel::visual::FreeCamera camera{};
 uintptr_t capturedActor=0;bool captured=false;
 volatile long active=0,fault=0,seen=0;void(*log)(const char*)=nullptr;
};
inline FreeCameraShared& FreeCameraState(){static auto* state=new FreeCameraShared;return *state;}
inline void FreeCameraFault(){auto& shared=FreeCameraState();InterlockedExchange(&shared.active,0);InterlockedExchange(&handRenderCancel,0);
 if(InterlockedExchange(&shared.fault,1)==0&&shared.log)shared.log("Freecam: a guarded game access failed; camera changes disabled until restart");}
// Lock-free fast path: called from very hot hooks (packet send, rendering).
inline bool FreeCameraActive(){auto& shared=FreeCameraState();return shared.active&&!shared.fault;}

// World identity: local actor + StartGame serial + dimension.
inline bool FreeCameraLocal(Context* context,uint32_t* serial,int* dimension){
 if(!Local(context)||!ReadDimension(dimension))return false;auto& events=Shared();std::lock_guard<std::mutex> lock(events.guard);*serial=events.worldSerial;return true;
}
// If the local actor changed, remember it, re-read ActorDataFlagComponent bit 3 and reset `initialized`.
inline bool FreeCameraRefreshActor(const Context& context,bool* captured){
 auto& shared=FreeCameraState();
 {std::lock_guard<std::mutex> lock(shared.guard);if(shared.capturedActor==context.actor){*captured=shared.captured;return true;}}
 uintptr_t flags{};uint64_t bits{};if(!Element(context.registry,context.entity,"ActorDataFlagComponent",0x18,&flags)||!Read(flags,&bits))return false;
 std::lock_guard<std::mutex> lock(shared.guard);shared.capturedActor=context.actor;shared.captured=(bits&8)!=0;shared.camera={};*captured=shared.captured;return true;
}
inline bool FreeCameraInitialized(){
 if(!FreeCameraActive())return false;Context context{};uint32_t serial{};int dimension{};if(!FreeCameraLocal(&context,&serial,&dimension))return false;
 auto& shared=FreeCameraState();std::lock_guard<std::mutex> lock(shared.guard);return nickel::visual::camera_same_world(shared.camera,context.actor,serial,dimension);
}
// Called by the menu every frame. Enabling and disabling both clear the state.
inline void ConfigureFreeCamera(bool enabled,const nickel::visual::CameraSettings& settings){
 auto& shared=FreeCameraState();const bool wanted=enabled&&!shared.fault&&nickel::visual::camera_settings_valid(settings);
 {std::lock_guard<std::mutex> lock(shared.guard);if(wanted!=(shared.active!=0)){shared.camera={};shared.capturedActor=0;shared.captured=false;}shared.settings=settings;}
 InterlockedExchange(&shared.active,wanted?1:0);InterlockedExchange(&handRenderCancel,wanted?1:0);
}

// ---- Camera (CameraSetupEvent, hook #234, runs before the game)
// Query on the local player's registry: storages 0x4f6047c7 (0x120-byte element: quaternion +0x30, position
// +0x40) and 0x119e772b (marker).
inline int FreeCameraComponents(const Context& context,uintptr_t* cameras,int capacity){
 uintptr_t storage{},tag{};uint64_t begin{},end{};
 if(!Storage(context.registry,0x4f6047c7u,&storage)||!Storage(context.registry,0x119e772bu,&tag)||!Read(storage+0x20,&begin)||!Read(storage+0x28,&end)||end<begin||(end-begin)%4||(end-begin)>4*65536)return 0;
 int count=0;for(uint64_t at=begin;at<end&&count<capacity;at+=4){uint32_t entity{},packed{};uintptr_t element{};if(!Read(at,&entity))break;
  if(entity>=0xfffc0000u||!Packed(tag,entity,&packed)||!InStorage(storage,entity,0x120,&element))continue;cameras[count++]=element;}
 return count;
}
inline void FreeCameraSetupWork(){
 if(!FreeCameraActive())return;Context context{};uint32_t serial{};int dimension{};bool captured{};
 if(!FreeCameraLocal(&context,&serial,&dimension)||!FreeCameraRefreshActor(context,&captured))return;
 uintptr_t cameras[64]{};const int count=FreeCameraComponents(context,cameras,64);if(!count)return;
 // Walk the view from the end and initialise from the first camera found.
 const uintptr_t first=cameras[count-1];std::array<float,4> initial{};float position[3];
 if(!Floats(first+0x30,initial.data(),4)||!Floats(first+0x40,position,3))return;
 // Yaw/pitch come from the analytic inverse of the quaternion formula used when writing (round-trip tested).
 float yaw{},pitch{};if(!nickel::visual::camera_angles(initial,&yaw,&pitch))return;
 nickel::visual::CameraVector shown{};std::array<float,4> rotation{};
 {auto& shared=FreeCameraState();std::lock_guard<std::mutex> lock(shared.guard);if(!shared.active||shared.fault)return;
  if(!nickel::visual::camera_same_world(shared.camera,context.actor,serial,dimension)){
   if(!nickel::visual::camera_initialize(shared.camera,context.actor,serial,dimension,{position[0],position[1],position[2]},yaw,pitch,shared.settings))return;}
  else if(!nickel::visual::camera_smooth(shared.camera,shared.settings))return;
  shown=shared.camera.position;rotation=nickel::visual::camera_quaternion(shared.camera.yaw,shared.camera.pitch);}
 // No lock is held while writing into the game; every destination is re-read (finite values) before writing.
 const float output[3]={shown.x,shown.y,shown.z};
 for(int i=count-1;i>=0;--i){float old[7];if(!Floats(cameras[i]+0x30,old,7))continue;memcpy((void*)(cameras[i]+0x30),rotation.data(),16);memcpy((void*)(cameras[i]+0x40),output,12);}
 InterlockedExchange(&FreeCameraState().seen,1);
}
inline void FreeCameraSetupGuarded(){__try{FreeCameraSetupWork();}__except(EXCEPTION_EXECUTE_HANDLER){FreeCameraFault();}}

// ---- Movement (LevelTickEvent #249): MoveInputComponent (step 100), input bits at +0x10.
inline void FreeCameraTickWork(){
 if(!FreeCameraActive())return;Context context{};uint32_t serial{};int dimension{};if(!FreeCameraLocal(&context,&serial,&dimension))return;
 uintptr_t input{};uint32_t bits{};if(!Element(context.registry,context.entity,"MoveInputComponent",100,&input)||!Read(input+0x10,&bits))return;
 nickel::visual::CameraInput move{};move.forward=(bits&(1u<<13))!=0;move.backward=(bits&(1u<<14))!=0;move.left=(bits&(1u<<15))!=0;move.right=(bits&(1u<<16))!=0;move.up=(bits&(1u<<7))!=0;move.down=(bits&1)!=0;
 auto& shared=FreeCameraState();std::lock_guard<std::mutex> lock(shared.guard);
 if(shared.active&&nickel::visual::camera_same_world(shared.camera,context.actor,serial,dimension))nickel::visual::camera_move(shared.camera,move,shared.settings);
}
inline void FreeCameraTickGuarded(){__try{FreeCameraTickWork();}__except(EXCEPTION_EXECUTE_HANDLER){FreeCameraFault();}}

// ---- Rotation (TurnDeltaEvent #4): event +0x50 = rdx[1] -> yaw, +0x54 = -rdx[0] -> pitch.
// The game call is skipped only once the camera is initialised.
inline bool FreeCameraTurnWork(const float* delta){
 if(!FreeCameraInitialized())return false;float input[2];if(!Floats((uintptr_t)delta,input,2))return false;
 auto& shared=FreeCameraState();std::lock_guard<std::mutex> lock(shared.guard);return shared.active&&nickel::visual::camera_turn(shared.camera,input[1],-input[0]);
}
inline bool FreeCameraTurnGuarded(const float* delta){__try{return FreeCameraTurnWork(delta);}__except(EXCEPTION_EXECUTE_HANDLER){FreeCameraFault();return false;}}

// ---- SprintIntentEvent #19: +0x40 = arg8 (input), +0x48 = arg12 (movement), +0x60 = arg11 (flags, bits
// 1/3/0x22). No "initialised" condition here.
inline bool FreeCameraSprintWork(uintptr_t input,uintptr_t movement,uintptr_t flags){
 if(!FreeCameraActive())return false;Context context{};bool captured{};if(!Local(&context)||!FreeCameraRefreshActor(context,&captured))return false;
 struct InputBytes{uint8_t bytes[100];};struct MovementBytes{uint8_t bytes[11];};InputBytes checked{};MovementBytes checkedMovement{};uint64_t checkedFlags{};
 if(!input||!movement||!flags||!Read(input,&checked)||!Read(movement,&checkedMovement)||!Read(flags,&checkedFlags))return false;
 memset((void*)input,0,8);memset((void*)(input+7),0,8);*(uint8_t*)(input+0x11)&=0xfe;memset((void*)(input+0x24),0,8);memset((void*)(input+0x3c),0,24);
 *(uint8_t*)(input+0x60)=(uint8_t)((checked.bytes[0x60]&0xc0)|(captured?2:0));
 memset((void*)movement,0,8);*(uint8_t*)(movement+8)=captured?1:0;memset((void*)(movement+9),0,2);
 uint64_t next=checkedFlags&~((1ull<<1)|(1ull<<3)|(1ull<<34));if(captured)next|=8;*(uint64_t*)flags=next;return true;
}
inline void FreeCameraSprintGuarded(uintptr_t input,uintptr_t movement,uintptr_t flags){__try{FreeCameraSprintWork(input,movement,flags);}__except(EXCEPTION_EXECUTE_HANDLER){FreeCameraFault();}}

// ---- PacketSendEvent #214: PlayerAuthInputPacket (id 0x90) while the camera is initialised.
// Clears input bits in the bitset at +0x88 (< 0x42); zeroes +0x80 and +0x5c..+0x6b; +0x6c = copy of +0x30;
// +0x74 = forward vector (-sin(yaw)cos(pitch), -sin(pitch), cos(yaw)cos(pitch)) from pitch +0x30, yaw +0x34.
inline void FreeCameraPacketWork(uintptr_t packet){
 if(!packet||!FreeCameraInitialized())return;Context context{};if(!Supported(&context.base,&context.size))return;
 const uint64_t get=Virtual0(context,packet,8);if(!get||reinterpret_cast<int(*)(uintptr_t)>(get)(packet)!=0x90)return;
 struct AuthBytes{uint8_t bytes[0x98];};AuthBytes checked{};if(!Read(packet,&checked))return;
 float pitch{},yaw{};memcpy(&pitch,checked.bytes+0x30,4);memcpy(&yaw,checked.bytes+0x34,4);
 if(!std::isfinite(pitch)||!std::isfinite(yaw)||std::fabs(pitch)>360||std::fabs(yaw)>1e6f)return;   // validated before any write
 const double p=pitch*.0174532925199433,y=yaw*.0174532925199433;
 const float forward[3]={(float)(-std::sin(y)*std::cos(p)),(float)-std::sin(p),(float)(std::cos(y)*std::cos(p))};
 uint64_t bits[2];memcpy(bits,checked.bytes+0x88,16);
 static constexpr int clear[]={0,1,3,6,31,7,9,23,10,11,12,13,14,15,51,52,16,17,19,18,21,22,60,61,63,64};
 for(int bit:clear)bits[bit>>6]&=~(1ull<<(bit&63));
 memcpy((void*)(packet+0x88),bits,16);memset((void*)(packet+0x80),0,8);memset((void*)(packet+0x5c),0,16);memcpy((void*)(packet+0x6c),checked.bytes+0x30,8);memcpy((void*)(packet+0x74),forward,12);
}
inline void FreeCameraPacketGuarded(uintptr_t packet){__try{FreeCameraPacketWork(packet);}__except(EXCEPTION_EXECUTE_HANDLER){FreeCameraFault();}}

// ---- Detours
using FreeCameraSetupFunction=void(*)(void*,void*);inline FreeCameraSetupFunction freeCameraSetupOriginal=nullptr;
inline void FreeCameraSetupDetour(void* a,void* b){FreeCameraSetupGuarded();freeCameraSetupOriginal(a,b);}
using FreeCameraTurnFunction=void(*)(void*,float*);inline FreeCameraTurnFunction freeCameraTurnOriginal=nullptr;
inline void FreeCameraTurnDetour(void* a,float* delta){if(!FreeCameraTurnGuarded(delta))freeCameraTurnOriginal(a,delta);}
// LevelCullerTypeUpdateEvent #233: culler type forced to 5 before calling the game.
using FreeCameraCullFunction=void(*)(void*,uint8_t);inline FreeCameraCullFunction freeCameraCullOriginal=nullptr;
inline void FreeCameraCullDetour(void* a,uint8_t type){freeCameraCullOriginal(a,FreeCameraActive()?(uint8_t)5:type);}
// RenderPlayerModelEvent #25: result forced to 1, only during level render prep.
using FreeCameraModelFunction=bool(*)(void*);inline FreeCameraModelFunction freeCameraModelOriginal=nullptr;
inline bool FreeCameraModelDetour(void* a){const bool original=freeCameraModelOriginal(a);return FreeCameraActive()&&levelRenderPrep?true:original;}
// LocalPlayerJumpEvent #16: cancelled unconditionally while active; arg9..arg11 forwarded unchanged.
using FreeCameraJumpFunction=void(*)(uint64_t,uint64_t,uint64_t,uint64_t,uint64_t,uint64_t,uint64_t,uint64_t,uint64_t,uint64_t,uint64_t,uint64_t);inline FreeCameraJumpFunction freeCameraJumpOriginal=nullptr;
inline void(*volatile jumpPre)(uint64_t arg7,uint64_t arg8)=nullptr;inline bool(*volatile jumpCancel)()=nullptr;inline void(*volatile jumpPost)(uint64_t arg8)=nullptr;   // scripts and No Jump Delay / High Jump (game_movement2.hpp)
inline void FreeCameraJumpDetour(uint64_t a,uint64_t b,uint64_t c,uint64_t d,uint64_t e,uint64_t f,uint64_t g,uint64_t h,uint64_t i,uint64_t j,uint64_t k,uint64_t l){if(!FreeCameraActive()){if(auto pre=jumpPre)pre(g,h);if(auto cc=jumpCancel)if(cc())return;freeCameraJumpOriginal(a,b,c,d,e,f,g,h,i,j,k,l);if(auto post=jumpPost)post(h);}}
using FreeCameraSprintFunction=void(*)(uint64_t,uint64_t,uint64_t,uint64_t,uint64_t,uint64_t,uint64_t,uint64_t,uint64_t,uint64_t,uint64_t,uint64_t,uint64_t,uint64_t);inline FreeCameraSprintFunction freeCameraSprintOriginal=nullptr;
inline void(*volatile sprintUtility)(uint64_t input,uint64_t state)=nullptr;
inline void(*volatile sprintScript)(uint64_t input,uint64_t state,uint64_t movement)=nullptr;   // scripts: move event; jump requests come from game_actions.hpp
inline void(*volatile sprintPvpBot)(uint64_t input,uint64_t state,uint64_t movement)=nullptr;   // PvP Bot (game_pvp_bot.hpp)
inline void(*volatile sprintPathFinder)(uint64_t input,uint64_t state,uint64_t movement)=nullptr;   // Path Finder (game_movement_e.hpp)
inline void(*volatile sprintMoveC)(uint64_t input,uint64_t state,uint64_t flags,uint64_t movement)=nullptr;   // Elytra Fly, Null Movement (game_movement_c.hpp)
inline void(*volatile sprintInventory)(uint64_t input,uint64_t state,uint64_t movement)=nullptr;   // Inventory Move (game_movement_a.hpp)
inline bool(*volatile sprintAutoSprint)(uint64_t input,uint64_t movement,uint64_t intent,uint64_t* old)=nullptr;   // Auto Sprint (game_movement_a.hpp)
inline void(*volatile sprintAutoRestore)(uint64_t movement,uint64_t old)=nullptr;
inline void FreeCameraSprintDetour(uint64_t a,uint64_t b,uint64_t c,uint64_t d,uint64_t e,uint64_t f,uint64_t g,uint64_t input,uint64_t i,uint64_t j,uint64_t flags,uint64_t movement,uint64_t m,uint64_t n){
 FreeCameraSprintGuarded(input,movement,flags);if(auto u=sprintUtility)u(input,j);if(auto mv=sprintMovement)mv(input,j,flags);if(auto sc=sprintScript)sc(input,j,movement);if(auto pb=sprintPvpBot)pb(input,j,movement);if(auto pf=sprintPathFinder)pf(input,j,movement);if(auto mc=sprintMoveC)mc(input,j,flags,movement);
 if(auto iv=sprintInventory)iv(input,j,movement);   // Inventory Move runs before Auto Sprint
 uint64_t forcedOld=0;auto autoSprint=sprintAutoSprint;const bool forced=autoSprint&&autoSprint(input,movement,m,&forcedOld);
 // The game receives the address of a local copy of the 16 bytes at rdx (rdx itself if the read fails); rcx
 // is forwarded.
 struct alignas(16) Copy16{uint8_t bytes[16];};Copy16 copy{};const bool copied=b&&Read(b,&copy);
 freeCameraSprintOriginal(a,copied?(uint64_t)&copy:b,c,d,e,f,g,input,i,j,flags,movement,m,n);
 if(forced)if(auto restore=sprintAutoRestore)restore(movement,forcedOld);   // the game saw the forced vector; the old one is put back
}
// PacketSendEvent: shared hook (game_packet_hooks.hpp), subscriber FreeCameraPacketGuarded.

// 1: hooks installed; -1: refused (shared hook missing, unexpected bytes or MinHook failure). Attempted once.
inline int InstallFreeCameraHooks(void(*log)(const char*)){
 static int status=0;if(status)return status;status=-1;FreeCameraState().log=log;uintptr_t base{};uint32_t size{};
 if(!Supported(&base,&size)){if(log)log("Freecam: not the supported Minecraft image, no camera hook installed");return status;}
 // Shared hooks first: without them movement, the player model and hand hiding would be incomplete.
 if(InstallMapHooks(log)!=1||InstallWorldHook(log)!=1||InstallHandRenderHook(log)!=1||InstallSendHook(log)!=1){if(log)log("Freecam: a shared hook (level tick, render prep or held item) is unavailable; Freecam stays inactive");return status;}
 struct Hook{uint32_t rva;uint8_t bytes[16];void* detour;void** original;};
 const Hook hooks[]={
  {0x53b3600,{0x41,0x57,0x41,0x56,0x41,0x54,0x56,0x57,0x53,0x48,0x81,0xec,0xe8,0,0,0},(void*)FreeCameraSetupDetour,(void**)&freeCameraSetupOriginal},
  {0x6da57d0,{0x55,0x41,0x57,0x41,0x56,0x41,0x54,0x56,0x57,0x53,0x48,0x81,0xec,0xe0,0,0},(void*)FreeCameraTurnDetour,(void**)&freeCameraTurnOriginal},
  {0x537e7e0,{0x55,0x56,0x57,0x53,0x48,0x83,0xec,0x58,0x48,0x8d,0x6c,0x24,0x50,0x48,0xc7,0x45},(void*)FreeCameraCullDetour,(void**)&freeCameraCullOriginal},
  {0x65b3420,{0x55,0x56,0x57,0x48,0x83,0xec,0x40,0x48,0x8d,0x6c,0x24,0x40,0x48,0xc7,0x45,0xf8},(void*)FreeCameraModelDetour,(void**)&freeCameraModelOriginal},
  {0xad34000,{0x41,0x57,0x41,0x56,0x41,0x55,0x41,0x54,0x56,0x57,0x55,0x53,0x48,0x83,0xec,0x78},(void*)FreeCameraJumpDetour,(void**)&freeCameraJumpOriginal},
  {0xb342490,{0x41,0x57,0x41,0x56,0x41,0x55,0x41,0x54,0x56,0x57,0x55,0x53,0x4c,0x8b,0x9c,0x24},(void*)FreeCameraSprintDetour,(void**)&freeCameraSprintOriginal}};
 constexpr size_t total=sizeof(hooks)/sizeof(hooks[0]);
 for(const auto& hook:hooks){if((uint64_t)hook.rva+16>size){if(log)log("Freecam: hook target outside the game image");return status;}
  for(int i=0;i<16;++i){uint8_t byte{};if(!Read(base+hook.rva+i,&byte)||byte!=hook.bytes[i]){if(log)log("Freecam: unexpected game code; camera hooks not installed");return status;}}}
 size_t created=0;auto rollback=[&](const char* why){for(size_t i=0;i<created;++i)MH_RemoveHook((void*)(base+hooks[i].rva));if(log)log(why);return status;};
 for(const auto& hook:hooks){if(MH_CreateHook((void*)(base+hook.rva),hook.detour,hook.original)!=MH_OK)return rollback("Freecam: could not create camera hooks");++created;}
 for(size_t i=0;i<total;++i)if(MH_QueueEnableHook((void*)(base+hooks[i].rva))!=MH_OK)return rollback("Freecam: could not queue camera hooks");
 if(MH_ApplyQueued()!=MH_OK)return rollback("Freecam: could not enable camera hooks; feature remains inactive");
 levelTickListener=FreeCameraTickGuarded;
 sendFreeCamera=FreeCameraPacketGuarded;
 status=1;if(log)log("Freecam: camera hooks installed (prologues verified)");return status;
}
}
