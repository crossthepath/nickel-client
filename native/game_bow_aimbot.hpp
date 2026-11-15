// Bow Aimbot (Combat): while a bow is being drawn, aims it at the best target through the rotation manager.
// - GameUpdate: the "valid" flag is cleared first. Needs a local player with external input data and a bow in the
//   selected hotbar slot that is being drawn (use duration > 0, power > 0: speed = power, gravity .05, drag .99 / .6,
//   old velocity moves first). The launch origin is the eye position at frame fraction 1 (game 0x2ceda40).
// - Candidates: every actor of the ActorOwnerComponent view walked from the end that passes the common target filter
//   (Players Only, Anti Bot, Teams, local player skipped) and lies within Distance (squared, position states +0x240).
//   Aim point = centre of the box *(actor+0x248). The view angle uses the camera: renderer = ClientInstance vtable
//   +0x5c8 (camera position *(*(renderer+0x480)+0x658)), camera = vtable +0x5c0, look matrix at camera+0x3c0 (column
//   major). d = point (the render position when the actor has one: RenderPosition - Offsets[0] + box height / 2,
//   otherwise the box centre) - camera position; x' = m0 dx + m4 dy + m8 dz + m12, y' = m1 dx + m5 dy + m9 dz + m13,
//   z' = m2 dx + m6 dy + m10 dz + m14; angle = atan2(hypot(x', y'), -z') in degrees. A candidate needs angle < FOV and,
//   in Distance mode, is ranked by the distance origin -> box centre (Angle mode: by the angle). The six best are kept
//   in a sorted array (upper-bound insertion, ties keep the earlier one).
// - Launch solve (per candidate, best first): delta = box centre - origin; for up to 200 ticks the required launch
//   speed is |(dx, dy + B, dz) / A| (A, B = sums of the drag powers and of the gravity terms of the tick model, B
//   starting at gravity and A at drag when the new velocity moves first); the first tick count within the bow speed
//   gets a 20 step bisection of the fractional last tick, and the velocity is the unit direction x speed.
// - Check: the flight is simulated with the Trajectories model (200 steps, clip, living boxes inflated by 0.3) and the
//   candidate is kept only if the first entity hit is that actor. yaw = atan2(-vx, vz) deg, pitch = -atan2(vy,
//   hypot(vx, vz)) deg. The first candidate that hits wins: angles, its runtime id and the valid flag are stored.
// - Request: absolute {yaw, pitch}, priority 1, mode 2 with Silent else mode 0 with smoothing 14, kept while the valid
//   flag holds, a bow is drawn and the target actor still exists.
// - Target Indicator (ScreenViewRender): with the valid flag, a target id and a drawn bow, the target render point is
//   published; four corner brackets (arms 9..16, outline 4 px rgba 25,18,36,190 (main colour: Nickel orange) then 2 px rgb 180,123,255) rotate
//   around it at 0.6 rad/s counted from the enable time. BowIndicatorLines() builds the 16 segments for the overlay.
// Enabling or disabling restarts the timer and clears the valid flag.
#pragma once
#include "game_combat.hpp"
#include "game_rotation.hpp"
#include "game_sticky_aim.hpp"
#include "game_trajectory.hpp"
#include "game_utility_packets.hpp"
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <mutex>
#include <vector>

namespace game {
constexpr int kRotBowAimbot=15;
struct BowAimbotConfig{int targetMode=0;float fov=40,distance=64;bool playersOnly=true,silent=false,indicator=true;};
// Aim state kept between updates: angles, runtime id (hasId = the id component was present), valid flag, enable time.
struct BowAimbotState{float yaw=0,pitch=0;uint64_t id=0;bool hasId=false,valid=false;int64_t startNs=0;};
// Indicator data for the overlay: target render point, seconds since the enable time, GUI scale, tick of publication.
struct BowIndicator{bool valid=false;float world[3]{};double seconds=0;float scale=1;uint64_t tick=0;};
struct BowAimbotShared{std::mutex guard;std::atomic<bool> enabled{false};BowAimbotConfig cfg;BowAimbotState st;
 std::mutex drawGuard;BowIndicator indicator;volatile long fault=0,screenFault=0;};
inline BowAimbotShared& BowAimbot(){static auto* s=new BowAimbotShared;return *s;}
// Enabling and disabling both run the same handler: valid flag cleared, timer restarted.
inline void BowAimbotActivate(){auto& s=BowAimbot();{std::lock_guard<std::mutex> lock(s.guard);s.st.valid=false;s.st.startNs=CombatNs();}
 {std::lock_guard<std::mutex> lock(s.drawGuard);s.indicator=BowIndicator{};}RotRelease(kRotBowAimbot);}

// Bow in the selected slot being drawn: out = flight parameters (speed = bow power); false for anything else.
inline bool BowPrepare(const Context& c,uint64_t actor,float alpha,TrajectoryParams* out){
 uint64_t supplies{},inventory{};int32_t selected{};
 if(!Read(actor+0x5e0,&supplies)||!supplies||!Read(supplies+0x10,&selected)||selected<0||selected>40||!Read(supplies+0xb8,&inventory)||!inventory)return false;
 const uint64_t get=Virtual(c,inventory,0x38);if(!get)return false;
 const uint64_t stack=reinterpret_cast<uint64_t(*)(uint64_t,int)>(get)(inventory,selected);if(!ValidStack(stack))return false;
 const uint64_t item=ItemOf(stack);char full[64]{};if(!item||!Text(item+0xd8,full,sizeof(full)))return false;
 const char* n=!strncmp(full,"minecraft:",10)?full+10:full;if(strcmp(n,"bow"))return false;
 const uint64_t useOf=Virtual(c,actor,0x498);const int duration=useOf?reinterpret_cast<int(*)(uint64_t)>(useOf)(actor):0;if(duration<=0)return false;
 const uint64_t powerOf=Virtual(c,item,0x28);if(!powerOf)return false;
 const float power=reinterpret_cast<float(*)(uint64_t,uint64_t,int,float)>(powerOf)(item,stack,duration,alpha);
 TrajectoryParams params=TrajectoryBds(0);params.speed=power/TrajectoryBds(0).speed*params.speed;if(!(params.speed>0))return false;
 *out=params;return true;}

// Launch velocity for a target offset (see the header). false when no tick count within 200 reaches it.
inline bool BowSolve(const TrajectoryParams& p,const float* d,float* out){
 const float speed=p.speed;if(!(speed>0)||!std::isfinite(speed))return false;
 const float x=d[0],y=d[1],z=d[2];if(!(1e-5f<std::sqrt(z*z+x*x+y*y)))return false;
 const float drag=p.air,gravity=p.gravity;
 float a=p.order?gravity:0.0f,b=p.order?drag:1.0f,sumA=0.0f,sumB=0.0f;const float fz=z+0.0f,fx=x+0.0f;
 for(int tick=0;tick<200;++tick){
  const float B=sumB+a,A=sumA+b;const float ny=(y+B)/A,nx=fx/A,nz=fz/A;
  if(std::sqrt(nx*nx+ny*ny+nz*nz)<=speed){
   float lo=0.0f,hi=1.0f;
   for(int k=0;k<20;++k){const float mid=(lo+hi)*0.5f;const float A2=mid*b+sumA;const float y2=(y+mid*a+sumB)/A2,x2=fx/A2,z2=fz/A2;const float len=std::sqrt(x2*x2+y2*y2+z2*z2);
    if(len<=speed)hi=mid;if(speed<len)lo=mid;}
   const float A3=sumA+b*hi;const float rz=fz/A3,rx=fx/A3,ry=(sumB+a*hi+y)/A3;const float inv=1.0f/std::sqrt(ry*ry+rx*rx+rz*rz);
   out[0]=speed*inv*rx;out[1]=speed*inv*ry;out[2]=rz*inv*speed;return true;}
  b=drag*b;a=a*drag+gravity;sumA=A;sumB=B;}
 return false;}

// Camera: position and the first 15 floats of the look matrix.
struct BowCameraData{float origin[3];float m[15];};
inline bool BowCamera(const Context& c,BowCameraData* out){
 const uint64_t rendererOf=Virtual(c,c.client,0x5c8),cameraOf=Virtual(c,c.client,0x5c0);if(!rendererOf||!cameraOf)return false;
 const uint64_t renderer=reinterpret_cast<uint64_t(*)(uint64_t)>(rendererOf)(c.client);uint64_t holder{};if(!renderer||!Read(renderer+0x480,&holder)||!holder)return false;
 const uint64_t camera=reinterpret_cast<uint64_t(*)(uint64_t)>(cameraOf)(c.client);
 return camera&&Floats(holder+0x658,out->origin,3)&&Floats(camera+0x3c0,out->m,15);}
// Angle (degrees) between the camera axis and the point.
inline float BowViewAngle(const BowCameraData& cam,const float* point){
 const float dx=point[0]-cam.origin[0],dy=point[1]-cam.origin[1],dz=point[2]-cam.origin[2];const float* m=cam.m;
 const float x=m[8]*dz+m[12]+m[4]*dy+dx*m[0],y=dz*m[9]+m[13]+dy*m[5]+m[1]*dx,z=dz*m[10]+m[14]+dy*m[6]+m[2]*dx;
 const float h=(float)std::sqrt((double)y*(double)y+(double)x*(double)x);
 return (float)std::atan2((double)h,(double)-z)*57.295780181884766f;}
// Box centre of the actor.
inline bool BowCenter(uintptr_t actor,float* out){uint64_t bb{};float b[6];if(!Read(actor+0x248,&bb)||!bb||!Floats(bb,b,6))return false;
 out[0]=(b[0]+b[3])*0.5f;out[1]=(b[1]+b[4])*0.5f;out[2]=(b[5]+b[2])*0.5f;return true;}
// Render point: RenderPosition (y minus Offsets[0]) with half the box height; false without the component.
inline bool BowRenderPoint(uintptr_t actor,float* out){
 uintptr_t registry{},render{},offsets{};uint32_t entity{};if(!ActorEntity(actor,&registry,&entity)||!Element(registry,entity,"RenderPositionComponent",12,&render))return false;
 float p[3];if(!Floats(render,p,3))return false;float y=p[1];float lift=0;if(Element(registry,entity,"OffsetsComponent",0x44,&offsets)&&Read(offsets,&lift))y=y-lift;
 uint64_t bb{};float b[6];if(!Read(actor+0x248,&bb)||!bb||!Floats(bb,b,6))return false;
 const float hx=(b[3]-b[0])*0.5f,hz=(b[5]-b[2])*0.5f;
 out[0]=((p[0]-hx)+hx+p[0]+0.0f)*0.5f;out[1]=(y+(b[4]-b[1])+y+0.0f)*0.5f;out[2]=(hz+p[2]+0.0f+(p[2]-hz))*0.5f;return true;}

struct BowCandidate{uint64_t actor=0;float point[3]{};float score=INFINITY;};
// Flight simulation; returns the first entity hit (0 for a block or nothing).
inline uint64_t BowSimulate(const Context& c,uint64_t region,const TrajectoryParams& p,const float* start,const float* velocity,const std::vector<TrajectoryBody>& bodies){
 for(int i=0;i<3;++i)if(!std::isfinite(start[i])||!std::isfinite(velocity[i]))return 0;
 const uint64_t clipOf=Virtual(c,region,0x198),blockOf=Virtual(c,region,0x28);if(!clipOf||!blockOf)return 0;
 using Clip=void*(*)(uint64_t,void*,const float*,const float*,bool,bool,int,bool,bool,uint64_t,const std::function<bool()>*,bool);
 static const std::function<bool()> accept=[]{return true;};
 float pos[3]={start[0],start[1],start[2]},v[3]={velocity[0],velocity[1],velocity[2]},travelled=0;
 for(int step=0;step<200&&travelled<256.f;++step){
  int cell[3]={(int)std::floor(pos[0]),(int)std::floor(pos[1]),(int)std::floor(pos[2])};const uint64_t block=reinterpret_cast<uint64_t(*)(uint64_t,int*)>(blockOf)(region,cell);
  uint64_t legacy{};uint8_t material{};const bool water=block&&Read(block+0x68,&legacy)&&legacy&&Read(legacy+0x28,&material)&&material==5;const float f=water?p.water:p.air;
  const float nv[3]={v[0]*f,v[1]*f-p.gravity,v[2]*f};const float d[3]={p.order?nv[0]:v[0],p.order?nv[1]:v[1],p.order?nv[2]:v[2]};v[0]=nv[0];v[1]=nv[1];v[2]=nv[2];
  const float length=std::sqrt(d[0]*d[0]+d[1]*d[1]+d[2]*d[2]);if(length<=1e-6f)continue;const float scale=std::min(1.f,(256.f-travelled)/length);
  const float to[3]={pos[0]+d[0]*scale,pos[1]+d[1]*scale,pos[2]+d[2]*scale};const float span=std::sqrt((to[0]-pos[0])*(to[0]-pos[0])+(to[1]-pos[1])*(to[1]-pos[1])+(to[2]-pos[2])*(to[2]-pos[2]));
  alignas(16) uint8_t result[0x200]{};reinterpret_cast<Clip>(clipOf)(region,result,pos,to,false,true,(int)std::floor(span)+1,false,false,c.actor,&accept,false);
  uint64_t weak{};memcpy(&weak,result+0x40,8);if(weak){if(InterlockedDecrement((volatile long*)(weak+0xc))==0){uint64_t table{},release{};if(Read(weak,&table)&&Read(table+8,&release)&&release)reinterpret_cast<void(*)(uint64_t)>(release)(weak);}}
  int type{};memcpy(&type,result+0x18,4);bool any=false;float best=0;uint64_t hitActor=0;
  if(type==0){float hp[3];memcpy(hp,result+0x2c,12);const float a=std::sqrt((hp[0]-pos[0])*(hp[0]-pos[0])+(hp[1]-pos[1])*(hp[1]-pos[1])+(hp[2]-pos[2])*(hp[2]-pos[2]));best=span>0?a/span:0;any=true;}
  for(const auto& body:bodies){float t{};if(TrajectoryRay(body.box,pos,to,&t)&&(!any||t<best)){best=t;any=true;hitActor=body.actor;}}
  if(any)return hitActor;
  pos[0]=to[0];pos[1]=to[1];pos[2]=to[2];travelled+=span;}
 return 0;}

// GameUpdate: picks the target, solves and checks the shot, stores the aim and submits the rotation request.
inline void BowAimbotWork(){
 auto& s=BowAimbot();if(!s.enabled)return;BowAimbotConfig k;{std::lock_guard<std::mutex> lock(s.guard);k=s.cfg;s.st.valid=false;}
 Context c{};if(!Local(&c)||!StickyHasInput(c))return;
 TrajectoryParams p{};if(!BowPrepare(c,c.actor,1.0f,&p))return;
 float start[4]{};reinterpret_cast<float*(*)(float*,uint64_t,float)>(c.base+Rva(0x2ceda40))(start,c.actor,1.0f);
 for(int i=0;i<3;++i)if(!std::isfinite(start[i]))return;
 BowCameraData cam{};if(!BowCamera(c,&cam))return;
 uintptr_t owners{};uint64_t begin{},end{};
 if(!Storage(c.registry,Fnv("ActorOwnerComponent"),&owners)||!Read(owners+0x20,&begin)||!Read(owners+0x28,&end)||end<begin||(end-begin)%4||(end-begin)>4*65536)return;
 std::array<BowCandidate,6> list{};const float limit=k.distance*k.distance;float own[3];if(!ActorPos(c.actor,own))return;
 for(uint64_t at=end;at>begin;){at-=4;uint32_t entity{};uintptr_t slot{};uint64_t actorPtr{};if(!Read(at,&entity))break;if(entity>=0xfffc0000u)continue;
  if(!InStorage(owners,entity,8,&slot)||!Read(slot,&actorPtr)||!actorPtr)continue;const uintptr_t actor=(uintptr_t)actorPtr;
  if(actor==c.actor||!CombatTargetValid(c,actor,k.playersOnly))continue;
  float pos[3];if(!ActorPos(actor,pos))continue;if(!((pos[2]-own[2])*(pos[2]-own[2])+(pos[1]-own[1])*(pos[1]-own[1])+(pos[0]-own[0])*(pos[0]-own[0])<=limit))continue;
  float center[3];if(!BowCenter(actor,center))continue;float seen[3];if(!BowRenderPoint(actor,seen)){seen[0]=center[0];seen[1]=center[1];seen[2]=center[2];}
  float score=BowViewAngle(cam,seen);if(!(score<k.fov&&std::isfinite(score)))continue;
  if(k.targetMode==1){score=std::sqrt((center[0]-start[0])*(center[0]-start[0])+(center[1]-start[1])*(center[1]-start[1])+(center[2]-start[2])*(center[2]-start[2]));if(!std::isfinite(score))continue;}
  int index=0;while(index<6&&list[index].score<=score)++index;if(index>=6)continue;
  for(int i=5;i>index;--i)list[i]=list[i-1];list[index].actor=actor;memcpy(list[index].point,center,12);list[index].score=score;}
 if(!list[0].actor)return;
 const uint64_t region=ActorRegion(c);if(!region)return;
 std::vector<TrajectoryBody> bodies;TrajectoryBodies(c,c.actor,start,bodies);std::reverse(bodies.begin(),bodies.end());   // the game lists the bodies from the end of the view, which decides ties
 bool found=false;float yaw=0,pitch=0;uint64_t id=0;bool hasId=false;
 for(const auto& candidate:list){if(!candidate.actor)break;
  const float delta[3]={candidate.point[0]-start[0],candidate.point[1]-start[1],candidate.point[2]-start[2]};float velocity[3];if(!BowSolve(p,delta,velocity))continue;
  if(BowSimulate(c,region,p,start,velocity,bodies)!=candidate.actor)continue;
  const float planar=(float)std::sqrt((double)velocity[0]*(double)velocity[0]+(double)velocity[2]*(double)velocity[2]);
  yaw=(float)std::atan2((double)-velocity[0],(double)velocity[2])*57.295780181884766f;pitch=(float)std::atan2((double)velocity[1],(double)planar)*-57.295780181884766f;
  hasId=RuntimeId(candidate.actor,&id);found=true;break;}
 {std::lock_guard<std::mutex> lock(s.guard);if(found){s.st.yaw=yaw;s.st.pitch=pitch;s.st.id=id;s.st.hasId=hasId;}s.st.valid=found;}
 if(!found)return;
 RotRequest q;q.module=kRotBowAimbot;q.yaw=yaw;q.pitch=pitch;q.priority=1;q.mode=k.silent?2:0;q.relative=false;q.yawOnly=false;q.speed=k.silent?0.0f:14.0f;
 q.predicate=[](uintptr_t actor){auto& b=BowAimbot();BowAimbotState st;{std::lock_guard<std::mutex> lock(b.guard);st=b.st;}if(!st.valid)return false;
  Context ctx{};if(!Local(&ctx))return false;TrajectoryParams params{};if(!BowPrepare(ctx,actor,1.0f,&params))return false;
  if(st.hasId){uintptr_t target{};if(!ActorByRuntimeId(ctx.registry,st.id,&target)||!target)return false;}return true;};   // still wanted
 RotSubmit(std::move(q));}
inline void BowAimbotTick(){auto& s=BowAimbot();if(s.fault)return;__try{BowAimbotWork();}__except(EXCEPTION_EXECUTE_HANDLER){InterlockedExchange(&s.fault,1);}}

// ScreenViewRender: publishes the indicator point while the aim is valid and a bow is drawn.
inline void BowAimbotScreenWork(){
 auto& s=BowAimbot();BowIndicator out{};bool want=false;BowAimbotConfig k;BowAimbotState st;
 if(s.enabled){std::lock_guard<std::mutex> lock(s.guard);k=s.cfg;st=s.st;want=k.indicator&&st.valid&&st.hasId;}
 Context c{};TrajectoryParams p{};uintptr_t target{};float point[3];
 if(want&&Local(&c)&&StickyHasInput(c)&&BowPrepare(c,c.actor,1.0f,&p)&&ActorByRuntimeId(c.registry,st.id,&target)&&target&&BowRenderPoint(target,point)){
  out.valid=true;memcpy(out.world,point,12);out.seconds=(double)(CombatNs()-st.startNs)/1000000.0/1000.0;out.tick=GetTickCount64();
  uint64_t gui{};float size[6];out.scale=1;if(Read(c.client+0x668,&gui)&&gui&&Floats(gui+0x40,size,6)&&size[0]>0&&size[4]>0)out.scale=size[0]/size[4];}   // pixels per GUI unit
 std::lock_guard<std::mutex> lock(s.drawGuard);s.indicator=out;}
inline void BowAimbotScreen(){auto& s=BowAimbot();if(s.screenFault)return;__try{BowAimbotScreenWork();}__except(EXCEPTION_EXECUTE_HANDLER){InterlockedExchange(&s.screenFault,1);}}

// ---- overlay side: segments of the indicator (colour = ImGui IM_COL32 order, thickness in pixels)
struct BowLine{float x0,y0,x1,y1,thickness;uint32_t color;};
constexpr uint32_t BowColor(int r,int g,int b,int a){return ((uint32_t)a<<24)|((uint32_t)b<<16)|((uint32_t)g<<8)|(uint32_t)r;}
inline bool BowIndicatorSnapshot(BowIndicator* out){auto& s=BowAimbot();std::lock_guard<std::mutex> lock(s.drawGuard);*out=s.indicator;return out->valid;}
// m = column-major view-projection (game::View().m), origin = camera position, W / H = screen size in pixels.
// Returns 16 segments (outline 4 px, then main 2 px, per bracket arm) or 0 when the point is not on screen.
inline int BowIndicatorLines(const BowIndicator& in,const float* m,const float* origin,float W,float H,BowLine* out){
 if(!in.valid||!m||!origin||!(W>0)||!(H>0))return 0;
 const float dx=in.world[0]-origin[0],dy=in.world[1]-origin[1],dz=in.world[2]-origin[2];
 const float w=m[11]*dz+m[15]+m[7]*dy+m[3]*dx;if(!(0.001f<w))return 0;
 const float nx=(dz*m[8]+m[12]+dy*m[4]+dx*m[0])/w,ny=(dz*m[9]+m[13]+dy*m[5]+dx*m[1])/w;if(!std::isfinite(nx)||!std::isfinite(ny))return 0;
 const float cx=W*0.5f+nx*(W*0.5f),cy=H*0.5f-ny*(H*0.5f);
 const double angle=in.seconds*0.6;const float sn=(float)std::sin(angle),cs=(float)std::cos(angle),k=in.scale;
 const float a16=16.f*k,a9=9.f*k;const float f16=-a16*sn,f17=-a16*cs,f18=-a9*sn,f19=-a9*cs,f12=a16*sn,f13=a16*cs,f14=a9*sn,f15=a9*cs;
 const uint32_t outline=BowColor(25,18,36,190),main=BowColor(255,138,61,255);int n=0;
 const float sides[2]={-1.f,1.f};
 for(float side:sides){
  const float e22=side*a16,e20=side*a9,e21=e20*cs,e20s=e20*sn;
  const float x1=(e21-f16)+cx,y1=e20s+f17+cy,x2=(e22*cs-f16)+cx,y2=e22*sn+f17+cy,x3=(e22*cs-f18)+cx,y3=e22*sn+f19+cy;
  const float u1=(e21-f12)+cx,v1=e20s+f13+cy,u2=(e22*cs-f12)+cx,v2=e22*sn+f13+cy,u3=(e22*cs-f14)+cx,v3=e22*sn+f15+cy;
  out[n++]={x1,y1,x2,y2,4.f*k,outline};out[n++]={x2,y2,x3,y3,4.f*k,outline};out[n++]={x1,y1,x2,y2,2.f*k,main};out[n++]={x2,y2,x3,y3,2.f*k,main};
  out[n++]={u1,v1,u2,v2,4.f*k,outline};out[n++]={u2,v2,u3,v3,4.f*k,outline};out[n++]={u1,v1,u2,v2,2.f*k,main};out[n++]={u2,v2,u3,v3,2.f*k,main};}
 return n;}
}
