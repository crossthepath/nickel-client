// Timer (Utility): changes the game's tick rate.
// - Setting "Tick Rate" (20, 0..100, step 0.1, "TPS"); list suffix "x" = rate / 20.
// - Game timers: mc = *(client + 0x1d0); simulation timer *(mc + 0xe8), render timer *(mc + 0xf0);
// +0 = ticks per second (float); +8, +0x10, +0x14 copied from the first to the second on every change.
// - Enable stores the current rate; each tick stores it if not yet stored, then writes the chosen rate to both
// timers; disable restores the stored rate to both.
// Writes happen on the tick thread (disable takes effect on the next tick, or immediately when out of game).
#pragma once
#include "game_read.hpp"
#include <atomic>
#include <cstdio>

namespace game {
struct TimerShared{std::atomic<float> rate{20};std::atomic<bool> enabled{false};float saved=20;bool has=false;std::atomic<bool> fault{false};};
inline TimerShared& Timer(){static auto* s=new TimerShared;return *s;}
inline bool TimerClocks(uint64_t* simulation,uint64_t* render){Context c{};uint64_t mc{};return Client(&c)&&Read(c.client+0x1d0,&mc)&&mc&&Read(mc+0xe8,simulation)&&*simulation&&Read(mc+0xf0,render)&&*render;}
inline void TimerSync(uint64_t a,uint64_t b){*(float*)(b+0x10)=*(float*)(a+0x10);*(float*)(b+8)=*(float*)(a+8);*(float*)(b+0x14)=*(float*)(a+0x14);}
inline void(*utilityLog)(const char*)=nullptr;
inline void TimerWork(){auto& t=Timer();uint64_t a{},b{};if(!TimerClocks(&a,&b)){static bool told=false;if(t.enabled&&!told&&utilityLog){told=true;utilityLog("Timer: game clocks not found");}return;}
 {static bool told=false;if(t.enabled&&!told&&utilityLog){told=true;char line[160];snprintf(line,sizeof(line),"Timer: clocks found, simulation %.2f tps, render %.2f tps, wanted %.2f",*(float*)a,*(float*)b,(float)t.rate);utilityLog(line);}}
 if(t.enabled){if(!t.has){t.saved=*(float*)a;t.has=true;TimerSync(a,b);}const float r=t.rate;*(float*)a=r;*(float*)b=r;}
 else if(t.has){t.has=false;*(float*)a=t.saved;*(float*)b=t.saved;TimerSync(a,b);}}
inline void TimerTick(){if(Timer().fault)return;__try{TimerWork();}__except(EXCEPTION_EXECUTE_HANDLER){Timer().fault=true;}}
}
