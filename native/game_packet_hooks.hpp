// Shared packet send hook (PacketSendEvent #214, game RVA 0x2360ff0), installed once with MinHook.
// Freecam rewrites PlayerAuthInputPacket here; Utility, Movement and Combat modules can inspect or cancel
// the send.
#pragma once
#include "game_read.hpp"
#include <MinHook.h>
#include <array>
#include <atomic>
#include <cstdio>
#include <utility>
#include "generated_receive_hooks.hpp"

namespace game {
inline void(*volatile sendFreeCamera)(uintptr_t packet)=nullptr;
inline bool(*volatile sendUtility)(uintptr_t packet)=nullptr;   // true = packet not sent
inline bool(*volatile sendMovement)(uintptr_t packet)=nullptr;   // No Fall (game_movement2.hpp)
inline bool(*volatile sendBacktrack)(uintptr_t packet)=nullptr;   // Backtrack: true = packet not sent
inline void(*volatile sendPlayer)(uintptr_t packet)=nullptr;   // Chest Stealer / Chest Filler interaction tracking (game_player_b.hpp)
inline void(*volatile sendCombat)(uintptr_t packet)=nullptr;   // Kill Aura Teleport (game_combat.hpp)
using GameSendFunction=void(*)(void*,void*);inline GameSendFunction gameSendOriginal=nullptr;
inline std::atomic<int> sendDepth{0};   // sends in progress; above 1 inside a callback = a nested send
// Every subscriber runs behind its own fault boundary (game_fault.hpp): a faulty one is logged once and skipped, the
// packet still reaches the game unless a working subscriber cancels it.
inline bool GameSendCancelled(uintptr_t p){static volatile long stopped[7]{};
 GuardVoid("Packet send: Freecam",&stopped[0],sendFreeCamera,p);GuardVoid("Packet send: Chest Stealer",&stopped[1],sendPlayer,p);
 GuardVoid("Packet send: silent rotation",&stopped[2],rotationSilent,p);GuardVoid("Packet send: Kill Aura",&stopped[3],sendCombat,p);
 return GuardBool("Packet send: movement",&stopped[4],sendMovement,p)||GuardBool("Packet send: Backtrack",&stopped[5],sendBacktrack,p)||
  GuardBool("Packet send: utility",&stopped[6],sendUtility,p);}
inline void GameSendDispatch(void* self,void* packet){if(GameSendCancelled((uintptr_t)packet))return;gameSendOriginal(self,packet);}
struct SendDepthScope{SendDepthScope(){++sendDepth;}~SendDepthScope(){--sendDepth;}};   // balanced on every exit
inline void GameSendDetour(void* self,void* packet){SendDepthScope depth;GameSendDispatch(self,packet);}
inline int InstallSendHook(void(*log)(const char*)){static int state=0;if(state)return state;state=-1;uintptr_t base{};uint32_t size{};if(!Supported(&base,&size))return state;
 const uint8_t bytes[16]={0x41,0x57,0x41,0x56,0x41,0x54,0x56,0x57,0x55,0x53,0x48,0x81,0xec,0x90,1,0};
 if(!CodeIs(base+Rva(0x2360ff0),bytes,16)){if(log)log("Packets: unexpected code at the send hook");return state;}
 if(MH_CreateHook((void*)(base+Rva(0x2360ff0)),(void*)GameSendDetour,(void**)&gameSendOriginal)!=MH_OK){if(log)log("Packets: send hook not installed");return state;}
 if(MH_EnableHook((void*)(base+Rva(0x2360ff0)))!=MH_OK){MH_RemoveHook((void*)(base+Rva(0x2360ff0)));if(log)log("Packets: send hook not installed (could not be enabled, removed)");return state;}
 if(log)log("Packets: send hook installed");state=1;return state;}

// ---- PacketReceiveEvent: 177 per-type dispatchers (generated_receive_hooks.hpp); the packet is **(ev+0x60)
// (r9 = shared_ptr<Packet>&). Three targets (ActorEvent 0x23a7320, StartGame 0x23a6fb0, LevelEvent
// 0x23a7260) belong to game_events.hpp, whose detours also call ReceiveFilter.
inline bool(*volatile receiveUtility)(uintptr_t packet)=nullptr;   // true = packet not handled
inline void(*volatile receiveCombat)(uintptr_t packet)=nullptr;   // Combat: may edit an incoming packet in place (Misplace)
inline bool(*volatile receiveLag)(void* shared)=nullptr;   // Blink / Fake Lag: true = packet held
inline bool(*volatile receiveBacktrack)(void* shared)=nullptr;   // Backtrack: true = packet held / not handled now
inline bool(*volatile receiveMovementD)(void* shared)=nullptr;   // Knockback Desync / Auto Jump Reset (game_movement_d.hpp): true = packet held
inline bool ReceiveFilter(void* shared){uint64_t packet{};if(!shared||!Read((uintptr_t)shared,&packet)||!packet)return false;static volatile long stopped[5]{};
 GuardVoid("Packet receive: Misplace",&stopped[0],receiveCombat,packet);
 return GuardBool("Packet receive: Knockback Desync",&stopped[1],receiveMovementD,shared)||GuardBool("Packet receive: Blink / Fake Lag",&stopped[2],receiveLag,shared)||
  GuardBool("Packet receive: Backtrack",&stopped[3],receiveBacktrack,shared)||GuardBool("Packet receive: utility",&stopped[4],receiveUtility,packet);}
using ReceiveFunction=void(*)(void*,void*,void*,void*);
inline ReceiveFunction receiveOriginal[kReceiveCount]{};
template<int I>inline void ReceiveDetour(void* a,void* b,void* c,void* d){if(ReceiveFilter(d))return;receiveOriginal[I](a,b,c,d);}
template<int... I>inline std::array<void*,sizeof...(I)> ReceiveDetours(std::integer_sequence<int,I...>){return {(void*)&ReceiveDetour<I>...};}
inline bool ReceiveOwnedElsewhere(uint32_t rva){return rva==0x23a7320||rva==0x23a6fb0||rva==0x23a7260;}
inline int InstallReceiveHooks(void(*log)(const char*)){static int state=0;if(state)return state;state=-1;uintptr_t base{};uint32_t size{};if(!Supported(&base,&size))return state;
 static const auto detours=ReceiveDetours(std::make_integer_sequence<int,kReceiveCount>{});const uint8_t head[12]={0x4c,0x89,0xc1,0x4d,0x8b,0x01,0x48,0x8b,0x01,0x48,0x8b,0x80};
 int created=0,skipped=0;for(int i=0;i<kReceiveCount;++i){const uint32_t rva=kReceiveTargets[i];if(ReceiveOwnedElsewhere(rva))continue;bool ok=rva+16<size;
  {bool first=ok,second=ok;const uint8_t alt[7]={0x48,0x83,0xec,0x38,0x4c,0x89,0xc1};   // 8 dispatchers have a stack frame (sub rsp,0x38 ; mov rcx,r8)
   first=first&&CodeIs(base+Rva(rva),head,12);second=second&&CodeIs(base+Rva(rva),alt,7);ok=first||second;}
  if(!ok||MH_CreateHook((void*)(base+Rva(rva)),detours[i],(void**)&receiveOriginal[i])!=MH_OK){++skipped;continue;}
  if(MH_QueueEnableHook((void*)(base+Rva(rva)))!=MH_OK){MH_RemoveHook((void*)(base+Rva(rva)));receiveOriginal[i]=nullptr;++skipped;continue;}++created;}
 if(MH_ApplyQueued()!=MH_OK){   // some may be live: disable them all, keep the trampolines (a thread may be inside a detour)
  for(int i=0;i<kReceiveCount;++i)if(receiveOriginal[i])MH_QueueDisableHook((void*)(base+Rva(kReceiveTargets[i])));MH_ApplyQueued();
  if(log)log("Packets: receive hooks could not be enabled, none left active");return state;}
 if(log){char line[128];snprintf(line,sizeof(line),"Packets: %d receive hooks installed, %d skipped",created,skipped);log(line);}state=1;return state;}
}
