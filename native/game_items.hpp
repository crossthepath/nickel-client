// Dropped items for Item ESP. Read-only, on the overlay render thread like the other ESPs.
// - entities of ActorOwnerComponent whose ActorTypeComponent == 0x40;
// - item stack at actor+0x3d0 (signature 0xfe5c95a7022e6fa5, resolved from the version profile);
// item = StackItem(stack); identifier = std::string at item+0xd8 (signature 0x367d49153d39e471);
// count = byte stack+0x22; custom name = NBT stack+0x10 "display" / "Name", § codes and control characters removed;
// - box = position (state +0x240: current +0, previous +0xc) minus OffsetsComponent, with the half-extents of the
// AABB at actor+0x248. The current position is used, without partial-tick interpolation.
#pragma once
#include "game_events.hpp"
#include <string>
#include <vector>

namespace game {
struct GroundItem{float lo[3],hi[3];uint64_t item;std::string id,custom;int count;};
inline std::string StripFormatting(const std::string& in){std::string out;for(size_t i=0;i<in.size();++i){const unsigned char c=in[i];if(c==0xc2&&i+1<in.size()&&(unsigned char)in[i+1]==0xa7){i+=2;continue;}if(c<0x20||c==0x7f)continue;out+=(char)c;}return out;}
// Text of a string tag in a CompoundTag (NbtNode; type 8 = string, std::string value at +0x48).
inline bool NbtString(uint64_t compound,const char* key,char* out,size_t capacity){const uint64_t node=NbtNode(compound,key);uint8_t type{};return node&&Read(node+0x68,&type)&&type==8&&Text(node+0x48,out,capacity);}
inline int ReadGroundItems(GroundItem* out,int capacity,float range){
 Context c{};uintptr_t owners{},types{};float origin[3];if(!Local(&c)||!Storage(c.registry,Fnv("ActorOwnerComponent"),&owners)||!Storage(c.registry,Fnv("ActorTypeComponent"),&types))return 0;
 {uint64_t state{};if(!Read(c.actor+0x240,&state)||!state||!Floats(state,origin,3))return 0;}
 uint64_t begin{},end{};if(!Read(owners+0x20,&begin)||!Read(owners+0x28,&end)||end<begin||(end-begin)%4||(end-begin)>4*65536)return 0;int n=0;
 for(uint64_t at=begin;at<end&&n<capacity;at+=4){uint32_t entity{};uintptr_t element{},slot{};int32_t type{};uint64_t actor{},state{},box{};if(!Read(at,&entity))break;if(entity>=0xfffc0000u)continue;
  if(!InStorage(types,entity,4,&element)||!Read(element,&type)||type!=0x40)continue;
  if(!InStorage(owners,entity,8,&slot)||!Read(slot,&actor)||!actor||!Read(actor+0x240,&state)||!state)continue;float p[3],b[6],drop=0;if(!Floats(state,p,3))continue;
  if(range>0&&(std::fabs(p[0]-origin[0])>range||std::fabs(p[1]-origin[1])>range||std::fabs(p[2]-origin[2])>range))continue;
  if(!Read(actor+0x248,&box)||!box||!Floats(box,b,6))continue;if(Element(c.registry,entity,"OffsetsComponent",0x44,&element))Floats(element,&drop,1);
  const uint64_t stack=actor+0x3d0;const uint64_t item=StackItem(stack);if(!item)continue;
  GroundItem g{};const float hx=(b[3]-b[0])*.5f,hz=(b[5]-b[2])*.5f,low=p[1]-drop;g.lo[0]=p[0]-hx;g.lo[1]=low;g.lo[2]=p[2]-hz;g.hi[0]=p[0]+hx;g.hi[1]=low+(b[4]-b[1]);g.hi[2]=p[2]+hz;
  g.item=item;char text[128]{};if(Text(item+0xd8,text,sizeof(text)))g.id=text;uint8_t count{};g.count=Read(stack+0x22,&count)?count:1;
  uint64_t tag{};char name[256]{};if(Read(stack+0x10,&tag)&&tag){const uint64_t display=NbtNode(tag,"display");uint8_t t{};if(display&&Read(display+0x68,&t)&&t==10&&NbtString(display+0x48,"Name",name,sizeof(name)))g.custom=StripFormatting(name);}
  out[n++]=std::move(g);}
 return n;}
}

namespace game {
// Players for Jump Circles: runtime id, feet position (state +0x240 minus OffsetsComponent), on-ground flag
// (OnGroundFlagComponent 0xc29078a0) and local-player flag. Read-only.
struct JumpBody{uint64_t id;float feet[3];bool ground,local;};
inline int ReadJumpBodies(JumpBody* out,int capacity){
 Context c{};uintptr_t owners{},players{},ground{};if(!Local(&c)||!Storage(c.registry,Fnv("ActorOwnerComponent"),&owners)||!Storage(c.registry,Fnv("PlayerComponent"),&players))return 0;const bool hasGround=Storage(c.registry,0xc29078a0u,&ground);
 uint64_t begin{},end{};if(!Read(owners+0x20,&begin)||!Read(owners+0x28,&end)||end<begin||(end-begin)%4||(end-begin)>4*65536)return 0;int n=0;
 for(uint64_t at=begin;at<end&&n<capacity;at+=4){uint32_t entity{},packed{};uintptr_t slot{},element{};uint64_t actor{},state{};if(!Read(at,&entity))break;if(entity>=0xfffc0000u||!Packed(players,entity,&packed))continue;
  if(!InStorage(owners,entity,8,&slot)||!Read(slot,&actor)||!actor||!Read(actor+0x240,&state)||!state)continue;JumpBody b{};float drop=0;if(!Floats(state,b.feet,3))continue;
  if(Element(c.registry,entity,"OffsetsComponent",0x44,&element))Floats(element,&drop,1);b.feet[1]-=drop;if(!Element(c.registry,entity,"RuntimeIDComponent",8,&element)||!Read(element,&b.id))continue;
  b.ground=hasGround&&Packed(ground,entity,&packed);b.local=entity==c.entity;out[n++]=b;}
 return n;}
}
