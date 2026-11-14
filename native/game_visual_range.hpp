// Visual Range (Utility): level tick subscriber announcing players entering or leaving render range.
// - Outside a game: the snapshot is cleared and a timestamp taken; in game, nothing happens for the first 250 ms.
// - Snapshot: players (PlayerComponent) other than the local player, from ActorOwnerComponent.
// - Local player position: *(actor+0x240) (x, y, z). If a previous snapshot exists and the player moved at most 64
// blocks: "§e{0}§r entered your visual range" for each new player and, with Leave Messages, "§7{0} left your
// visual range" for each one gone (strings visual_range.entered / .left). Beyond 64 blocks (teleport) nothing is shown.
// - Message: game function RVA 0x6da9820(local player, std::string*, empty optional<std::string>*).
#pragma once
#include "game_packets.hpp"
#include <atomic>
#include <map>
#include <string>

namespace game {
struct VisualRangeShared{std::atomic<bool> enabled{false},leave{true},fault{false};std::map<uint64_t,std::string> seen;float last[3]{};bool valid=false,out=true;uint64_t outAt=0;};
inline VisualRangeShared& VisualRange(){static auto* s=new VisualRangeShared;return *s;}
inline uint64_t VisualRangeNow(){return GetTickCount64();}
inline void VisualRangeWork(){auto& v=VisualRange();Context c{};
 if(!v.enabled||!Local(&c)){v.seen.clear();v.valid=false;v.out=true;v.outAt=VisualRangeNow();return;}
 if(v.out&&VisualRangeNow()-v.outAt<250)return;v.out=false;
 uint64_t state{};float pos[3];if(!Read(c.actor+0x240,&state)||!state||!Floats(state,pos,3))return;
 uintptr_t owners{};uint64_t begin{},end{};if(!Storage(c.registry,Fnv("ActorOwnerComponent"),&owners)||!Read(owners+0x20,&begin)||!Read(owners+0x28,&end)||end<begin||(end-begin)%4||(end-begin)>4*65536)return;
 const auto roster=SnapshotVisualRoster(c);std::map<uint64_t,std::string> now;
 for(uint64_t at=begin;at<end;at+=4){uint32_t entity{};uintptr_t slot{},element{};uint64_t actor{},id{};if(!Read(at,&entity))break;if(entity>=0xfffc0000u||entity==c.entity)continue;
  if(!Has(c.registry,entity,"PlayerComponent")||!InStorage(owners,entity,8,&slot)||!Read(slot,&actor)||!actor)continue;
  if(IsVisualBot(c,entity,true,roster))continue;
  if(!Element(c.registry,entity,"RuntimeIDComponent",8,&element)||!Read(element,&id))continue;
  char name[256]{};if(!RawText(actor+0xbe8,name,sizeof(name))||!name[0])continue;now[id]=name;}
 const float dx=pos[0]-v.last[0],dy=pos[1]-v.last[1],dz=pos[2]-v.last[2];
 if(v.valid&&std::sqrt(dx*dx+dy*dy+dz*dz)<=64.f){
  for(auto& p:now)if(!v.seen.count(p.first))ClientMessage(c,"\xc2\xa7""e"+p.second+"\xc2\xa7r entered your visual range");
  if(v.leave)for(auto& p:v.seen)if(!now.count(p.first))ClientMessage(c,"\xc2\xa7""7"+p.second+" left your visual range");}
 v.seen.swap(now);v.last[0]=pos[0];v.last[1]=pos[1];v.last[2]=pos[2];v.valid=true;}
inline void VisualRangeTick(){if(VisualRange().fault)return;__try{VisualRangeWork();}__except(EXCEPTION_EXECUTE_HANDLER){VisualRange().fault=true;}}
}
