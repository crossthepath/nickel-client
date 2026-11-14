#pragma once
#include "game_hand.hpp"
#include "game_visual_entities.hpp"
#include "game_trajectory.hpp"
#include "../include/nickel/visual_world.hpp"
#include <memory>
#include <mutex>
#include <vector>

namespace game {
struct WorldLine{float a[3],b[3],colorA[4],colorB[4],thickness;bool depth;};
struct WorldBox{float low[3],high[3],color[4];bool depth;};
struct WorldQuad{float p[4][3],color[4];bool depth;};   // coloured quad (Jump Circles rings)
struct WorldFrame{std::vector<WorldLine> lines;std::vector<WorldBox> boxes;std::vector<WorldQuad> quads;uint64_t tick=0;uintptr_t actor=0;uint32_t worldSerial=0;int dimension=-1;};
// Trajectories: local player, then other players with Other Players
inline void TrajectoryWork(const Context& context,const float* camera,std::vector<WorldLine>* out){
 auto& shared=Trajectory();float color[4],thickness;bool impact,others;AcquireSRWLockShared(&shared.lock);memcpy(color,shared.color,16);thickness=shared.thickness;impact=shared.impact;others=shared.others;ReleaseSRWLockShared(&shared.lock);
 float alpha=1;{uint64_t timer{};float value{};if(context.client&&Read(context.client+0xe8,&timer)&&timer&&Read(timer+8,&value)&&std::isfinite(value))alpha=std::clamp(value,0.f,1.f);}
 auto emit=[&](const TrajectoryPath& path){
  for(size_t i=1;i<path.points.size();++i){WorldLine line{};memcpy(line.a,path.points[i-1].data(),12);memcpy(line.b,path.points[i].data(),12);memcpy(line.colorA,color,16);memcpy(line.colorB,color,16);line.thickness=thickness;line.depth=true;out->push_back(line);}
  if(impact&&path.hit){static const int edges[12][2]={{0,1},{1,3},{3,2},{2,0},{4,5},{5,7},{7,6},{6,4},{0,4},{1,5},{2,6},{3,7}};const float* lo=path.box;const float* hi=path.box+3;
   for(auto& e:edges){WorldLine line{};const int a=e[0],b=e[1];line.a[0]=a&1?hi[0]:lo[0];line.a[1]=a&4?hi[1]:lo[1];line.a[2]=a&2?hi[2]:lo[2];line.b[0]=b&1?hi[0]:lo[0];line.b[1]=b&4?hi[1]:lo[1];line.b[2]=b&2?hi[2]:lo[2];
    memcpy(line.colorA,color,16);memcpy(line.colorB,color,16);line.thickness=thickness;line.depth=true;out->push_back(line);}}};
 {TrajectoryParams p{};if(TrajectoryPrepare(context,context.actor,alpha,&p)){float start[3]{};uint64_t rotation{};float angles[2]{};
   reinterpret_cast<float*(*)(float*,uint64_t,float)>(context.base+0x2ceda40)(start,context.actor,alpha);
   if(Read(context.actor+0x250,&rotation)&&rotation&&Floats(rotation,angles,2))TrajectoryShooter(context,context.actor,p,start,angles[1],angles[0],camera,emit);}}
 if(others){uintptr_t owners{},positions{};uint64_t begin{},end{};if(!Storage(context.registry,Fnv("ActorOwnerComponent"),&owners)||!Storage(context.registry,Fnv("RenderPositionComponent"),&positions)||!Read(owners+0x20,&begin)||!Read(owners+0x28,&end)||end<begin||(end-begin)>4*65536)return;
  for(uint64_t at=begin;at<end;at+=4){uint32_t entity{};uintptr_t slot{},element{};uint64_t actor{},rotation{};if(!Read(at,&entity)||entity>=0xfffc0000u||entity==context.entity)continue;
   if(!Has(context.registry,entity,"PlayerComponent")||!InStorage(owners,entity,8,&slot)||!Read(slot,&actor)||!actor)continue;int32_t type{};if(!Element(context.registry,entity,"ActorTypeComponent",4,&element)||!Read(element,&type)||!(type&0x100))continue;
   TrajectoryParams p{};if(!TrajectoryPrepare(context,actor,alpha,&p))continue;float start[3],lift=0,angles[2];if(!InStorage(positions,entity,12,&element)||!Floats(element,start,3))continue;
   if(Element(context.registry,entity,"OffsetsComponent",0x44,&element))Floats(element,&lift,1);start[1]=start[1]-lift+1.62001f;
   if(!Read(actor+0x250,&rotation)||!rotation||!Floats(rotation,angles,2))continue;TrajectoryShooter(context,actor,p,start,angles[1],angles[0],camera,emit);}}
 InterlockedExchange(&shared.seen,1);
}
inline bool TrajectoryLines(const Context& context,const float* camera,std::vector<WorldLine>& out){__try{TrajectoryWork(context,camera,&out);return true;}__except(EXCEPTION_EXECUTE_HANDLER){return false;}}
struct WorldShared{std::mutex guard;std::shared_ptr<const WorldFrame> frame;volatile long fault=0,seen=0,clipped=0;void(*log)(const char*)=nullptr;};
inline WorldShared& World(){static WorldShared* shared=new WorldShared;return *shared;}
inline bool WorldFinite(const float* value,int count){for(int i=0;i<count;++i)if(!std::isfinite(value[i]))return false;return true;}
inline bool WorldLineValid(const WorldLine& line){return WorldFinite(line.a,3)&&WorldFinite(line.b,3)&&WorldFinite(line.colorA,4)&&WorldFinite(line.colorB,4)&&std::isfinite(line.thickness)&&line.thickness>0;}
inline bool WorldBoxValid(const WorldBox& box){if(!WorldFinite(box.low,3)||!WorldFinite(box.high,3)||!WorldFinite(box.color,4))return false;for(int i=0;i<3;++i)if(box.low[i]>box.high[i])return false;return true;}
inline void SubmitWorld(WorldFrame&& frame){
 auto& shared=World();bool clipped=false;size_t kept=0;
 for(size_t i=0;i<frame.lines.size();++i){if(!WorldLineValid(frame.lines[i]))continue;if(kept<65536)frame.lines[kept++]=frame.lines[i];else clipped=true;}
 frame.lines.resize(kept);kept=0;
 for(size_t i=0;i<frame.boxes.size();++i){if(!WorldBoxValid(frame.boxes[i]))continue;if(kept<4096)frame.boxes[kept++]=frame.boxes[i];else clipped=true;}
 frame.boxes.resize(kept);if(frame.quads.size()>16384){frame.quads.resize(16384);clipped=true;}
 if(clipped&&!shared.clipped&&shared.log){shared.clipped=1;shared.log("World drawing: too many primitives in one frame, the excess was dropped");}
 std::lock_guard<std::mutex> lock(shared.guard);shared.frame=std::make_shared<const WorldFrame>(std::move(frame));
}
inline void WorldWork(void* ctx,void* renderer){
 auto& shared=World();if(shared.fault)return;
 std::shared_ptr<const WorldFrame> frame;{std::lock_guard<std::mutex> lock(shared.guard);frame=shared.frame;}
 auto& paths=Trajectory();const bool trajectories=paths.enabled&&!paths.fault;
 bool useFrame=frame&&!(frame->lines.empty()&&frame->boxes.empty()&&frame->quads.empty());if(!useFrame&&!trajectories)return;Context context{};if(!Local(&context))return;
 uint32_t serial{};{std::lock_guard<std::mutex> lock(Shared().guard);serial=Shared().worldSerial;}
 int dimension{};if(!ReadDimension(&dimension))return;
 if(useFrame&&!nickel::visual::world_frame_fresh(GetTickCount64(),frame->tick,frame->actor,context.actor,frame->worldSerial,serial,frame->dimension,dimension))useFrame=false;
 uint64_t holder{};float origin[3];if(!Read((uintptr_t)renderer+0x480,&holder)||!holder||!Floats(holder+0x658,origin,3))return;
 // Trajectories are computed here, on the LevelRenderPrep thread
 std::vector<WorldLine> extra;
 if(trajectories&&!TrajectoryLines(context,origin,extra)){InterlockedExchange(&paths.fault,1);if(World().log)World().log("Trajectories: a fault was caught while simulating a path; Trajectories is disabled until the game restarts");}
 if(!useFrame&&extra.empty())return;
 Pen pen{};if(!PenOpen(context,(uint64_t)ctx,&pen))return;
 const uint64_t screen=(uint64_t)ctx;
 auto emitLine=[&](const WorldLine& line){float a[3],b[3];for(int i=0;i<3;++i){a[i]=line.a[i]-origin[i];b[i]=line.b[i]-origin[i];}PenSegment(pen,a,b,line.thickness,line.colorA,line.colorB);};
 auto emitBox=[&](const WorldBox& box){static const int pick[8][3]={{0,0,0},{1,0,0},{1,0,1},{0,0,1},{0,1,0},{1,1,0},{1,1,1},{0,1,1}};static const int faces[6][4]={{0,1,2,3},{4,5,6,7},{0,1,5,4},{3,2,6,7},{0,3,7,4},{1,2,6,5}};
  float low[3],high[3];for(int i=0;i<3;++i){low[i]=box.low[i]-origin[i];high[i]=box.high[i]-origin[i];}
  for(int f=0;f<6;++f){float quad[4][3];for(int i=0;i<4;++i){const int corner=faces[f][i];for(int j=0;j<3;++j)quad[i][j]=pick[corner][j]?high[j]:low[j];}PenQuad(pen,box.color,quad);}};
 for(int group=0;group<2;++group){const bool depth=group==0;uint64_t* material=Material(context,depth);if(!material)continue;
  {const WorldBox* batch[256];size_t used=0;auto flush=[&](){if(!used)return;PenBegin(pen,(int)(used*48));for(size_t i=0;i<used;++i)emitBox(*batch[i]);PenFlush(pen,screen,material);used=0;};
   if(useFrame)for(const auto& box:frame->boxes)if(box.depth==depth){batch[used++]=&box;if(used==256)flush();}flush();}
  {const WorldQuad* batch[1024];size_t used=0;auto flush=[&](){if(!used)return;PenBegin(pen,(int)(used*8));for(size_t i=0;i<used;++i){float q[4][3];for(int k=0;k<4;++k)for(int j=0;j<3;++j)q[k][j]=batch[i]->p[k][j]-origin[j];PenQuad(pen,batch[i]->color,q);}PenFlush(pen,screen,material);used=0;};
   if(useFrame)for(const auto& quad:frame->quads)if(quad.depth==depth){batch[used++]=&quad;if(used==1024)flush();}flush();}
  {const WorldLine* batch[2048];size_t used=0;auto flush=[&](){if(!used)return;PenBegin(pen,(int)(used*8));for(size_t i=0;i<used;++i)emitLine(*batch[i]);PenFlush(pen,screen,material);used=0;};
   if(useFrame)for(const auto& line:frame->lines)if(line.depth==depth){batch[used++]=&line;if(used==2048)flush();}
   for(const auto& line:extra)if(line.depth==depth){batch[used++]=&line;if(used==2048)flush();}flush();}
 }
 InterlockedExchange(&shared.seen,1);
}
inline void WorldGuarded(void* ctx,void* renderer){__try{WorldWork(ctx,renderer);}__except(EXCEPTION_EXECUTE_HANDLER){InterlockedExchange(&World().fault,1);static bool told=false;if(!told&&World().log){told=true;World().log("World drawing: a fault was caught in the render preparation pass; world drawing is disabled until the game restarts");}}}
using WorldPrepFunction=void(*)(void*,void*,void*,void*);inline WorldPrepFunction worldPrepOriginal=nullptr;
// 1 while the game's render-prep call (hook #21) runs; read by RenderPlayerModel (Freecam).
inline volatile long levelRenderPrep=0;
// Block ESP listener: rdx = screen context, r8 = LevelRenderer
inline void(*levelPrepListener)(void*,void*)=nullptr;
inline void(*volatile schematicPrep)(void*,void*)=nullptr;   // Schematics ghost blocks (game_schematic.hpp)
// LevelRenderPrepStartEvent: fired at the start of the same game function as LevelRenderPrepEvent (hook #21).
// Used by True Sight.
inline void(*volatile levelPrepStart)()=nullptr;
inline void WorldPrepDetour(void* a,void* b,void* c,void* d){if(auto start=levelPrepStart)start();InterlockedExchange(&levelRenderPrep,1);worldPrepOriginal(a,b,c,d);InterlockedExchange(&levelRenderPrep,0);if(auto esp=levelPrepListener)esp(b,c);if(auto sp=schematicPrep)sp(b,c);WorldGuarded(b,c);}
inline int InstallWorldHook(void(*log)(const char*)){static int state=0;if(state)return state;state=-1;World().log=log;const uint8_t bytes[16]={0x55,0x41,0x57,0x41,0x56,0x41,0x55,0x41,0x54,0x56,0x57,0x53,0x48,0x81,0xec,0x28};state=InstallOne(0x4e09290,bytes,16,(void*)WorldPrepDetour,(void**)&worldPrepOriginal,"World drawing",log);return state;}
inline bool ReadReachLimit(float* range){uintptr_t base{};uint32_t size{};if(!Supported(&base,&size)||0xef3f020+4>size)return false;float value{};if(!Read(base+0xef3f020,&value)||!std::isfinite(value)||value<=0||value>64)return false;*range=value;return true;}
struct ReachFlags{volatile long enabled=0,cancel=0;};
inline ReachFlags& WorldReach(){static ReachFlags* flags=new ReachFlags;return *flags;}
inline bool CancelOutOfReach(uintptr_t local,uintptr_t target){
 auto& reach=WorldReach();float limit=0,distance=0;const bool known=reach.enabled&&reach.cancel&&ReadReachLimit(&limit)&&Reach(local,target,&distance);
 return nickel::visual::cancel_out_of_reach(reach.enabled!=0,reach.cancel!=0,known,distance,limit);
}
}
