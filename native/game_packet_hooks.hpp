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
inline void GameSendDispatch(void* self,void* packet){if(auto f=sendFreeCamera)f((uintptr_t)packet);if(auto pp=sendPlayer)pp((uintptr_t)packet);if(auto r=rotationSilent)r((uintptr_t)packet);if(auto cb=sendCombat)cb((uintptr_t)packet);if(auto mv=sendMovement)if(mv((uintptr_t)packet))return;if(auto bt=sendBacktrack)if(bt((uintptr_t)packet))return;if(auto u=sendUtility)if(u((uintptr_t)packet))return;gameSendOriginal(self,packet);}
inline void GameSendDetour(void* self,void* packet){++sendDepth;GameSendDispatch(self,packet);--sendDepth;}
inline int InstallSendHook(void(*log)(const char*)){static int state=0;if(state)return state;state=-1;uintptr_t base{};uint32_t size{};if(!Supported(&base,&size))return state;
 const uint8_t bytes[16]={0x41,0x57,0x41,0x56,0x41,0x54,0x56,0x57,0x55,0x53,0x48,0x81,0xec,0x90,1,0};
 for(int i=0;i<16;++i){uint8_t b{};if(!Read(base+0x2360ff0+i,&b)||b!=bytes[i]){if(log)log("Packets: unexpected code at the send hook");return state;}}
 if(MH_CreateHook((void*)(base+0x2360ff0),(void*)GameSendDetour,(void**)&gameSendOriginal)!=MH_OK||MH_EnableHook((void*)(base+0x2360ff0))!=MH_OK){if(log)log("Packets: send hook not installed");return state;}
 if(log)log("Packets: send hook installed");state=1;return state;}

// ---- PacketReceiveEvent: 177 per-type dispatchers (generated_receive_hooks.hpp); the packet is **(ev+0x60)
// (r9 = shared_ptr<Packet>&). Three targets (ActorEvent 0x23a7320, StartGame 0x23a6fb0, LevelEvent
// 0x23a7260) belong to game_events.hpp, whose detours also call ReceiveFilter.
inline bool(*volatile receiveUtility)(uintptr_t packet)=nullptr;   // true = packet not handled
inline void(*volatile receiveCombat)(uintptr_t packet)=nullptr;   // Combat: may edit an incoming packet in place (Misplace)
inline bool(*volatile receiveLag)(void* shared)=nullptr;   // Blink / Fake Lag: true = packet held
inline bool(*volatile receiveBacktrack)(void* shared)=nullptr;   // Backtrack: true = packet held / not handled now
inline bool(*volatile receiveMovementD)(void* shared)=nullptr;   // Knockback Desync / Auto Jump Reset (game_movement_d.hpp): true = packet held
inline bool ReceiveFilter(void* shared){uint64_t packet{};if(!shared||!Read((uintptr_t)shared,&packet)||!packet)return false;if(auto c=receiveCombat)c(packet);if(auto md=receiveMovementD)if(md(shared))return true;if(auto l=receiveLag)if(l(shared))return true;if(auto b=receiveBacktrack)if(b(shared))return true;auto f=receiveUtility;return f?f(packet):false;}
using ReceiveFunction=void(*)(void*,void*,void*,void*);
inline ReceiveFunction receiveOriginal[kReceiveCount]{};
template<int I>inline void ReceiveDetour(void* a,void* b,void* c,void* d){if(ReceiveFilter(d))return;receiveOriginal[I](a,b,c,d);}
template<int... I>inline std::array<void*,sizeof...(I)> ReceiveDetours(std::integer_sequence<int,I...>){return {(void*)&ReceiveDetour<I>...};}
inline bool ReceiveOwnedElsewhere(uint32_t rva){return rva==0x23a7320||rva==0x23a6fb0||rva==0x23a7260;}
inline int InstallReceiveHooks(void(*log)(const char*)){static int state=0;if(state)return state;state=-1;uintptr_t base{};uint32_t size{};if(!Supported(&base,&size))return state;
 static const auto detours=ReceiveDetours(std::make_integer_sequence<int,kReceiveCount>{});const uint8_t head[12]={0x4c,0x89,0xc1,0x4d,0x8b,0x01,0x48,0x8b,0x01,0x48,0x8b,0x80};
 int created=0,skipped=0;for(int i=0;i<kReceiveCount;++i){const uint32_t rva=kReceiveTargets[i];if(ReceiveOwnedElsewhere(rva))continue;bool ok=rva+16<size;
  {bool first=ok,second=ok;const uint8_t alt[7]={0x48,0x83,0xec,0x38,0x4c,0x89,0xc1};   // 8 dispatchers have a stack frame (sub rsp,0x38 ; mov rcx,r8)
   for(int k=0;first&&k<12;++k){uint8_t b{};first=Read(base+rva+k,&b)&&b==head[k];}for(int k=0;second&&k<7;++k){uint8_t b{};second=Read(base+rva+k,&b)&&b==alt[k];}ok=first||second;}
  if(!ok||MH_CreateHook((void*)(base+rva),detours[i],(void**)&receiveOriginal[i])!=MH_OK){++skipped;continue;}MH_QueueEnableHook((void*)(base+rva));++created;}
 if(MH_ApplyQueued()!=MH_OK){if(log)log("Packets: receive hooks could not be enabled");return state;}
 if(log){char line[128];snprintf(line,sizeof(line),"Packets: %d receive hooks installed, %d skipped",created,skipped);log(line);}state=1;return state;}
}
