// Free Look: native adapter for the nickel::visual core (include/nickel/visual_free_look.hpp).
// While active, the camera orbits freely and the player's rotation stays fixed: on UpdatePlayerFromCamera (game RVA
// 0x6f31ff0) the saved pose is written back and the game function is skipped, only on the GameUpdate thread and only
// for the local player. Enable/disable transitions also run on the GameUpdate thread (hook #30).
#pragma once
#include "game_free_camera.hpp"
#include "../include/nickel/visual_free_look.hpp"
#include <mutex>

namespace game {
struct FreeLookShared {
 std::mutex guard;bool wanted=false,keep=false;int mode=0;                       // written by the menu
 volatile long gameThread=0,fault=0,seen=0;void(*log)(const char*)=nullptr;
 // Game thread only (StateComponent<FreeLook::State> 0x950501cf).
 bool active=false,perspectiveSaved=false;int savedPerspective=0;uintptr_t actor=0;nickel::visual::LookRotation saved{};
};
inline FreeLookShared& FreeLookState(){static auto* state=new FreeLookShared;return *state;}
inline void FreeLookFault(){auto& shared=FreeLookState();if(InterlockedExchange(&shared.fault,1)==0&&shared.log)shared.log("Free Look: a guarded game access failed; Free Look disabled until restart");}
inline bool FreeLookOnGameThread(){auto& shared=FreeLookState();return shared.gameThread!=0&&(DWORD)shared.gameThread==GetCurrentThreadId();}
inline void ConfigureFreeLook(bool wanted,int mode,bool keep){auto& shared=FreeLookState();std::lock_guard<std::mutex> lock(shared.guard);shared.wanted=wanted&&!shared.fault;shared.mode=mode;shared.keep=keep;}

// Reads rotation: r = *(actor+0x250) {pitch, yaw}; head = ActorHeadRotationComponent (0xbabe7211) [0], else yaw.
inline bool FreeLookReadRotation(const Context& context,nickel::visual::LookRotation* out){
 uint64_t rotation{};float pair[2];if(!Read(context.actor+0x250,&rotation)||!rotation||!Floats(rotation,pair,2))return false;
 uintptr_t head{};float headYaw{};const bool hasHead=Element(context.registry,context.entity,"ActorHeadRotationComponent",8,&head)&&Floats(head,&headYaw,1);
 out->pitch=pair[0];out->yaw=pair[1];out->headYaw=hasHead?headYaw:pair[1];return true;
}
// Writes rotation: *(actor+0x250) = {pitch, yaw}; head: both floats = headYaw. Re-read before writing.
inline bool FreeLookWriteRotation(const Context& context,const nickel::visual::LookRotation& pose){
 uint64_t rotation{};float old[2];if(!Read(context.actor+0x250,&rotation)||!rotation||!Floats(rotation,old,2))return false;
 const float pair[2]={pose.pitch,pose.yaw};memcpy((void*)rotation,pair,8);
 uintptr_t head{};float oldHead[2];if(Element(context.registry,context.entity,"ActorHeadRotationComponent",8,&head)&&Floats(head,oldHead,2)){const float both[2]={pose.headYaw,pose.headYaw};memcpy((void*)head,both,8);}
 return true;
}
// Perspective: options = client->vtable[0x560](client); option #3; setter at game RVA 0x5b6fe70(options, value).
inline bool FreeLookPerspective(int* current,uint64_t* options){Context context{};if(!Client(&context))return false;*options=GameOptions(context);uint64_t option{};int32_t value{};
 if(!*options||!Read(*options+0x10+8ull*3,&option)||!option||!Read(option+0x18,&value))return false;*current=value;return true;}
inline bool FreeLookSetPerspective(uint64_t options,int value){
 static int checked=0;uintptr_t base{};uint32_t size{};if(!Supported(&base,&size))return false;
 if(!checked){checked=-1;const uint8_t bytes[16]={0x56,0x57,0x48,0x83,0xec,0x48,0x89,0xd6,0x48,0x8b,0x81,0x40,0x1b,0x00,0x00,0x48};bool same=true;for(int i=0;i<16&&same;++i){uint8_t byte{};same=Read(base+Rva(0x5b6fe70)+i,&byte)&&byte==bytes[i];}if(same)checked=1;}
 if(checked!=1)return false;reinterpret_cast<void(*)(uint64_t,int)>(base+Rva(0x5b6fe70))(options,value);return true;
}
// Camera angles: last entity of the camera view (same storages as Freecam); option 3 == 2 -> inverted vector.
inline bool FreeLookCameraAngles(const Context& context,float* pitch,float* yaw){
 uintptr_t cameras[64]{};const int count=FreeCameraComponents(context,cameras,64);if(!count)return false;std::array<float,4> q{};if(!Floats(cameras[count-1]+0x30,q.data(),4))return false;
 int perspective{};uint64_t options{};if(!FreeLookPerspective(&perspective,&options))return false;return nickel::visual::free_look_camera_angles(q,perspective==2,pitch,yaw);
}

// ---- Transitions, on the GameUpdate thread
inline void FreeLookEnable(FreeLookShared& shared,int mode){
 Context context{};if(Local(&context)){nickel::visual::LookRotation pose{};if(FreeLookReadRotation(context,&pose)){shared.saved=pose;shared.actor=context.actor;shared.active=true;}}
 int current{};uint64_t options{};if(!FreeLookPerspective(&current,&options))return;const int target=nickel::visual::free_look_perspective_target(mode,current);
 if(target>=0){shared.savedPerspective=current;shared.perspectiveSaved=true;FreeLookSetPerspective(options,target);}
}
inline void FreeLookDisable(FreeLookShared& shared,bool keep){
 Context context{};const bool local=Local(&context)&&context.actor==shared.actor;
 if(local&&shared.active&&!keep){float pitch{},yaw{};if(FreeLookCameraAngles(context,&pitch,&yaw)){uintptr_t base{};uint32_t size{};
  // Game RVA 0x6da57d0: if Freecam hooks it, the call goes through its detour.
  if(Supported(&base,&size)){float turn[2];const auto value=nickel::visual::free_look_release_turn(shared.saved,pitch,yaw);turn[0]=value[0];turn[1]=value[1];reinterpret_cast<void(*)(uint64_t,float*)>(base+Rva(0x6da57d0))(context.actor,turn);}}}
 if(shared.perspectiveSaved){int current{};uint64_t options{};if(FreeLookPerspective(&current,&options))FreeLookSetPerspective(options,shared.savedPerspective);}
 if(local&&shared.active)FreeLookWriteRotation(context,shared.saved);
 shared.active=false;shared.perspectiveSaved=false;shared.actor=0;
}
inline void FreeLookUpdateWork(){
 auto& shared=FreeLookState();bool wanted{},keep{};int mode{};{std::lock_guard<std::mutex> lock(shared.guard);wanted=shared.wanted;keep=shared.keep;mode=shared.mode;}
 if(shared.fault)return;
 // Follows the module state: `active` is set on enable (if a player exists); disable restores the pose.
 static bool enabled=false;if(wanted&&!enabled){enabled=true;FreeLookEnable(shared,mode);}else if(!wanted&&enabled){enabled=false;FreeLookDisable(shared,keep);}
}
inline void FreeLookUpdateGuarded(){__try{FreeLookUpdateWork();}__except(EXCEPTION_EXECUTE_HANDLER){FreeLookFault();}}

// ---- UpdatePlayerFromCameraEvent (hook #20, game RVA 0x6f31ff0; r8 = actor): true = skip the game call.
inline bool FreeLookCameraWork(uintptr_t actor){
 auto& shared=FreeLookState();if(shared.fault||!shared.active||!FreeLookOnGameThread())return false;   // game thread only
 Context context{};if(!Local(&context)||context.actor!=actor)return false;
 if(context.actor!=shared.actor){shared.active=false;return false;}                                    // local actor changed: request dropped
 nickel::visual::LookRotation current{},pose{};if(!FreeLookReadRotation(context,&current)||!nickel::visual::free_look_apply(shared.saved,current.pitch,current.yaw,&pose))return false;
 if(!FreeLookWriteRotation(context,pose))return false;InterlockedExchange(&shared.seen,1);return true;
}
inline bool FreeLookCameraGuarded(uintptr_t actor){__try{return FreeLookCameraWork(actor);}__except(EXCEPTION_EXECUTE_HANDLER){FreeLookFault();return false;}}

// GameUpdate #30 (MinecraftGame::update, rcx only): the event fires BEFORE the game function runs.
using FreeLookGameUpdateFunction=void(*)(void*);inline FreeLookGameUpdateFunction freeLookGameUpdateOriginal=nullptr;
inline void FreeLookGameUpdateDetour(void* game){
 auto& shared=FreeLookState();InterlockedExchange(&shared.gameThread,(long)GetCurrentThreadId());FreeLookUpdateGuarded();{auto rp=rotationPush;if(rp)rp(0);if(auto g=rotationGameUpdate)g();if(auto cg=combatGameUpdate)cg();if(auto dg=discordGameUpdate)dg();if(rp)if(auto pop=rotationPop)pop();}if(auto l=lagGameUpdate)l();freeLookGameUpdateOriginal(game);
}
inline void(*volatile combatCamera)()=nullptr;   // Combat: UpdatePlayerFromCamera subscribers (Auto Mace)
using FreeLookCameraFunction=void(*)(void*,void*,void*);inline FreeLookCameraFunction freeLookCameraOriginal=nullptr;
inline void FreeLookCameraDetour(void* a,void* b,void* actor){if(FreeLookCameraGuarded((uintptr_t)actor))return;
 if(auto rp=rotationPush){rp(3);if(auto cc=combatCamera)cc();if(auto pop=rotationPop)pop();if(auto cam=rotationCamera)if(cam((uintptr_t)actor))return;}   // push the camera event and apply the rotation request
 freeLookCameraOriginal(a,b,actor);}

// 1: hooks installed; -1: refused. Single attempt, full rollback on failure.
inline int InstallFreeLookHooks(void(*log)(const char*)){
 static int status=0;if(status)return status;status=-1;FreeLookState().log=log;uintptr_t base{};uint32_t size{};
 if(!Supported(&base,&size)){if(log)log("Free Look: not the supported Minecraft image, no hook installed");return status;}
 struct Hook{uint32_t rva;uint8_t bytes[16];void* detour;void** original;};
 const Hook hooks[]={
  {0x879010,{0x55,0x41,0x56,0x56,0x57,0x53,0x48,0x81,0xec,0xc0,0x00,0x00,0x00,0x48,0x8d,0xac},(void*)FreeLookGameUpdateDetour,(void**)&freeLookGameUpdateOriginal},
  {0x6f31ff0,{0x41,0x57,0x41,0x56,0x41,0x55,0x41,0x54,0x56,0x57,0x55,0x53,0x48,0x81,0xec,0xd8},(void*)FreeLookCameraDetour,(void**)&freeLookCameraOriginal}};
 for(const auto& hook:hooks){if((uint64_t)hook.rva+16>size)return status;if(!CodeIs(base+Rva(hook.rva),hook.bytes,16)){if(log)log("Free Look: unexpected game code; hooks not installed");return status;}}
 size_t created=0;auto rollback=[&](const char* why){for(size_t i=0;i<created;++i)MH_RemoveHook((void*)(base+Rva(hooks[i].rva)));if(log)log(why);return status;};
 for(const auto& hook:hooks){if(MH_CreateHook((void*)(base+Rva(hook.rva)),hook.detour,hook.original)!=MH_OK)return rollback("Free Look: could not create hooks");++created;}
 for(const auto& hook:hooks)if(MH_QueueEnableHook((void*)(base+Rva(hook.rva)))!=MH_OK)return rollback("Free Look: could not queue hooks");
 if(MH_ApplyQueued()!=MH_OK)return rollback("Free Look: could not enable hooks");
 status=1;if(log)log("Free Look: GameUpdate and UpdatePlayerFromCamera hooks installed (prologues verified)");return status;
}
}
