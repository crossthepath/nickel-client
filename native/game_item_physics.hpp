// Item Physics: dropped items tumble and settle instead of bobbing.
// ActorRenderEvent (hook #0, game_filter.hpp ActorDetour): for a dropped item (ActorTypeComponent == 0x40,
// actor+0x480 null, RuntimeID, stack at actor+0x3d0 with a live Item) keeps per-item state (yaw / pitch / roll /
// direction, seeded with splitmix64 of the id), spins it in the air or in water and settles it on the ground. The top W
// of the context's world matrix stack is replaced BEFORE rendering by W*T(pos)*P*inverse(B)*T(-pos)
// (B = vanilla bob/spin transform) and restored AFTER.
#pragma once
#include "game_hand.hpp"
#include "game_filter.hpp"
#include <mutex>
#include <unordered_map>

namespace game {
struct PhysicsItem{float yaw,pitch,roll,sense;double seen;};
struct PhysicsShared{std::mutex guard;bool enabled=false,randomYaw=true,blockItems=true;float spin=1,settle=1;std::unordered_map<uint64_t,PhysicsItem> items;double purge=0;volatile long fault=0;};
inline PhysicsShared& Physics(){static auto* s=new PhysicsShared;return *s;}
inline double PhysicsNowMs(){LARGE_INTEGER f{},c{};QueryPerformanceFrequency(&f);QueryPerformanceCounter(&c);return (double)c.QuadPart*1000.0/(double)f.QuadPart;}
inline bool PhysicsHas(uintptr_t registry,uint32_t entity,uint32_t component){uintptr_t storage{};uint32_t packed{};return Storage(registry,component,&storage)&&Packed(storage,entity,&packed);}
inline void PhysicsWork(void* holder,void* actorPtr,void* posPtr,PhysicsToken* token){
 auto& s=Physics();bool randomYaw,blockItems;float spin,settle;{std::lock_guard<std::mutex> l(s.guard);if(!s.enabled)return;randomYaw=s.randomYaw;blockItems=s.blockItems;spin=s.spin;settle=s.settle;}
 const uintptr_t actor=(uintptr_t)actorPtr;if(!QuietContext(holder)||!posPtr)return;uintptr_t registry{},element{};uint32_t entity{};int32_t type{};uint8_t hidden{};uint64_t id{};
 if(!ActorEntity(actor,&registry,&entity)||!Element(registry,entity,"ActorTypeComponent",4,&element)||!Read(element,&type)||type!=0x40)return;
 if(!Read(actor+0x480,&hidden)||hidden||!Element(registry,entity,"RuntimeIDComponent",8,&element)||!Read(element,&id))return;
 const uint64_t stack=actor+0x3d0;uint64_t cb{},item{};uint32_t strong{};if(!Read(stack+8,&cb)||!cb||!Read(cb+8,&strong)||!strong||!Read(cb,&item)||!item)return;
 Context c{};if(!Supported(&c.base,&c.size))return;bool flat=true;uint64_t block=0;
 {const uint64_t f=Virtual(c,item,0x30);if(f)CallGetter(f,item,&block);
  if(block){const int kind=reinterpret_cast<int(*)(uint64_t)>(c.base+0x5757550)(stack);if(kind!=1){if(!blockItems)return;flat=false;}}}
 const double now=PhysicsNowMs();PhysicsItem st{};
 {std::lock_guard<std::mutex> l(s.guard);if(now-s.purge>=1000){s.purge=now;for(auto it=s.items.begin();it!=s.items.end();)if(now-it->second.seen>2000)it=s.items.erase(it);else ++it;}
  auto found=s.items.find(id);if(found==s.items.end()){uint64_t z=id+0x9E3779B97F4A7C15ull;z=(z^(z>>30))*0xBF58476D1CE4E5B9ull;z=(z^(z>>27))*0x94D049BB133111EBull;const uint64_t h=z^(z>>31);
   PhysicsItem n{};n.yaw=randomYaw?(float)(h&0xffff)*1.52587890625e-05f*6.2831855f:0.f;n.sense=(h&0x10000)?1.f:-1.f;n.seen=now;found=s.items.emplace(id,n).first;}
  PhysicsItem& p=found->second;const float dt=(float)(std::min(now-p.seen,100.0)/1000.0);p.seen=now;const float step=flat?3.14159265f:1.5707963f;
  float vit=4;{uintptr_t storage{},sv{};float d[3];if(Storage(registry,0x1b5d5238u,&storage)&&InStorage(storage,entity,0x24,&sv)&&Floats(sv+0x18,d,3))vit=std::sqrt(d[0]*d[0]+d[1]*d[1]+d[2]*d[2])*20*1.5f+4;}
  const bool ground=PhysicsHas(registry,entity,0xc29078a0u),water=PhysicsHas(registry,entity,0x78e89f39u);
  if(ground&&!water){const float f=1-std::exp(settle*-10.f*dt);p.pitch+=f*(std::round(p.pitch/step)*step-p.pitch);p.roll+=f*(std::round(p.roll/step)*step-p.roll);}
  else{const float rate=spin*std::min(20.f,vit)*(water?.15f:1.f);p.pitch=std::fmod(p.sense*rate*dt+p.pitch,6.2831855f);p.roll=std::fmod(rate*.5f*dt+p.roll,6.2831855f);}
  st=p;}
 float height=0;if(Element(registry,entity,"OffsetsComponent",0x44,&element))Floats(element,&height,1);
 const float deg=57.29578f;Mat4 P=Translation(0,(flat?.03f:.125f)-height,0)*Rotation(st.yaw*deg,0,1,0)*Rotation(st.pitch*deg,1,0,0)*Rotation(st.roll*deg,0,0,1);if(flat)P=P*Rotation(90,1,0,0);
 // rebuild the vanilla transform B
 float a=0;Read((uintptr_t)holder+0xb0,&a);int32_t age{};float bobOffs{};Read(actor+0x468,&age);Read(actor+0x474,&bobOffs);uint8_t optOn{},optB{};float extraDeg=0;
 const bool optional=Read(actor+0x48c,&optOn)&&optOn==1&&Read(actor+0x484,&optB)&&optB!=0;if(optional)Read(actor+0x488,&extraDeg);
 uint64_t flags{};const float base=(block&&Read(block+0x130,&flags)&&!(flags&0x100))?.2f:0.f;const float sec=((float)age+a)/20.f;
 const float m=optional?std::clamp(2*sec,0.f,1.f):1.f,bob=std::sin(2*sec+m*bobOffs)*.1f+.1f;float yb=base+bob,extra=0;
 if(optional){const float t=std::clamp(sec/.4f,0.f,1.f);yb=base+bob+(1-t*t*t)*.4f;extra=extraDeg;}
 const float theta=(std::max(0.f,sec-.025f)*57.29578f+extra);Mat4 B=Translation(0,yb,0)*Rotation(theta,0,1,0),Binv{};if(!Inverse(B,&Binv))return;
 float pos[3];if(!Floats((uintptr_t)posPtr,pos,3))return;
 uint64_t ctx{},camera{},top{};if(!Read((uintptr_t)holder+0x28,&ctx)||!ctx||!Read(ctx+0x18,&camera)||!camera||!StackLevel(camera+0x40,0,&top)||!top)return;
 Mat4 W{};if(!Floats(top,W.m,16))return;const Mat4 next=W*Translation(pos[0],pos[1],pos[2])*P*Binv*Translation(-pos[0],-pos[1],-pos[2]);
 token->w=top;memcpy(token->saved,W.m,64);memcpy((void*)top,next.m,64);token->on=true;}
inline void PhysicsPre(void* holder,void* actor,void* pos,PhysicsToken* token){if(Physics().fault)return;__try{PhysicsWork(holder,actor,pos,token);}__except(EXCEPTION_EXECUTE_HANDLER){InterlockedExchange(&Physics().fault,1);token->on=false;}}
inline void PhysicsPost(PhysicsToken* token){if(!token->on||!token->w)return;__try{memcpy((void*)token->w,token->saved,64);}__except(EXCEPTION_EXECUTE_HANDLER){}}
}
