// Movement modules on LevelTick / packet receive / packet send:
// - Step: MaxAutoStepComponent (float) = Height; the original value is restored on disable.
// - Spider: horizontal collision (HorizontalCollisionFlagComponent, 2 bytes: [0] != 0 or [1] == 1) plus a
// movement key (bits 13..16 of MoveInput+0x10) -> vy = max(vy, Speed).
// - Flight (LevelTick): airborne only; abilities (0x6de78dc5, 0x10) +4 |= 3; vy = +/-Vertical Speed (jump
// bit 7, sneak bit 0) else 0. Normal: +8 = Horizontal Speed, +0xc = 0.05. Motion: +8 = +0xc = 0,
// horizontal velocity = H x key direction (yaw + 90 deg). Disable: +4 &= ~3.
// - Bunny Hop: nothing when MoveInput+0x60 & 0x40; Hold Jump / Moving Only; jump request on ground. Modify
// Velocity: horizontal velocity = Speed x direction (0 without keys).
// - Velocity: SetActorMotionPacket (0x28) for the local player: Chance; Normal scales by H% / V%; Anchor
// (delay in ticks then vy -= Amount, or y = -Amount without delay); Drop cancels the packet and the next
// NetworkStackLatency (0x73).
#pragma once
#include "game_movement2.hpp"
#include <random>

namespace game {
struct Movement3{std::mutex guard;bool step=false,spider=false,flight=false,bunny=false,velocity=false;float stepHeight=1,spiderSpeed=.2f;int flightMode=0;float flightV=.2f,flightH=.05f;
 bool bunnyModify=true,bunnyHold=false,bunnyMoving=false;float bunnySpeed=.28f;int velMode=0;int anchorDelay=5;float anchorAmount=.1f,velH=100,velV=100;bool chance=false;float chanceAmount=100;
 float savedStep=0;bool stepSaved=false,wasStep=false,wasFlight=false;int anchorTicks=0;bool anchorArmed=false,dropLatency=false;};
inline Movement3& Move3(){static auto* m=new Movement3;return *m;}
inline uintptr_t Move3Input(const Context& c){uintptr_t in{};return Element(c.registry,c.entity,"MoveInputComponent",100,&in)?in:0;}
inline uintptr_t Move3Abilities(const Context& c){uintptr_t storage{},el{};return Storage(c.registry,0x6de78dc5u,&storage)&&InStorage(storage,c.entity,0x10,&el)?el:0;}
inline void Move3Horizontal(const Context& c,uint64_t sv,uint32_t bits,float speed,bool unitWhenStill){const float f=(float)((bits>>13)&1)-(float)((bits>>14)&1),s=(float)((bits>>16)&1)-(float)((bits>>15)&1);
 float n=std::sqrt(f*f+s*s);float pitch{},yaw{};if(!RotRead(c.actor,&pitch,&yaw))return;if(n<=0){if(!unitWhenStill){*(float*)(sv+0x18)=0;*(float*)(sv+0x20)=0;return;}n=1;}
 const float r=(yaw+90.f)*0.017453292f,co=std::cos(r),si=std::sin(r),a=s/n,b=f/n;*(float*)(sv+0x18)=speed*(co*b-si*a);*(float*)(sv+0x20)=speed*(co*a+si*b);}
inline void Move3TickWork(){auto& m=Move3();std::lock_guard<std::mutex> l(m.guard);Context c{};const bool have=Local(&c);
 // Step
 if(m.wasStep&&!m.step){if(have&&m.stepSaved){uintptr_t h{};if(Element(c.registry,c.entity,"MaxAutoStepComponent",4,&h))*(float*)h=m.savedStep;}m.stepSaved=false;}m.wasStep=m.step;
 if(m.wasFlight&&!m.flight&&have)if(uintptr_t ab=Move3Abilities(c))*(uint32_t*)(ab+4)&=~3u;m.wasFlight=m.flight;
 if(!have)return;uint64_t sv{};Read(c.actor+0x240,&sv);
 if(m.step){uintptr_t h{};if(Element(c.registry,c.entity,"MaxAutoStepComponent",4,&h)){if(!m.stepSaved){Read(h,&m.savedStep);m.stepSaved=true;}*(float*)h=m.stepHeight;}}
 if(m.spider&&sv){uintptr_t hc{},in=Move3Input(c);uint8_t a{},b{};uint32_t bits{};if(Element(c.registry,c.entity,"HorizontalCollisionFlagComponent",2,&hc)&&Read(hc,&a)&&Read(hc+1,&b)&&(a||b==1)&&in&&Read(in+0x10,&bits)&&(bits&(0xfu<<13))){
   float vy{};Read(sv+0x1c,&vy);if(vy<m.spiderSpeed)*(float*)(sv+0x1c)=m.spiderSpeed;}}
 if(m.flight&&sv&&!ActionOnGround(c.actor)){const uintptr_t ab=Move3Abilities(c),in=Move3Input(c);uint32_t bits{};if(ab&&in&&Read(in+0x10,&bits)){*(uint32_t*)(ab+4)|=3u;
   *(float*)(sv+0x1c)=(bits&0x80)?m.flightV:((bits&1)?-m.flightV:0.f);
   if(m.flightMode==0){*(float*)(ab+8)=m.flightH;*(float*)(ab+0xc)=.05f;}else{*(float*)(ab+8)=0;*(float*)(ab+0xc)=0;Move3Horizontal(c,sv,bits,m.flightH,false);}}}
 if(m.bunny&&sv){const uintptr_t in=Move3Input(c);uint32_t bits{};uint8_t flags{};if(in&&Read(in+0x10,&bits)&&Read(in+0x60,&flags)&&!(flags&0x40)){
   const bool moving=(bits&(0xfu<<13))!=0;if(!(m.bunnyHold&&!(bits&0x80))&&!(m.bunnyMoving&&!moving)){JumpRequest(c.actor);if(m.bunnyModify)Move3Horizontal(c,sv,bits,m.bunnySpeed,false);}}}
 if(m.anchorArmed&&--m.anchorTicks<=0){m.anchorArmed=false;if(sv){float vy{};Read(sv+0x1c,&vy);*(float*)(sv+0x1c)=vy-m.anchorAmount;}}}
inline void Move3Tick(){static volatile long stopped_=0;if(!stopped_)__try{Move3TickWork();}__except(game::FeatureFault("Move3Tick",&stopped_,GetExceptionInformation())){game::FeatureFlush();}}
inline bool Move3ReceiveWork(uintptr_t packet){auto& m=Move3();std::lock_guard<std::mutex> l(m.guard);if(!m.velocity||PacketIdOf(packet)!=0x28)return false;Context c{};uint64_t mine{},who{};
 if(!Local(&c)||!RuntimeId(c.actor,&mine)||!Read(packet+0x30,&who)||who!=mine)return false;
 if(m.chance){static std::mt19937_64 gen{std::random_device{}()};const int r=(int)(gen()%100);if(m.chanceAmount<=(float)r)return false;}
 if(m.velMode==2){m.dropLatency=true;return true;}
 if(m.velMode==1){if(m.anchorDelay>0){m.anchorTicks=m.anchorDelay;m.anchorArmed=true;}else *(float*)(packet+0x3c)=-m.anchorAmount;return false;}
 *(float*)(packet+0x38)*=m.velH/100.f;*(float*)(packet+0x40)*=m.velH/100.f;*(float*)(packet+0x3c)*=m.velV/100.f;return false;}
inline bool Move3Receive(uintptr_t packet){__try{return Move3ReceiveWork(packet);}__except(EXCEPTION_EXECUTE_HANDLER){return false;}}
inline bool(*volatile moveSendC)(uintptr_t packet)=nullptr;   // Elytra Fly Bounce, Null Movement (game_movement_c.hpp)
inline bool MoveSendAll(uintptr_t packet){Move2Send(packet);if(auto c=moveSendC)c(packet);auto& m=Move3();if(m.dropLatency&&PacketIdOf(packet)==0x73){m.dropLatency=false;return true;}return false;}
}
