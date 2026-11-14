// Air Place: places blocks against air.
// Each tick (local player alive, holding a block): aim point = eye + look x min(Range, game-mode reach); target cell =
// floor of that point, must be replaceable and outside the player box. Prefer Support: if a solid neighbour face is in
// reach (same search as Auto Block Reset), place normally on it. Otherwise find the face where the look ray enters the
// cell (slab method) and place on the neighbouring cell's opposite face, contact point clamped to the cell.
// Held right click: one placement, then one every Delay ms, never the same cell twice while held; the arm swings.
// Preview: cell filled with Fill Color, Preview Color outline of 1.5.
#pragma once
#include "game_actions.hpp"
#include <atomic>
#include <mutex>

namespace game {
struct AirPlaceShared{std::mutex guard;bool enabled=false,support=true,preview=true;float range=3;int delay=150;std::atomic<bool> rightHeld{false};
 bool hasTarget=false;int32_t target[3]{};bool placedValid=false;int32_t placed[3]{};uint64_t lastNs=0;bool wasHeld=false;};
inline AirPlaceShared& AirPlace(){static auto* s=new AirPlaceShared;return *s;}
inline uint64_t AirNowNs(){LARGE_INTEGER f{},c{};QueryPerformanceFrequency(&f);QueryPerformanceCounter(&c);return (uint64_t)((double)c.QuadPart*1e9/(double)f.QuadPart);}
inline void AirPlaceWork(){
 auto& s=AirPlace();bool on,support;float range;int delay;{std::lock_guard<std::mutex> l(s.guard);on=s.enabled;support=s.support;range=s.range;delay=s.delay;}
 Context c{};if(!on||!Local(&c)){std::lock_guard<std::mutex> l(s.guard);s.hasTarget=false;s.placedValid=false;return;}
 const uint64_t stack=ActionHeld(c,c.actor);float eye[3]{};uint64_t state{},rot{};float pitch{},yaw{};
 if(!BlockStack(stack)||!Read(c.actor+0x240,&state)||!state||!Floats(state,eye,3)||!Read(c.actor+0x250,&rot)||!rot||!Read(rot,&pitch)||!Read(rot+4,&yaw)){std::lock_guard<std::mutex> l(s.guard);s.hasTarget=false;return;}
 const float reach=AbrReach(c,c.actor);const float dist=reach>0?std::fmin(range,reach):range;const float p=pitch*.0174532925f,y=yaw*.0174532925f;
 const float dir[3]={-std::sin(y)*std::cos(p),-std::sin(p),std::cos(y)*std::cos(p)};const float at[3]={eye[0]+dir[0]*dist,eye[1]+dir[1]*dist,eye[2]+dir[2]*dist};
 const int32_t cell[3]={(int32_t)std::floor(at[0]),(int32_t)std::floor(at[1]),(int32_t)std::floor(at[2])};
 const uint64_t region=ActorRegion(c);if(!region||!BlockReplaceable(RegionBlock(c,region,cell))){std::lock_guard<std::mutex> l(s.guard);s.hasTarget=false;return;}
 uint64_t box{};float bb[6]{};if(Read(c.actor+0x248,&box)&&box)for(int i=0;i<6;++i)Read(box+i*4,&bb[i]);
 if(cell[0]<bb[3]&&cell[1]<bb[4]&&cell[2]<bb[5]&&cell[0]+1>bb[0]&&cell[1]+1>bb[1]&&cell[2]+1>bb[2]){std::lock_guard<std::mutex> l(s.guard);s.hasTarget=false;return;}
 AbrPlace place{};bool supported=support&&AbrFind(c,c.actor,cell,reach>0?reach:range+1,&place);
 if(!supported){// entry face: largest entry t among the slabs; faces 0 -y, 1 +y, 2 -z, 3 +z, 4 -x, 5 +x
  float best=-1e9f;int face=-1;const float lo[3]={(float)cell[0],(float)cell[1],(float)cell[2]};
  const int axisFace[3][2]={{4,5},{0,1},{2,3}};for(int a=0;a<3;++a){if(std::fabs(dir[a])<1e-4f)continue;const float plane=dir[a]>0?lo[a]:lo[a]+1;const float t=(plane-eye[a])/dir[a];if(t>best){best=t;face=dir[a]>0?axisFace[a][0]:axisFace[a][1];}}
  if(face<0){std::lock_guard<std::mutex> l(s.guard);s.hasTarget=false;return;}
  static const int normal[6][3]={{0,-1,0},{0,1,0},{0,0,-1},{0,0,1},{-1,0,0},{1,0,0}};
  memcpy(place.pos,cell,12);for(int k=0;k<3;++k)place.nb[k]=cell[k]+normal[face][k];place.face=(uint8_t)(face^1);
  for(int k=0;k<3;++k){const float e=eye[k]+dir[k]*best;place.hit[k]=std::fmin(std::fmax(e,lo[k]),lo[k]+1);}place.ok=true;}
 bool fire=false;{std::lock_guard<std::mutex> l(s.guard);s.hasTarget=true;memcpy(s.target,cell,12);const bool held=s.rightHeld.load();
  if(!held){s.placedValid=false;s.wasHeld=false;return;}
  const bool same=s.placedValid&&!memcmp(s.placed,cell,12);if(same)return;
  if(s.wasHeld&&(double)(AirNowNs()-s.lastNs)/1e6<=(double)delay)return;fire=true;}
 if(!fire)return;ActionSwing(c,c.actor,1);bool ok=false;
 if(supported)ok=AbrPlaceBlock(c,c.actor,place,reach>0?reach:range+1);
 else{int32_t* sel=ActionSelected(c.actor);if(sel){const int32_t slot=*sel;const uint64_t nbBlock=RegionBlock(c,region,place.nb);int32_t runtime{};if(nbBlock)Read(nbBlock+0x114,&runtime);
   uint8_t* t=NewUseTransaction(c,stack);*(int32_t*)(t+0x68)=0;memcpy(t+0x70,place.nb,12);*(int32_t*)(t+0x7c)=runtime;*(int32_t*)(t+0x84)=slot;*(uint8_t*)(t+0x88)=0;*(uint8_t*)(t+0x80)=place.face;
   memcpy(t+0xf0,eye,12);const float rel[3]={place.hit[0]-place.nb[0],place.hit[1]-place.nb[1],place.hit[2]-place.nb[2]};memcpy(t+0xfc,rel,12);SendUseTransaction(c,c.actor,t);ok=true;}}
 if(ok){std::lock_guard<std::mutex> l(s.guard);memcpy(s.placed,cell,12);s.placedValid=true;s.lastNs=AirNowNs();s.wasHeld=true;}}
inline void AirPlaceTick(){static volatile long stopped_=0;if(!stopped_)__try{AirPlaceWork();}__except(game::FeatureFault("AirPlaceTick",&stopped_,GetExceptionInformation())){game::FeatureFlush();}}
}
