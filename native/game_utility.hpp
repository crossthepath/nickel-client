// Simple Utility modules, subscribed to the level tick (LevelTickEvent).
// Spammer: in game, once the delay (ms) since the last send has elapsed (timer armed on enable), sends a random message
// from the list; a leading "/" sends it as a command, otherwise as chat.
// Auto Disconnect: out of game, the trigger is reset. In game, if health <= threshold and not yet triggered:
// ServerDisconnect mode -> DisconnectPacket #5 (+0x30 = 0, message at +0x38, +0x80 = 0) handled locally, then the
// module turns off; CommandDisconnect mode -> command "/text" (a leading "/" in the setting is dropped, so "/me" and "me" both work), auto_disconnect.command_warning message, and the
// module turns off only with Disable on Execute.
#pragma once
#include "game_packets.hpp"
#include "game_packet_hooks.hpp"
#include "game_utility_packets.hpp"
#include "game_skin.hpp"
#include "game_timer.hpp"
#include "game_visual_range.hpp"
#include "game_actions.hpp"
#include "game_air_place.hpp"
#include "game_bed_breaker.hpp"
#include "game_base_finder.hpp"
#include "game_player.hpp"
#include "game_movement2.hpp"
#include "game_movement3.hpp"
#include "game_schematic.hpp"
#include "game_rotation.hpp"
#include "game_lag.hpp"
#include "game_inventory.hpp"
#include <algorithm>
#include <mutex>
#include <vector>

namespace game {
struct SpammerShared{std::mutex guard;std::vector<std::string> messages;double delay=50;bool enabled=false,armed=false;uint64_t last=0;std::atomic<bool> fault{false};};
inline SpammerShared& Spammer(){static auto* s=new SpammerShared;return *s;}
inline uint64_t UtilityMs(){LARGE_INTEGER f{},c{};QueryPerformanceFrequency(&f);QueryPerformanceCounter(&c);return (uint64_t)((double)c.QuadPart*1000.0/(double)f.QuadPart);}
inline void SpammerWork(){auto& s=Spammer();std::string pick;{std::lock_guard<std::mutex> lock(s.guard);
  if(!s.enabled){s.armed=false;return;}if(!s.armed){s.armed=true;s.last=UtilityMs();return;}
  Context c{};if(!Local(&c))return;const uint64_t now=UtilityMs();if((double)(now-s.last)<=s.delay)return;s.last=now;
  if(s.messages.empty())return;static uint64_t seed=GetTickCount64()*0x9e3779b97f4a7c15ull;uint64_t z=(seed+=0x9e3779b97f4a7c15ull);z=(z^(z>>30))*0xbf58476d1ce4e5b9ull;z=(z^(z>>27))*0x94d049bb133111ebull;z^=z>>31;
  pick=s.messages[z%s.messages.size()];}
 Context c{};if(!Local(&c)||pick.empty())return;if(pick[0]=='/')SendCommand(c,pick);else SendChat(c,pick);}
inline void SpammerTick(){if(Spammer().fault)return;__try{SpammerWork();}__except(EXCEPTION_EXECUTE_HANDLER){Spammer().fault=true;}}

struct AutoDisconnectShared{std::mutex guard;bool enabled=false,command=false,disableOnExecute=false,triggered=false;int threshold=6;std::string text;std::atomic<bool> disableRequest{false},fault{false};};
inline AutoDisconnectShared& AutoDisconnect(){static auto* s=new AutoDisconnectShared;return *s;}
inline void AutoDisconnectWork(){auto& a=AutoDisconnect();Context c{};std::lock_guard<std::mutex> lock(a.guard);
 if(!Local(&c)||!a.enabled){a.triggered=false;return;}
 float health{},maximum{};if(!ReadHealth(&health,&maximum))return;if((float)a.threshold<health||a.triggered)return;a.triggered=true;
 if(!a.command){GamePacket p=CreatePacket(c,5);if(p.object){*reinterpret_cast<uint8_t*>(p.object+0x30)=0;PacketText(p.object+0x38)="Nickel Auto Disconnect was triggered. The module was disabled so you can rejoin.";*reinterpret_cast<uint32_t*>(p.object+0x80)=0;ReceivePacket(c,&p);ReleasePacket(p);}
  a.disableRequest=true;return;}
 SendCommand(c,"/"+(a.text.size()&&a.text[0]=='/'?a.text.substr(1):a.text));ClientMessage(c,"Auto Disconnect triggered the disconnect command and will not run again until you reconnect.");
 if(a.disableOnExecute)a.disableRequest=true;}
inline void AutoDisconnectTick(){if(AutoDisconnect().fault)return;__try{AutoDisconnectWork();}__except(EXCEPTION_EXECUTE_HANDLER){AutoDisconnect().fault=true;}}

// No Packet (PacketSendEvent): without Filter every send is cancelled; with Filter only the listed ids
// (decimal integers). Id = packet->vtable[1]().
struct NoPacketShared{SRWLOCK lock=SRWLOCK_INIT;bool enabled=false,filter=false;std::vector<int> ids;};
inline NoPacketShared& NoPacket(){static auto* s=new NoPacketShared;return *s;}
inline int PacketId(uintptr_t packet){uint64_t table{},get{};if(!packet||!Read(packet,&table)||!table||!Read(table+8,&get)||!get)return -1;return reinterpret_cast<int(*)(uintptr_t)>(get)(packet);}
inline bool NoPacketWork(uintptr_t packet){auto& n=NoPacket();AcquireSRWLockShared(&n.lock);const bool on=n.enabled,filter=n.filter;std::vector<int> ids=filter?n.ids:std::vector<int>{};ReleaseSRWLockShared(&n.lock);
 if(!on)return false;if(!filter)return true;const int id=PacketId(packet);for(int x:ids)if(x==id)return true;return false;}
inline bool NoPacketGuarded(uintptr_t packet){__try{return NoPacketWork(packet);}__except(EXCEPTION_EXECUTE_HANDLER){return false;}}
// Luau scripts (native/script_engine.hpp): "." commands, packetSend, tick.
inline bool(*volatile scriptSend)(uintptr_t)=nullptr;inline void(*volatile scriptTick)()=nullptr;
inline void(*volatile playerSendC)(uintptr_t)=nullptr;inline void(*volatile playerTickC)()=nullptr;   // Player batch C (game_player_c.hpp)
inline bool UtilitySend(uintptr_t packet){if(NoPacketGuarded(packet))return true;if(auto ss=scriptSend)if(ss(packet))return true;if(auto pc=playerSendC)pc(packet);InstaPacketSend(packet);PacketLoggerSend(packet);return false;}


// Single subscriber installed on the shared tick hook (game_map.hpp).
inline void PvpBotLevelTick();   // game_pvp_bot.hpp
inline void MoveATick();inline void MoveBTick();inline void MoveCTick();inline void AutoMlgTick();inline void PfTick();inline void PearlChaserTick();   // game_movement_a..e.hpp
inline void UtilityTick(){{Context c{};if(Local(&c)){ItemRegistryCapture(c);static bool told=false;if(!told&&ItemRegistry().control.load()&&actionLog){told=true;actionLog("Actions: item registry seen on the level tick (lent to other threads for item descriptors)");}}}if(auto st=scriptTick)st();AirPlaceTick();BedBreakerTick();BaseFinderTick();PlayerTick();Move3Tick();MoveATick();MoveBTick();MoveCTick();AutoMlgTick();PvpBotLevelTick();PfTick();PearlChaserTick();SchematicTick();ScaffoldTick();NukerTick();WindChargeTick();PearlCatchTick();TimerTick();VisualRangeTick();SpammerTick();AutoDisconnectTick();SkinStealerTick();if(auto pt=playerTickC)pt();Move2Tick();}   // No Fall last: it caps the velocity the other modules set
}
