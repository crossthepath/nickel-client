// Movement simulator for the script API (player.simulate): predicts the local player's next ticks with the game's player
// physics, from the current state and an input, without touching the player.
// - State: feet position (centre of the box bottom), box size *(actor+0x248), velocity *(*(actor+0x240)+0x18), on-ground
//   flag, yaw (degrees).
// - One tick: jump (first tick only, on the ground: vertical speed 0.42, sprint adds 0.2 along the yaw); input (strafe,
//   forward in [-1, 1], x 0.3 when sneaking, normalized when longer than 1) times the acceleration: on the ground
//   speed x 0.16277136 / slip^3 with slip = block friction x 0.91 (speed 0.1, x 1.3 sprinting), in the air 0.02
//   (0.026 sprinting), turned by the yaw; then the move against the collision boxes of the blocks (BlockSource vtable
//   +0xb0 over the swept box, entries of 0x30 bytes, box at +8, entries flagged at +0x2c skipped): vertical first, then
//   the larger horizontal axis, with a 0.5625 step-up when a horizontal move is blocked on the ground; a blocked axis
//   stops its velocity; then gravity 0.08, vertical drag 0.98 and the horizontal slip.
// - Outcome per tick: Normal; UnloadedChunk when the box reaches an unloaded chunk; Unreliable in a liquid, on a ladder or
//   vine, in a web (not modelled); Mounted when riding; InvalidInput for non-finite input. The simulation stops after
//   the first outcome other than Normal (that tick is still reported).
#pragma once
#include "game_movement_e.hpp"
#include <cmath>
#include <cstring>
#include <string>
#include <vector>

namespace game {
struct SimInput{float strafe=0,forward=0,yaw=0,pitch=0;bool jump=false,sneak=false,sprint=false;};
struct SimTick{float pos[3]{},vel[3]{};bool onGround=false,landed=false,collided=false;int outcome=0;};   // 0 Normal, 2 Unreliable, 3 UnloadedChunk, 5 Mounted, 6 InvalidInput
struct SimBox{float v[6];};
struct SimWorld{Context c{};uint64_t region=0,fetch=0,chunkAt=0;};

// Collision boxes of the blocks inside `area` (absolute coordinates).
inline bool SimBoxes(const SimWorld& w,const float* area,std::vector<SimBox>& out){
 out.clear();if(!w.fetch)return false;static uint64_t buf[3]{};constexpr uint64_t elements=0x800,stride=0x30;
 if(!buf[0]){const uint64_t b=MoveBAllocate(elements*stride);if(!b)return false;buf[0]=buf[1]=b;buf[2]=b+elements*stride;}
 buf[1]=buf[0];if(!PfFetchRaw(w.fetch,w.region,buf,area)){buf[1]=buf[0];return false;}
 for(uint64_t e=buf[0];e<buf[1];e+=stride){uint8_t flagged{};float s[6]{};if(!Read(e+0x2c,&flagged)||!Floats(e+8,s,6))return false;if(flagged)continue;out.push_back(SimBox{{s[0],s[1],s[2],s[3],s[4],s[5]}});}
 return true;}
// Largest move of `box` along `axis` (0 x, 1 y, 2 z) by `d` before touching one of `boxes`.
inline float SimClip(const float* box,const std::vector<SimBox>& boxes,int axis,float d){
 const int a1=(axis+1)%3,a2=(axis+2)%3;
 for(const auto& b:boxes){if(b.v[a1+3]<=box[a1]+1e-7f||b.v[a1]>=box[a1+3]-1e-7f||b.v[a2+3]<=box[a2]+1e-7f||b.v[a2]>=box[a2+3]-1e-7f)continue;
  if(d>0&&b.v[axis]>=box[axis+3]-1e-7f)d=std::min(d,b.v[axis]-box[axis+3]);else if(d<0&&b.v[axis+3]<=box[axis]+1e-7f)d=std::max(d,b.v[axis+3]-box[axis]);}
 return d;}
inline void SimShift(float* box,int axis,float d){box[axis]+=d;box[axis+3]+=d;}
// Moves `box` by `want` with collisions; `got` = the move done.
inline void SimMove(const std::vector<SimBox>& boxes,float* box,const float* want,float* got){
 float d[3]={want[0],want[1],want[2]};d[1]=SimClip(box,boxes,1,d[1]);SimShift(box,1,d[1]);
 const int first=std::fabs(d[0])>=std::fabs(d[2])?0:2,second=first==0?2:0;
 d[first]=SimClip(box,boxes,first,d[first]);SimShift(box,first,d[first]);d[second]=SimClip(box,boxes,second,d[second]);SimShift(box,second,d[second]);
 for(int i=0;i<3;++i)got[i]=d[i];}
inline float SimFriction(const SimWorld& w,const float* feet){const uint64_t b=PfBlockRaw(w.c,w.region,(int)std::floor(feet[0]),(int)std::floor(feet[1]-0.5f),(int)std::floor(feet[2]));
 std::string n;if(!b||!XrayBlockName((uintptr_t)b,&n))return 0.6f;return PfBlockProps(PfFullId(n)).friction;}
// Liquid, climbable or web anywhere in the box: the simplified physics no longer applies.
inline bool SimUnreliable(const SimWorld& w,const float* box){
 for(int x=(int)std::floor(box[0]);x<=(int)std::floor(box[3]-1e-4f);++x)for(int y=(int)std::floor(box[1]);y<=(int)std::floor(box[4]-1e-4f);++y)for(int z=(int)std::floor(box[2]);z<=(int)std::floor(box[5]-1e-4f);++z){
  const uint64_t b=PfBlockRaw(w.c,w.region,x,y,z);uint8_t f0{},f1{};if(BlockFlags(b,&f0,&f1)&&(f1&2))return true;std::string n;if(b&&XrayBlockName((uintptr_t)b,&n)){const PfProps p=PfBlockProps(PfFullId(n));if(p.climb||p.web)return true;}}
 return false;}
inline bool SimLoaded(const SimWorld& w,const float* box){if(!w.chunkAt)return true;
 for(int cx=(int)std::floor(box[0])>>4;cx<=((int)std::floor(box[3])>>4);++cx)for(int cz=(int)std::floor(box[2])>>4;cz<=((int)std::floor(box[5])>>4);++cz)if(!PfChunkRaw(w.chunkAt,w.region,cx,cz))return false;return true;}

// Runs up to `ticks` ticks; false when the player or the world is not available (nothing to report).
inline bool Simulate(const SimInput& in,int ticks,std::vector<SimTick>& out){
 out.clear();SimWorld w;if(!Local(&w.c))return false;w.region=ActorRegion(w.c);if(!w.region)return false;w.fetch=Virtual0(w.c,w.region,0xb0);w.chunkAt=Virtual(w.c,w.region,0x148);
 uint64_t st{},bb{};float box[6]{},v[3]{};if(!Read(w.c.actor+0x240,&st)||!st||!Read(w.c.actor+0x248,&bb)||!bb||!Floats(bb,box,6))return false;if(!Floats(st+0x18,v,3))v[0]=v[1]=v[2]=0;
 bool onGround=ActionOnGround(w.c.actor);
 const bool finite=std::isfinite(in.strafe)&&std::isfinite(in.forward)&&std::isfinite(in.yaw);
 uintptr_t ride{};uint32_t mount{};const bool mounted=Element(w.c.registry,w.c.entity,"PassengerComponent",16,&ride)&&Read(ride,&mount)&&(mount&0x3ffff)!=0x3ffff;
 const float r=in.yaw*0.017453292f,sn=std::sin(r),cs=std::cos(r);std::vector<SimBox> boxes;
 for(int t=0;t<ticks;++t){SimTick k;const bool wasGround=onGround;
  if(!finite||mounted){k.outcome=!finite?6:5;for(int i=0;i<3;++i){k.vel[i]=v[i];}k.pos[0]=(box[0]+box[3])*.5f;k.pos[1]=box[1];k.pos[2]=(box[2]+box[5])*.5f;k.onGround=onGround;out.push_back(k);break;}
  const float feet[3]={(box[0]+box[3])*.5f,box[1],(box[2]+box[5])*.5f};const float slip=onGround?SimFriction(w,feet)*0.91f:0.91f;
  if(t==0&&in.jump&&onGround){v[1]=0.42f;if(in.sprint){v[0]+=-sn*0.2f;v[2]+=cs*0.2f;}}
  float s=std::clamp(in.strafe,-1.f,1.f),f=std::clamp(in.forward,-1.f,1.f);if(in.sneak){s*=0.3f;f*=0.3f;}
  const float speed=0.1f*(in.sprint?1.3f:1.f),accel=onGround?speed*(0.16277136f/(slip*slip*slip)):(in.sprint?0.026f:0.02f);
  float len=s*s+f*f;if(len>=1e-4f){len=std::sqrt(len);if(len<1.f)len=1.f;s=s/len*accel;f=f/len*accel;v[0]+=s*cs-f*sn;v[2]+=f*cs+s*sn;}
  const float area[6]={std::min(box[0],box[0]+v[0])-1.f,std::min(box[1],box[1]+v[1])-1.f,std::min(box[2],box[2]+v[2])-1.f,std::max(box[3],box[3]+v[0])+1.f,std::max(box[4],box[4]+v[1])+1.5f,std::max(box[5],box[5]+v[2])+1.f};
  if(!SimLoaded(w,area)){k.outcome=3;}
  SimBoxes(w,area,boxes);float got[3]{};float trial[6];memcpy(trial,box,24);SimMove(boxes,trial,v,got);
  const bool blockedH=std::fabs(got[0]-v[0])>1e-6f||std::fabs(got[2]-v[2])>1e-6f;
  if(blockedH&&(onGround||(v[1]<0&&got[1]!=v[1]))){   // step-up
   float up[6];memcpy(up,box,24);const float lift=SimClip(up,boxes,1,0.5625f);SimShift(up,1,lift);const float flat[3]={v[0],0,v[2]};float g2[3]{};SimMove(boxes,up,flat,g2);
   const float down=SimClip(up,boxes,1,-lift+std::min(0.f,v[1]));SimShift(up,1,down);
   if(g2[0]*g2[0]+g2[2]*g2[2]>got[0]*got[0]+got[2]*got[2]+1e-6f){memcpy(trial,up,24);got[0]=g2[0];got[2]=g2[2];got[1]=lift+down;}}
  memcpy(box,trial,24);
  const bool hitX=std::fabs(got[0]-v[0])>1e-6f,hitZ=std::fabs(got[2]-v[2])>1e-6f,hitY=std::fabs(got[1]-v[1])>1e-6f;
  onGround=hitY&&v[1]<0;if(hitX)v[0]=0;if(hitZ)v[2]=0;if(hitY)v[1]=0;
  v[1]=(v[1]-0.08f)*0.98f;v[0]*=slip;v[2]*=slip;
  k.pos[0]=(box[0]+box[3])*.5f;k.pos[1]=box[1];k.pos[2]=(box[2]+box[5])*.5f;for(int i=0;i<3;++i)k.vel[i]=v[i];k.onGround=onGround;k.landed=onGround&&!wasGround;k.collided=hitX||hitZ;
  if(!k.outcome&&SimUnreliable(w,box))k.outcome=2;out.push_back(k);if(k.outcome)break;}
 return true;}
inline bool SimulateGuarded(const SimInput& in,int ticks,std::vector<SimTick>& out){__try{return Simulate(in,ticks,out);}__except(EXCEPTION_EXECUTE_HANDLER){out.clear();return false;}}
}
