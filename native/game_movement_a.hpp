// Movement modules, group A: Speed, Auto Sprint, Fast Stop, No Slow Down, Inventory Move.
// - Speed: two game hooks (RVA 0x6d218d0 and 0x6d22cf0, the movement-speed systems). The game function runs first, then
//   the local player's speed component (0x39cf8d64, 0x28 bytes per entity) gets the Speed setting written at +0xc,
//   overwriting the value the game just computed (nothing is saved or restored; the game recomputes it each tick).
//   Mode All always writes, Air only when OnGroundFlagComponent (0xc29078a0) is absent, Ground only when it is present.
// - Auto Sprint: SprintIntentEvent (hook RVA 0xb342490). Mode Input sets bit 0 of input+0x11 and stores 7 into the
//   intent int (arg 13) every tick. Mode Omni does the same only while one enabled direction (Forward bit 13,
//   Backwards 14, Left 15, Right 16 of the u32 at input+0x10) is held, and then also forces the 2-float movement vector
//   (arg 12) to (0, 1) ("straight ahead") for the duration of the game call: the old vector is put back after the game
//   function returns. When no wanted direction is held and the module itself had set sprint, bit 0 and the intent are
//   cleared once.
// - Fast Stop: LevelTick. With no movement key held (bits 13..16 of the MoveInput word), the horizontal velocity
//   (*(actor+0x240) +0x18 / +0x20) is zeroed, so the player stops at once. (The reference gates this on a state byte
//   that is never set, so its module does nothing; Nickel's version always applies.)
// - No Slow Down: Blocks zeroes the three floats of BlockMovementSlowdownMultiplierComponent (0xf6091f58, 12 bytes) every
//   LevelTick. The ItemUseSlowdown hook (RVA 0xce35400, void(EntityId*, WeakPtr<Item>&, Registry*)) is cancelled (the game
//   function is not called) when the entity is the local player and All is set, or Food is set and item vtable +0x98 says
//   food, or Chargeables is set, the item is not food and item vtable +0xa8 is true (or, as that is false for the bow on
//   this game version, the item is a bow, crossbow or trident).
// - Inventory Move: only while no external input data is present (the player is in a screen), the client instance
//   predicate (RVA 0x65b0a70) is false and the top screen name (ClientInstance vtable +0x798) is "inventory_screen". W,
//   A, S, D (by physical position: Z Q S D on AZERTY), Space and Left Ctrl are read from the keyboard; SprintIntent writes the key bits, the normalised move
//   vector (input+0x24 / +0x28 and the arg 12 vector), jump (bits 7, 26 with a rising-edge bit 25) and sprint (bit 8);
//   the outgoing PlayerAuthInput (id 0x90) gets the same keys (input bits 10..13, 3, 6, 0x3d, 0x3c, 4) and the vector at
//   +0x64 / +0x80. Leaving the screen resets the state.
#pragma once
#include "game_movement.hpp"
#include "game_movement3.hpp"
#include "game_movement_b.hpp"
#include "game_sticky_aim.hpp"
#include <MinHook.h>
#include <atomic>
#include <cmath>
#include <cstring>
#include <mutex>
#include <string>

namespace game {
// ---- Shared state (written by the menu thread, read by game threads) ----
struct MoveAShared{
 std::atomic<bool> speed{false};std::atomic<int> speedMode{0};std::atomic<float> speedValue{0.1f};            // mode 0 All, 1 Air, 2 Ground
 std::atomic<bool> autoSprint{false};std::atomic<int> sprintMode{0};std::atomic<bool> fwd{true},back{true},left{true},right{true};   // mode 0 Input, 1 Omni
 std::atomic<bool> fastStop{false};
 std::atomic<bool> noSlow{false},blocks{true},all{false},food{false},charge{false};
 std::atomic<bool> inventory{false};
 std::atomic<uint8_t> sprintState{0};     // Auto Sprint: sprint was set by the module
};
inline MoveAShared& MoveA(){static auto* s=new MoveAShared;return *s;}
inline bool MoveAAny(){auto& s=MoveA();return s.speed||s.autoSprint||s.fastStop||s.noSlow||s.inventory;}

// ---- Speed ----
inline void SpeedApplyWork(){
 auto& s=MoveA();if(!s.speed)return;const int mode=s.speedMode;Context c{};if(!Local(&c))return;
 const bool ground=LocalHas(c,0xc29078a0u);if(mode==2&&!ground)return;if(mode==1&&ground)return;
 uintptr_t storage{},element{};float old{};if(!Storage(c.registry,0x39cf8d64u,&storage)||!InStorage(storage,c.entity,0x28,&element)||!Read(element+0xc,&old))return;
 *reinterpret_cast<float*>(element+0xc)=s.speedValue;}
inline void SpeedApply(){static volatile long stopped_=0;if(!game::FeatureStopped(&stopped_))__try{SpeedApplyWork();}__except(game::FeatureFault("SpeedApply",&stopped_,GetExceptionInformation())){game::FeatureFlush();}}
using SpeedSystemFunction=uint64_t(*)(void*);
inline SpeedSystemFunction speedOriginalA=nullptr,speedOriginalB=nullptr;
inline uint64_t SpeedDetourA(void* self){const uint64_t result=speedOriginalA(self);SpeedApply();return result;}
inline uint64_t SpeedDetourB(void* self){const uint64_t result=speedOriginalB(self);SpeedApply();return result;}

// ---- No Slow Down: item use ----
inline bool NoSlowCancelWork(void* entityId,void* weakItem){
 auto& s=MoveA();if(!s.noSlow)return false;Context c{};if(!Local(&c)||!entityId||!weakItem)return false;
 int32_t mine{},target{};if(!Read(c.actor+0x18,&mine)||!Read(reinterpret_cast<uintptr_t>(entityId),&target)||mine!=target)return false;
 uint64_t control{};if(!Read(reinterpret_cast<uintptr_t>(weakItem),&control)||!control)return false;
 int32_t strong{};uint64_t item{};if(!Read(control+8,&strong)||!strong||!Read(control,&item)||!item)return false;
 const uint64_t isFood=Virtual0(c,item,0x98);if(!isFood)return false;const bool food=reinterpret_cast<uint8_t(*)(uint64_t)>(isFood)(item)!=0;
 if(s.all)return true;if(s.food&&food)return true;
 if(s.charge&&!food){const uint64_t chargeable=Virtual0(c,item,0xa8);if(chargeable&&reinterpret_cast<uint8_t(*)(uint64_t)>(chargeable)(item)!=0)return true;
  // vtable +0xa8 is false for the bow on this game version (it stayed slowed while charging): charged items by name too
  char name[64]{};if(Text(item+0xd8,name,sizeof(name))&&(!strcmp(name,"bow")||!strcmp(name,"crossbow")||!strcmp(name,"trident")))return true;}
 return false;}
inline bool NoSlowCancel(void* entityId,void* weakItem){__try{return NoSlowCancelWork(entityId,weakItem);}__except(EXCEPTION_EXECUTE_HANDLER){return false;}}
using ItemUseSlowdownFunction=void(*)(void*,void*,void*);
inline ItemUseSlowdownFunction itemUseOriginal=nullptr;
inline void ItemUseSlowdownDetour(void* entityId,void* weakItem,void* registry){if(NoSlowCancel(entityId,weakItem))return;itemUseOriginal(entityId,weakItem,registry);}

// ---- LevelTick: No Slow Down (Blocks) and Fast Stop ----
inline void MoveATickWork(){
 auto& s=MoveA();const bool blocks=s.noSlow&&s.blocks,stop=s.fastStop;if(!blocks&&!stop)return;Context c{};if(!Local(&c))return;
 if(blocks){uintptr_t storage{},element{};if(Storage(c.registry,0xf6091f58u,&storage)&&InStorage(storage,c.entity,12,&element)){*reinterpret_cast<uint64_t*>(element)=0;*reinterpret_cast<uint32_t*>(element+8)=0;}}
 if(stop){const uintptr_t in=Move3Input(c);uint32_t bits{};uint64_t sv{};
  if(in&&Read(in+0x10,&bits)){const bool moving=(bits&(0xfu<<13))!=0;
   if(!moving&&Read(c.actor+0x240,&sv)&&sv){*reinterpret_cast<float*>(sv+0x18)=0;*reinterpret_cast<float*>(sv+0x20)=0;}}}}
inline void MoveATick(){static volatile long stopped_=0;if(!game::FeatureStopped(&stopped_))__try{MoveATickWork();}__except(game::FeatureFault("MoveATick",&stopped_,GetExceptionInformation())){game::FeatureFlush();}}

// ---- Auto Sprint (SprintIntent) ----
inline void MoveASetAutoSprint(bool on){auto& s=MoveA();if(s.autoSprint.exchange(on)!=on)s.sprintState=0;}   // enable and disable both reset the state
// Called before the game's SprintIntent function. true: the movement vector was replaced, *old holds the previous 8 bytes
// and MoveAAutoSprintRestore must be called after the game function.
inline bool AutoSprintWork(uint64_t input,uint64_t vector,uint64_t intent,uint64_t* old){
 auto& s=MoveA();if(!s.autoSprint||!input||!vector||!intent)return false;
 if(s.sprintMode==0){*reinterpret_cast<uint8_t*>(input+0x11)|=1;*reinterpret_cast<int32_t*>(intent)=7;return false;}
 uint32_t bits{};if(!Read(input+0x10,&bits))return false;
 const bool wanted=(s.fwd&&((bits>>13)&1))||(s.back&&((bits>>14)&1))||(s.left&&((bits>>15)&1))||(s.right&&((bits>>16)&1));
 if(!wanted){if(s.sprintState==1){*reinterpret_cast<uint8_t*>(input+0x11)&=0xfe;*reinterpret_cast<int32_t*>(intent)=0;s.sprintState=0;}return false;}
 uint64_t previous{};if(!Read(vector,&previous))return false;
 *reinterpret_cast<uint8_t*>(input+0x11)|=1;*reinterpret_cast<int32_t*>(intent)=7;s.sprintState=1;
 *old=previous;*reinterpret_cast<uint64_t*>(vector)=0x3f80000000000000ull;   // x = 0.0, z = 1.0
 return true;}
inline bool MoveAAutoSprint(uint64_t input,uint64_t vector,uint64_t intent,uint64_t* old){__try{return AutoSprintWork(input,vector,intent,old);}__except(EXCEPTION_EXECUTE_HANDLER){return false;}}
inline void MoveAAutoSprintRestore(uint64_t vector,uint64_t old){static volatile long reported_=0;__try{if(vector)*reinterpret_cast<uint64_t*>(vector)=old;}__except(game::FeatureFault("MoveAAutoSprintRestore",&reported_,GetExceptionInformation())){game::FeatureFlush();}}

// ---- Inventory Move ----
struct InventoryMoveState{float x=0,z=0;bool w=false,s=false,a=false,d=false,jump=false,sprint=false,valid=false,front=false,previous=false;};
struct InventoryMoveShared{std::mutex guard;InventoryMoveState state;};
inline InventoryMoveShared& InventoryMove(){static auto* s=new InventoryMoveShared;return *s;}
inline void MoveASetInventory(bool on){auto& s=MoveA();if(s.inventory.exchange(on)!=on){auto& m=InventoryMove();std::lock_guard<std::mutex> l(m.guard);m.state=InventoryMoveState{};}}
// The keys are only read while the game window has the focus (the game receives them from window messages).
// Keys are read by physical position (scan code), so W A S D on QWERTY and Z Q S D on AZERTY both work, like the
// game's default bindings.
inline bool InventoryKey(int vk){return (GetAsyncKeyState(vk)&0x8000)!=0;}
inline bool InventoryScan(UINT scan){const UINT vk=MapVirtualKeyW(scan,MAPVK_VSC_TO_VK);return vk&&(GetAsyncKeyState((int)vk)&0x8000)!=0;}
inline bool InventoryFocused(){DWORD pid{};const HWND hwnd=GetForegroundWindow();return hwnd&&GetWindowThreadProcessId(hwnd,&pid)&&pid==GetCurrentProcessId();}
// Name of the top screen (ClientInstance vtable +0x798 fills a std::string). When the name is not stored inline its buffer
// comes from the game's allocator (MallocRedirect), so it is released there: freeing it on this module's heap corrupted
// the game heap ("inventory_screen" is 16 characters, so never inline).
inline bool InventoryScreenIs(const Context& c,const char* wanted){
 const uint64_t get=Virtual0(c,c.client,0x798);if(!get)return false;alignas(8) uint8_t text[32]{};
 reinterpret_cast<void*(*)(uint64_t,void*)>(get)(c.client,text);char name[32]{};const bool ok=Text(reinterpret_cast<uintptr_t>(text),name,sizeof(name));
 uint64_t capacity{};memcpy(&capacity,text+0x18,8);if(capacity>15){uint64_t heap{};memcpy(&heap,text,8);if(heap)MoveBFree(heap,capacity+1);}   // the game's heap, not ours
 return ok&&strcmp(name,wanted)==0;}
inline bool InventoryClientPredicate(const Context& c){return reinterpret_cast<uint8_t(*)(uint64_t)>(c.base+0x65b0a70)(c.client)!=0;}
inline void InventoryReset(){auto& m=InventoryMove();std::lock_guard<std::mutex> l(m.guard);m.state=InventoryMoveState{};}
inline void InventorySprintWork(uint64_t input,uint64_t state,uint64_t vector){
 if(!MoveA().inventory)return;Context c{};uint64_t mine{};if(!Local(&c)||!Read(c.actor+0x240,&mine)||mine!=state||!input||!vector)return;
 if(StickyHasInput(c)||InventoryClientPredicate(c)||!InventoryScreenIs(c,"inventory_screen")||!InventoryFocused()){InventoryReset();return;}
 const bool w=InventoryScan(0x11),s=InventoryScan(0x1f),a=InventoryScan(0x1e),d=InventoryScan(0x20),jump=InventoryKey(0x20),sprint=InventoryKey(0xA2);
 const float sx=(float)a-(float)d,sz=(float)w-(float)s;float x=0,z=0;
 if(sx!=0.f||sz!=0.f){const float scale=1.0f/std::sqrt(sx*sx+sz*sz);z=sz*scale;x=sx*scale;}
 auto& m=InventoryMove();std::lock_guard<std::mutex> l(m.guard);auto& st=m.state;const bool front=jump&&!st.previous;
 uint32_t bits{};if(!Read(input,&bits))return;
 bits=(bits&0xf9fe1e7fu)|((uint32_t)w<<13)|((uint32_t)s<<14)|((uint32_t)a<<15)|((uint32_t)d<<16)|((uint32_t)jump<<7)|((uint32_t)sprint<<8)|((uint32_t)jump<<26)|((uint32_t)front<<25);
 *reinterpret_cast<uint32_t*>(input)=bits;*reinterpret_cast<float*>(input+0x24)=x;*reinterpret_cast<float*>(input+0x28)=z;
 uint8_t flags{};Read(input+0x60,&flags);*reinterpret_cast<uint8_t*>(input+0x60)=(uint8_t)((flags&0xed)|((uint8_t)sprint<<1)|((uint8_t)jump<<4));
 *reinterpret_cast<float*>(vector)=x;*reinterpret_cast<float*>(vector+4)=z;
 st.x=x;st.z=z;st.w=w;st.s=s;st.a=a;st.d=d;st.jump=jump;st.sprint=sprint;st.valid=true;st.front=front;st.previous=jump;}
inline void InventorySprint(uint64_t input,uint64_t state,uint64_t vector){static volatile long stopped_=0;if(!game::FeatureStopped(&stopped_))__try{InventorySprintWork(input,state,vector);}__except(game::FeatureFault("InventorySprint",&stopped_,GetExceptionInformation())){game::FeatureFlush();}}
inline void InventoryBit(uintptr_t packet,int bit,bool on){if(bit<0||bit>=0x42)return;uint64_t* word=reinterpret_cast<uint64_t*>(packet+0x88)+(bit>>6);const uint64_t mask=1ull<<(bit&63);if(on)*word|=mask;else *word&=~mask;}
inline void InventorySendWork(uintptr_t packet){
 if(!MoveA().inventory||PacketIdOf(packet)!=0x90)return;auto& m=InventoryMove();std::lock_guard<std::mutex> l(m.guard);if(!m.state.valid)return;
 Context c{};if(Local(&c)&&!StickyHasInput(c)){const auto& st=m.state;
  InventoryBit(packet,10,st.w);InventoryBit(packet,11,st.s);InventoryBit(packet,12,st.a);InventoryBit(packet,13,st.d);
  InventoryBit(packet,3,st.jump);InventoryBit(packet,6,st.jump);InventoryBit(packet,0x3d,st.jump);InventoryBit(packet,0x3c,st.front);InventoryBit(packet,4,st.sprint);
  *reinterpret_cast<float*>(packet+0x64)=st.x;*reinterpret_cast<float*>(packet+0x68)=st.z;*reinterpret_cast<float*>(packet+0x80)=st.x;*reinterpret_cast<float*>(packet+0x84)=st.z;return;}
 m.state=InventoryMoveState{};}
inline void InventorySend(uintptr_t packet){static volatile long stopped_=0;if(!game::FeatureStopped(&stopped_))__try{InventorySendWork(packet);}__except(game::FeatureFault("InventorySend",&stopped_,GetExceptionInformation())){game::FeatureFlush();}}
// Send dispatcher replacing MoveSendAll in game::sendMovement (the packet cannot be cancelled by Inventory Move).
inline bool MoveASendAll(uintptr_t packet){InventorySend(packet);return MoveSendAll(packet);}

// ---- Hook installation (prologues verified, rollback on failure; attempted once) ----
// Prologue check with a wildcard over the rip-relative displacement of the stack cookie load.
inline bool MoveACheck(uintptr_t base,uint32_t rva,const uint8_t* bytes,const uint8_t* mask,int count){for(int i=0;i<count;++i){uint8_t b{};if(!Read(base+rva+i,&b))return false;if(mask[i]&&b!=bytes[i])return false;}return true;}
// 1 installed, -1 refused.
inline int InstallSpeedHooks(void(*log)(const char*)){
 static int status=0;if(status)return status;status=-1;uintptr_t base{};uint32_t size{};if(!Supported(&base,&size)){if(log)log("Speed: not the supported Minecraft image, no hook installed");return status;}
 // push rsi; sub rsp,50h; mov rax,[rip+disp32] (disp wildcard); xor rax,rsp; mov [rsp+48h],rax; cmp byte [rcx+0B0h],0
 const uint8_t bytes[20]={0x56,0x48,0x83,0xec,0x50,0x48,0x8b,0x05,0,0,0,0,0x48,0x31,0xe0,0x48,0x89,0x44,0x24,0x48};
 const uint8_t mask[20]={1,1,1,1,1,1,1,1,0,0,0,0,1,1,1,1,1,1,1,1};
 struct Hook{uint32_t rva;void* detour;void** original;};const Hook hooks[]={{0x6d218d0,(void*)SpeedDetourA,(void**)&speedOriginalA},{0x6d22cf0,(void*)SpeedDetourB,(void**)&speedOriginalB}};
 for(const auto& h:hooks)if((uint64_t)h.rva+20>size||!MoveACheck(base,h.rva,bytes,mask,20)){if(log)log("Speed: unexpected bytes at the movement speed systems, no hook installed");return status;}
 size_t created=0;auto rollback=[&](const char* why){for(size_t i=0;i<created;++i)MH_RemoveHook((void*)(base+hooks[i].rva));if(log)log(why);return status;};
 for(const auto& h:hooks){if(MH_CreateHook((void*)(base+h.rva),h.detour,h.original)!=MH_OK)return rollback("Speed: could not create hooks");++created;}
 for(const auto& h:hooks)if(MH_QueueEnableHook((void*)(base+h.rva))!=MH_OK)return rollback("Speed: could not queue hooks");
 if(MH_ApplyQueued()!=MH_OK)return rollback("Speed: could not enable hooks");
 status=1;if(log)log("Speed: movement speed hooks installed (prologues verified)");return status;}
inline int InstallItemUseSlowdownHook(void(*log)(const char*)){
 static int status=0;if(status)return status;status=-1;uintptr_t base{};uint32_t size{};if(!Supported(&base,&size)){if(log)log("No Slow Down: not the supported Minecraft image, no hook installed");return status;}
 const uint8_t bytes[16]={0x56,0x57,0x53,0x48,0x83,0xec,0x40,0x0f,0x29,0x74,0x24,0x30,0x4c,0x89,0xc6,0x48};const uint8_t mask[16]={1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1};
 const uint32_t rva=0xce35400;if((uint64_t)rva+16>size||!MoveACheck(base,rva,bytes,mask,16)){if(log)log("No Slow Down: unexpected bytes at the item use slowdown function, no hook installed");return status;}
 if(MH_CreateHook((void*)(base+rva),(void*)ItemUseSlowdownDetour,(void**)&itemUseOriginal)!=MH_OK){if(log)log("No Slow Down: could not create the hook");return status;}
 if(MH_EnableHook((void*)(base+rva))!=MH_OK){MH_RemoveHook((void*)(base+rva));if(log)log("No Slow Down: could not enable the hook");return status;}
 status=1;if(log)log("No Slow Down: item use slowdown hook installed (prologue verified)");return status;}
}
