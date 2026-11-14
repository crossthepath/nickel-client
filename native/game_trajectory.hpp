// Trajectories: computed on the render thread (LevelRenderPrep, hook #21).
// - held item (without "minecraft:"): bow/arrow 0 (only the bow counts), snowball 1, egg/blue_egg/brown_egg 2,
// ender_pearl 3, fishing_rod/fishing_hook 4; Dynamic mode uses the default (BDS) parameters;
// - bow: speed = 3 * power/3, power = Item->vtable[0x28](item, stack, use duration (actor->vtable[0x498]), alpha),
// nothing if duration <= 0 or power <= 0; alpha = *(*(ClientInstance+0xe8)+8) clamped to [0,1];
// - local player origin: game function 0x2ceda40(&out, actor, alpha); other players: RenderPosition - Offsets + 1.62001;
// angles *(actor+0x250): [0] pitch, [1] yaw; direction (-sin yaw*cos pitch, -sin pitch, cos yaw*cos pitch);
// - at most 200 steps, distance <= 256; water drag if the material (*(BlockLegacy+0x28)) == 5 at block floor(pos)
// (block = region->vtable[0x28](region, &pos)); order 0: move with the old velocity; order 1: with the new one;
// - block collision: BlockSource::clip = region->vtable[0x198] (liquids 0, second flag 1, steps = floor(length)+1,
// shooter actor, std::function filter that always returns true); mobs: boxes *(actor+0x248) of living actors
// (ActorTypeComponent & 0x100) within 256, inflated by 0.3; block hit -> the cell; mob hit -> point +/- 0.1;
// - drawing: polyline with Color / Thickness and impact box (12 edges) if Impact, material name_tag_depth_tested; the
// first point is dropped when the origin is closer to the camera than "speed" blocks.
// "Other Players" only walks PlayerComponent entities (mobs' held items are not read), and other players' yaw comes
// from *(actor+0x250) rather than their head rotation.
#pragma once
#include "game_hand.hpp"
#include <array>
#include <functional>

namespace game {
struct TrajectoryShared{SRWLOCK lock=SRWLOCK_INIT;volatile long enabled=0,fault=0,seen=0;float color[4]{.298f,.8f,1,1},thickness=1.5f;bool impact=true,others=false;};
inline TrajectoryShared& Trajectory(){static TrajectoryShared* shared=new TrajectoryShared;return *shared;}
struct TrajectoryParams{float speed,gravity,air,water;bool order;float extra,factor;bool normalized;};
inline bool TrajectoryKind(const char* name,int* kind){
 const char* n=!strncmp(name,"minecraft:",10)?name+10:name;
 if(!strcmp(n,"bow")||!strcmp(n,"arrow"))*kind=0;else if(!strcmp(n,"snowball"))*kind=1;else if(!strcmp(n,"egg")||!strcmp(n,"blue_egg")||!strcmp(n,"brown_egg"))*kind=2;
 else if(!strcmp(n,"ender_pearl"))*kind=3;else if(!strcmp(n,"fishing_rod")||!strcmp(n,"fishing_hook"))*kind=4;else return false;return true;
}
inline TrajectoryParams TrajectoryBds(int kind){
 switch(kind){case 0:return {3.f,.05f,.99f,.6f,false,0,0,false};case 1:case 2:return {1.5f,.03f,.99f,.6f,false,0,0,false};case 3:return {1.5f,.025f,1.f,1.f,false,0,0,false};
  case 4:return {1.5f,.036799997f,.92f,.828f,true,.0075f,.4f,true};default:return {0,.05f,.99f,.6f,false,0,0,false};}
}
// Item in the selected slot (*(actor+0x5e0): slot +0x10, container *(+0xb8), vtable[0x38])
inline bool TrajectoryPrepare(const Context& context,uint64_t actor,float alpha,TrajectoryParams* out){
 uint64_t supplies{},inventory{};int32_t selected{};if(!Read(actor+0x5e0,&supplies)||!supplies||!Read(supplies+0x10,&selected)||selected<0||selected>40||!Read(supplies+0xb8,&inventory)||!inventory)return false;
 const uint64_t get=Virtual(context,inventory,0x38);if(!get)return false;const uint64_t stack=reinterpret_cast<uint64_t(*)(uint64_t,int)>(get)(inventory,selected);if(!ValidStack(stack))return false;
 const uint64_t item=ItemOf(stack);char full[64]{};if(!item||!Text(item+0xd8,full,sizeof(full)))return false;int kind{};if(!TrajectoryKind(full,&kind))return false;
 TrajectoryParams params=TrajectoryBds(kind);
 if(kind==0){const char* n=!strncmp(full,"minecraft:",10)?full+10:full;if(strcmp(n,"bow"))return false;
  const uint64_t useOf=Virtual(context,actor,0x498);const int duration=useOf?reinterpret_cast<int(*)(uint64_t)>(useOf)(actor):0;if(duration<=0)return false;
  const uint64_t powerOf=Virtual(context,item,0x28);if(!powerOf)return false;const float power=reinterpret_cast<float(*)(uint64_t,uint64_t,int,float)>(powerOf)(item,stack,duration,alpha);
  params.speed=power/TrajectoryBds(0).speed*params.speed;if(!(params.speed>0))return false;}
 *out=params;return true;
}
struct TrajectoryBody{float box[6];uint64_t actor;};
// Ray [from,to] against a box; entry t in [0,1]
inline bool TrajectoryRay(const float* box,const float* from,const float* to,float* t){
 float lo=0,hi=1;for(int i=0;i<3;++i){const float d=to[i]-from[i];if(std::fabs(d)>=1e-6f){float a=(box[i]-from[i])/d,b=(box[i+3]-from[i])/d;if(a>b)std::swap(a,b);lo=std::max(lo,a);hi=std::min(hi,b);if(lo>hi)return false;}
  else if(from[i]<box[i]||from[i]>box[i+3])return false;}*t=lo;return true;
}
// Living actors other than the shooter, with a box within 256 of the origin, inflated by 0.3
inline void TrajectoryBodies(const Context& context,uint64_t shooter,const float* start,std::vector<TrajectoryBody>& out){
 out.clear();uintptr_t owners{};if(!Storage(context.registry,Fnv("ActorOwnerComponent"),&owners))return;uint64_t begin{},end{};if(!Read(owners+0x20,&begin)||!Read(owners+0x28,&end)||end<begin||(end-begin)>4*65536)return;
 for(uint64_t at=begin;at<end;at+=4){uint32_t entity{};uintptr_t slot{},element{};uint64_t actor{},box{};float b[6];if(!Read(at,&entity)||entity>=0xfffc0000u)continue;
  if(!InStorage(owners,entity,8,&slot)||!Read(slot,&actor)||!actor||actor==shooter)continue;int32_t type{};if(!Element(context.registry,entity,"ActorTypeComponent",4,&element)||!Read(element,&type)||!(type&0x100))continue;
  if(!Read(actor+0x248,&box)||!box||!Floats(box,b,6))continue;float d2=0;for(int i=0;i<3;++i){const float c=std::clamp(start[i],b[i],b[i+3])-start[i];d2+=c*c;}if(std::sqrt(d2)>256)continue;
  TrajectoryBody body{{b[0]-.3f,b[1]-.3f,b[2]-.3f,b[3]+.3f,b[4]+.3f,b[5]+.3f},actor};out.push_back(body);}
}
struct TrajectoryPath{std::vector<std::array<float,3>> points;bool hit=false;float box[6]{};uint64_t actor=0;};   // actor: the mob hit, 0 for a block
// Flight simulation
inline void TrajectorySimulate(const Context& context,uint64_t shooter,uint64_t region,const TrajectoryParams& p,const float* start,const float* velocity,const std::vector<TrajectoryBody>& bodies,TrajectoryPath& path){
 path.points.clear();path.hit=false;path.actor=0;for(int i=0;i<3;++i)if(!std::isfinite(start[i])||!std::isfinite(velocity[i]))return;
 const uint64_t clipOf=Virtual(context,region,0x198),blockOf=Virtual(context,region,0x28);if(!clipOf||!blockOf)return;
 using Clip=void*(*)(uint64_t,void*,const float*,const float*,bool,bool,int,bool,bool,uint64_t,const std::function<bool()>*,bool);
 static const std::function<bool()> accept=[]{return true;};
 float pos[3]={start[0],start[1],start[2]},v[3]={velocity[0],velocity[1],velocity[2]},travelled=0;path.points.push_back({pos[0],pos[1],pos[2]});
 for(int step=0;step<200&&travelled<256.f;++step){
  int cell[3]={(int)std::floor(pos[0]),(int)std::floor(pos[1]),(int)std::floor(pos[2])};const uint64_t block=reinterpret_cast<uint64_t(*)(uint64_t,int*)>(blockOf)(region,cell);
  uint64_t legacy{};uint8_t material{};const bool water=block&&Read(block+0x68,&legacy)&&legacy&&Read(legacy+0x28,&material)&&material==5;const float f=water?p.water:p.air;
  const float nv[3]={v[0]*f,v[1]*f-p.gravity,v[2]*f};const float d[3]={p.order?nv[0]:v[0],p.order?nv[1]:v[1],p.order?nv[2]:v[2]};v[0]=nv[0];v[1]=nv[1];v[2]=nv[2];
  float length=std::sqrt(d[0]*d[0]+d[1]*d[1]+d[2]*d[2]);if(length<=1e-6f)continue;const float scale=std::min(1.f,(256.f-travelled)/length);
  const float to[3]={pos[0]+d[0]*scale,pos[1]+d[1]*scale,pos[2]+d[2]*scale};const float span=std::sqrt((to[0]-pos[0])*(to[0]-pos[0])+(to[1]-pos[1])*(to[1]-pos[1])+(to[2]-pos[2])*(to[2]-pos[2]));
  alignas(16) uint8_t result[0x200]{};reinterpret_cast<Clip>(clipOf)(region,result,pos,to,false,true,(int)std::floor(span)+1,false,false,shooter,&accept,false);
  uint64_t weak{};memcpy(&weak,result+0x40,8);if(weak){if(InterlockedDecrement((volatile long*)(weak+0xc))==0){uint64_t table{},release{};if(Read(weak,&table)&&Read(table+8,&release)&&release)reinterpret_cast<void(*)(uint64_t)>(release)(weak);}}
  int type{};memcpy(&type,result+0x18,4);bool any=false,onBlock=false;float best=0;
  if(type==0){float hp[3];memcpy(hp,result+0x2c,12);const float a=std::sqrt((hp[0]-pos[0])*(hp[0]-pos[0])+(hp[1]-pos[1])*(hp[1]-pos[1])+(hp[2]-pos[2])*(hp[2]-pos[2]));best=span>0?a/span:0;any=true;onBlock=true;}
  uint64_t struck=0;for(const auto& body:bodies){float t{};if(TrajectoryRay(body.box,pos,to,&t)&&(!any||t<best)){best=t;any=true;onBlock=false;struck=body.actor;}}
  if(any){const float hit[3]={pos[0]+(to[0]-pos[0])*best,pos[1]+(to[1]-pos[1])*best,pos[2]+(to[2]-pos[2])*best};
   if(onBlock){int b[3];memcpy(b,result+0x20,12);for(int i=0;i<3;++i){path.box[i]=(float)b[i];path.box[i+3]=(float)b[i]+1;}}else for(int i=0;i<3;++i){path.box[i]=hit[i]-.1f;path.box[i+3]=hit[i]+.1f;}
   path.hit=true;path.actor=onBlock?0:struck;path.points.push_back({hit[0],hit[1],hit[2]});return;}
  pos[0]=to[0];pos[1]=to[1];pos[2]=to[2];path.points.push_back({pos[0],pos[1],pos[2]});travelled+=span;
 }
}
// Initial velocity of a throw along (yaw, pitch) in degrees: the type's speed, or for the fishing hook (normalized) the
// shooter's motion plus factor x direction, renormalized to the speed.
inline void TrajectoryVelocity(const TrajectoryParams& p,const float* motion,float yaw,float pitch,float* vel){
 const float r=.017453292f,cp=std::cos(pitch*r),sy=std::sin(yaw*r),sp=std::sin(pitch*r),cy=std::cos(yaw*r);const float dir[3]={-sy*cp,-sp,cy*cp};
 if(!p.normalized){for(int i=0;i<3;++i)vel[i]=dir[i]*p.speed;return;}
 float w[3];for(int i=0;i<3;++i)w[i]=motion[i]+p.factor*dir[i];const float l=std::sqrt(w[0]*w[0]+w[1]*w[1]+w[2]*w[2]);if(l>1e-6f)for(int i=0;i<3;++i)vel[i]=w[i]/l*p.speed;else vel[0]=vel[1]=vel[2]=0;}
// Script trace (projectile.trace): flight of a `kind` projectile thrown by the local player from `start` along (yaw,
// pitch), with the type's default (BDS) parameters; mobs are hit as in the drawing.
inline bool TrajectoryTrace(const Context& c,int kind,const float* start,float yaw,float pitch,TrajectoryPath* path){
 uint64_t owner{};if(!Read(c.actor+0x1f0,&owner)||!owner)return false;const uint64_t regionOf=Virtual(c,owner,0x68);if(!regionOf)return false;const uint64_t region=reinterpret_cast<uint64_t(*)(uint64_t)>(regionOf)(owner);if(!region)return false;
 const TrajectoryParams p=TrajectoryBds(kind);float motion[3]{};uint64_t state{};if(Read(c.actor+0x240,&state)&&state)Floats(state+0x18,motion,3);
 float vel[3];TrajectoryVelocity(p,motion,yaw,pitch,vel);std::vector<TrajectoryBody> bodies;TrajectoryBodies(c,c.actor,start,bodies);TrajectorySimulate(c,c.actor,region,p,start,vel,bodies,*path);return path->hit;}
// Lines for one shooter, appended to `out` (world coordinates)
template<class Emit> inline void TrajectoryShooter(const Context& context,uint64_t actor,const TrajectoryParams& p,const float* start,float yaw,float pitch,const float* camera,Emit emit){
 uint64_t owner{};if(!Read(actor+0x1f0,&owner)||!owner)return;const uint64_t regionOf=Virtual(context,owner,0x68);if(!regionOf)return;const uint64_t region=reinterpret_cast<uint64_t(*)(uint64_t)>(regionOf)(owner);if(!region)return;
 std::vector<TrajectoryBody> bodies;TrajectoryBodies(context,actor,start,bodies);
 uint64_t state{};float motion[3]{};if(Read(actor+0x240,&state)&&state)Floats(state+0x18,motion,3);
 const float r=.017453292f,cp=std::cos(pitch*r),sy=std::sin(yaw*r),sp=std::sin(pitch*r),cy=std::cos(yaw*r);const float dir[3]={-sy*cp,-sp,cy*cp};float vel[3];
 if(!p.normalized){for(int i=0;i<3;++i)vel[i]=dir[i]*p.speed;}
 else{float w[3];for(int i=0;i<3;++i)w[i]=motion[i]+p.factor*dir[i];const float l=std::sqrt(w[0]*w[0]+w[1]*w[1]+w[2]*w[2]);if(l>1e-6f)for(int i=0;i<3;++i)vel[i]=w[i]/l*p.speed;else vel[0]=vel[1]=vel[2]=0;}
 TrajectoryPath path;TrajectorySimulate(context,actor,region,p,start,vel,bodies,path);
 if(!path.points.empty()){const float dx=start[0]-camera[0],dy=start[1]-camera[1],dz=start[2]-camera[2];if(std::sqrt(dx*dx+dy*dy+dz*dz)<p.speed)path.points.erase(path.points.begin());}
 emit(path);
}
}
