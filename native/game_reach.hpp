// Reach: attack reach drawn at random from the Reach range (on enable, after every attack and when the setting
// changes) and an optional larger block pick range.
// - Attack reach: a mid-function hook at RVA 0x4e0a4a7, right after the game loads the entity pick distance into xmm7.
//   When xmm7 still holds the default reach (the float at RVA 0xef3f020, 3.0), it is replaced by the drawn reach.
// - Block reach: GameMode::getPickRange (RVA 0x376f1a0) returns max(game value, Block Reach Distance).
#pragma once
#include "game_read.hpp"
#include <MinHook.h>
#include <atomic>
#include <cstring>
#include <mutex>
#include <random>

namespace game {
// Read by the mid-function stub: reach to use, the game's default reach, enabled flag.
struct alignas(16) ReachPatch { float reach = 3.0f; float fallback = 3.0f; int32_t enabled = 0; int32_t pad = 0; };
inline ReachPatch& ReachData() { static auto* data = new ReachPatch; return *data; }
struct ReachShared {
 std::mutex guard; bool enabled = false; float low = 3.0f, high = 3.3f; bool blockReach = false; float blockDistance = 6.7f;
 std::atomic<bool> reroll{ false };
};
inline ReachShared& Reach() { static auto* shared = new ReachShared; return *shared; }

// Uniform draw in [low, high], like the range settings.
inline void ReachDraw(ReachShared& s) {
 static std::mt19937 rng{ std::random_device{}() };
 const float value = s.low < s.high ? std::uniform_real_distribution<float>(s.low, s.high)(rng) : s.low;
 ReachData().reach = value;
}

// Builds the stub: compare xmm7 with the default reach, replace it when the module is on, replay the two overwritten
// instructions (mov rax,[rbx] ; mov rax,[rax+0xa78]) and jump back.
inline int InstallReachHook(void (*log)(const char*)) {
 static int state = 0; if (state) return state; state = -1;
 uintptr_t base{}; uint32_t size{}; if (!Supported(&base, &size)) return state;
 uint8_t* target = reinterpret_cast<uint8_t*>(base + Rva(0x4e0a4a7));
 const uint8_t expected[10] = { 0x48, 0x8b, 0x03, 0x48, 0x8b, 0x80, 0x78, 0x0a, 0x00, 0x00 };
 if (!CodeIs(reinterpret_cast<uintptr_t>(target), expected, 10)) { if (log) log("Reach: unexpected code at the pick distance, no hook installed"); return state; }
 float fallback{}; if (!Read(base + Rva(0xef3f020), &fallback)) return state;
 ReachData().fallback = fallback;
 // stub within +-2 GB so a rel32 jump reaches it
 uint8_t* stub = nullptr;
 for (uintptr_t at = (reinterpret_cast<uintptr_t>(target) & ~0xffffull) - 0x10000; at > reinterpret_cast<uintptr_t>(target) - 0x70000000 && !stub; at -= 0x10000)
  stub = static_cast<uint8_t*>(VirtualAlloc(reinterpret_cast<void*>(at), 0x1000, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE));
 if (!stub) { if (log) log("Reach: no memory near the game for the stub"); return state; }
 const uint64_t data = reinterpret_cast<uint64_t>(&ReachData());
 uint8_t code[64]; size_t n = 0;
 auto put = [&](std::initializer_list<uint8_t> bytes) { for (uint8_t b : bytes) code[n++] = b; };
 put({ 0x50 });                                                     // push rax
 put({ 0x48, 0xb8 }); std::memcpy(code + n, &data, 8); n += 8;      // mov rax, &ReachData
 put({ 0x83, 0x78, 0x08, 0x00 });                                   // cmp dword [rax+8], 0
 put({ 0x74, 0x0a });                                               // je skip
 put({ 0x0f, 0x2e, 0x78, 0x04 });                                   // ucomiss xmm7, [rax+4]
 put({ 0x75, 0x04 });                                               // jne skip
 put({ 0xf3, 0x0f, 0x10, 0x38 });                                   // movss xmm7, [rax]
 put({ 0x58 });                                                     // skip: pop rax
 for (int i = 0; i < 10; ++i) code[n++] = expected[i];              // replayed instructions
 const int32_t back = static_cast<int32_t>(reinterpret_cast<intptr_t>(target + 10) - reinterpret_cast<intptr_t>(stub + n + 5));
 put({ 0xe9 }); std::memcpy(code + n, &back, 4); n += 4;            // jmp target+10
 std::memcpy(stub, code, n); FlushInstructionCache(GetCurrentProcess(), stub, n);
 uint8_t patch[10]; patch[0] = 0xe9;
 const int32_t to = static_cast<int32_t>(reinterpret_cast<intptr_t>(stub) - reinterpret_cast<intptr_t>(target + 5));
 std::memcpy(patch + 1, &to, 4); for (int i = 5; i < 10; ++i) patch[i] = 0x90;
 DWORD old{}; if (!VirtualProtect(target, 10, PAGE_EXECUTE_READWRITE, &old)) return state;
 std::memcpy(target, patch, 10); VirtualProtect(target, 10, old, &old); FlushInstructionCache(GetCurrentProcess(), target, 10);
 if (log) log("Reach: pick distance hook installed");
 state = 1; return state;
}

using PickRangeFunction = float (*)(void*, const int*);
inline PickRangeFunction pickRangeOriginal = nullptr;
inline float PickRangeDetour(void* mode, const int* input) {
 const float range = pickRangeOriginal(mode, input);
 auto& s = Reach();
 if (!s.enabled || !s.blockReach) return range;
 return range <= s.blockDistance ? s.blockDistance : range;
}
inline int InstallPickRangeHook(void (*log)(const char*)) {
 static int state = 0; if (state) return state; state = -1;
 uintptr_t base{}; uint32_t size{}; if (!Supported(&base, &size)) return state;
 const uint8_t expected[8] = { 0x56, 0x57, 0x48, 0x83, 0xec, 0x28, 0x8b, 0x02 };
 if (!CodeIs(base + Rva(0x376f1a0), expected, 8)) { if (log) log("Reach: unexpected code at getPickRange, no hook installed"); return state; }
 void* target = reinterpret_cast<void*>(base + Rva(0x376f1a0));
 if (MH_CreateHook(target, reinterpret_cast<void*>(PickRangeDetour), reinterpret_cast<void**>(&pickRangeOriginal)) != MH_OK || MH_EnableHook(target) != MH_OK) { if (log) log("Reach: getPickRange hook not installed"); return state; }
 if (log) log("Reach: getPickRange hook installed");
 state = 1; return state;
}

// Called after an attack goes through: draws a new reach for the next hit.
inline void ReachAfterAttack() { auto& s = Reach(); if (s.enabled) s.reroll = true; }
}
