// Crystal Optimizer: removes an end crystal client-side as soon as the attack on it is sent.
// - PacketSend: an InventoryTransactionPacket (0x1e, not cancelled) whose transaction (+0x170) is UseItemOnEntity
//   (type 3 at +8) with action Attack (1 at +0x70) on an actor of type 0x47 (end crystal, runtime id at +0x68) queues
//   the crystal's ActorUniqueID once.
// - ScreenViewRender: for each queued id still known by the level, a RemoveActorPacket (0x0e, unique id at +0x30) is
//   handled locally.
// - PacketReceive: Login, Disconnect, StartGame, ChangeDimension (1, 5, 0x0b, 0x3d) and Transfer (0x55) clear the queue;
//   so do enabling and disabling.
// Anti Crystal: the y position (+0x3c) of every sent PlayerAuthInputPacket (0x90) and MovePlayerPacket (0x13) is lowered
// by Offset. With Safe it pauses on an auth input reporting HandledTeleport (input bit 37), when the player was in water
// or lava (WasInWaterFlagComponent, WasInLavaFlagComponent), without a region, or when the block at the feet
// (floor(x), floor(y - height offset), floor(z)) is a ladder, vine or scaffolding.
#pragma once
#include "game_combat.hpp"
#include "game_packets.hpp"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <mutex>
#include <string>
#include <vector>

namespace game {
struct CrystalOptimizerShared{std::mutex guard;std::atomic<bool> enabled{false};std::vector<uint64_t> pending;volatile long fault=0;};
inline CrystalOptimizerShared& CrystalOptimizer(){static auto* s=new CrystalOptimizerShared;return *s;}
inline void CrystalClear(){auto& s=CrystalOptimizer();std::lock_guard<std::mutex> lock(s.guard);s.pending.clear();}
inline int CrystalPacketId(uintptr_t packet){uint64_t table{},get{};if(!Read(packet,&table)||!table||!Read(table+8,&get)||!get)return -1;return reinterpret_cast<int(*)(uintptr_t)>(get)(packet);}

inline void CrystalOptimizerSend(uintptr_t packet){auto& s=CrystalOptimizer();if(!s.enabled||CrystalPacketId(packet)!=0x1e)return;
 uint64_t tr{},runtime{};int32_t type{},action{};if(!Read(packet+0x170,&tr)||!tr||!Read(tr+8,&type)||type!=3||!Read(tr+0x70,&action)||action!=1||!Read(tr+0x68,&runtime))return;
 Context c{};uintptr_t actor{},registry{},element{};uint32_t entity{};int32_t kind{};uint64_t unique{};
 if(!Local(&c)||!ActorByRuntimeId(c.registry,runtime,&actor)||!actor||!ActorEntity(actor,&registry,&entity))return;
 if(!Element(registry,entity,"ActorTypeComponent",4,&element)||!Read(element,&kind)||kind!=0x47)return;
 if(!Element(registry,entity,"ActorUniqueIDComponent",8,&element)||!Read(element,&unique))return;
 std::lock_guard<std::mutex> lock(s.guard);if(std::find(s.pending.begin(),s.pending.end(),unique)==s.pending.end())s.pending.push_back(unique);}

// Actor of the local level with this ActorUniqueID.
inline bool CrystalKnown(const Context& c,uint64_t unique){uintptr_t owners{};uint64_t begin{},end{};if(!Storage(c.registry,Fnv("ActorOwnerComponent"),&owners)||!Read(owners+0x20,&begin)||!Read(owners+0x28,&end)||end<begin||(end-begin)>4*65536)return false;
 for(uint64_t at=begin;at<end;at+=4){uint32_t entity{};uintptr_t element{};uint64_t id{};if(!Read(at,&entity)||entity>=0xfffc0000u)continue;if(Element(c.registry,entity,"ActorUniqueIDComponent",8,&element)&&Read(element,&id)&&id==unique)return true;}
 return false;}
inline void CrystalOptimizerRender(){auto& s=CrystalOptimizer();std::vector<uint64_t> ids;{std::lock_guard<std::mutex> lock(s.guard);ids.swap(s.pending);}
 if(ids.empty())return;Context c{};if(!Local(&c))return;
 for(uint64_t id:ids){if(!CrystalKnown(c,id))continue;GamePacket p=CreatePacket(c,0x0e);if(p.object){*reinterpret_cast<uint64_t*>(p.object+0x30)=id;ReceivePacket(c,&p);}ReleasePacket(p);}}
inline void CrystalOptimizerReceive(uintptr_t packet){if(!CrystalOptimizer().enabled)return;const int id=CrystalPacketId(packet);
 if((id>=0&&id<0x3e&&((0x2000000000000822ull>>id)&1))||id==0x55)CrystalClear();}

struct AntiCrystalShared{std::atomic<bool> enabled{false},safe{false};std::atomic<float> offset{0.1f};};
inline AntiCrystalShared& AntiCrystal(){static auto* s=new AntiCrystalShared;return *s;}
inline uint64_t AntiCrystalBlockAt(uint64_t function,uint64_t region,const int32_t* pos){__try{return reinterpret_cast<uint64_t(*)(uint64_t,const int32_t*)>(function)(region,pos);}__except(EXCEPTION_EXECUTE_HANDLER){return 0;}}
// False when Safe asks to leave the packet alone.
inline bool AntiCrystalAllowed(){if(!AntiCrystal().safe)return true;Context c{};if(!Local(&c))return false;
 if(Has(c.registry,c.entity,"WasInWaterFlagComponent")||Has(c.registry,c.entity,"WasInLavaFlagComponent"))return false;
 uint64_t owner{},position{},legacy{};float pos[3],lift=0;uintptr_t element{};if(!Read(c.actor+0x1f0,&owner)||!owner)return true;
 const uint64_t regionOf=Virtual0(c,owner,0x68);if(!regionOf)return true;uint64_t region{};if(!CallGetter(regionOf,owner,&region)||!region)return true;
 if(!Read(c.actor+0x240,&position)||!position||!Floats(position,pos,3))return false;if(Element(c.registry,c.entity,"OffsetsComponent",0x44,&element))Floats(element,&lift,1);
 const int32_t at[3]={(int32_t)floorf(pos[0]),(int32_t)floorf(pos[1]-lift),(int32_t)floorf(pos[2])};const uint64_t getBlock=Virtual0(c,region,0x10);
 const uint64_t block=getBlock?AntiCrystalBlockAt(getBlock,region,at):0;char name[96]{};if(!block||!Read(block+0x68,&legacy)||!legacy||!Text(legacy+0xe8,name,sizeof(name)))return false;
 const std::string text=name;return text.find("ladder")==std::string::npos&&text.find("vine")==std::string::npos&&text.find("scaffolding")==std::string::npos;}
inline void AntiCrystalSend(uintptr_t packet){auto& s=AntiCrystal();if(!s.enabled)return;const int id=CrystalPacketId(packet);
 if(id==0x90){uint64_t bits{};if(s.safe&&Read(packet+0x88,&bits)&&((bits>>37)&1))return;}else if(id!=0x13)return;
 if(AntiCrystalAllowed())*reinterpret_cast<float*>(packet+0x3c)-=s.offset.load();}
}
