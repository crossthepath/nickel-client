// Movement modules. Strafe:
// - SprintIntentEvent #19: +0x40 input (move vector x +0x24, z +0x28), +0x58 movement state (= actor+0x240,
// velocity x +0x18, z +0x20), +0x60 flags (bits 2 and 32 tested).
// Conditions: local player; Mode Always, or Air and not on the ground (OnGroundFlag 0xc29078a0); flag bits 2 and 32
// clear; not flying (0x6de78dc5, bit 0 of +4), not in water (0x78e89f39) or lava (0x832a2768); not during Knockback
// Pause after a SetActorMotion (0x28) targeting the player.
// No input: Stop Without Input reduces horizontal speed by Acceleration per tick.
// With input: wanted direction = input rotated by yaw (cos/sin of yaw+90); speed = current (Keep, nothing if zero)
// or Speed (Fixed), change clamped to +/-Acceleration.
// - Disable clears the knockback timestamp.
// Yaw: actor rotation (*(actor+0x250)+4, degrees), the same field as Free Look / Debug Overlay.
#pragma once
#include "game_events.hpp"
#include <atomic>
#include <mutex>

namespace game {
struct StrafeShared{std::mutex guard;bool enabled=false,always=false,fixed=false,stop=false;float acceleration=.05f,speed=.28f;int pause=500;
 bool knocked=false;uint64_t knockedNs=0;};
inline StrafeShared& Strafe(){static auto* s=new StrafeShared;return *s;}
inline uint64_t MoveNowNs(){LARGE_INTEGER f{},c{};QueryPerformanceFrequency(&f);QueryPerformanceCounter(&c);return (uint64_t)((double)c.QuadPart*1e9/(double)f.QuadPart);}
inline bool LocalHas(const Context& c,uint32_t component){uintptr_t storage{};uint32_t packed{};return Storage(c.registry,component,&storage)&&Packed(storage,c.entity,&packed);}
inline bool LocalFlying(const Context& c){uintptr_t storage{},element{};uint32_t word{};return Storage(c.registry,0x6de78dc5u,&storage)&&InStorage(storage,c.entity,0x10,&element)&&Read(element+4,&word)&&(word&1);}
inline void StrafeWork(uint64_t input,uint64_t state,uint64_t flags){
 auto& s=Strafe();bool always,fixed,stop,knocked;float accel,speed;int pause;uint64_t at;
 {std::lock_guard<std::mutex> lock(s.guard);if(!s.enabled)return;always=s.always;fixed=s.fixed;stop=s.stop;accel=s.acceleration;speed=s.speed;pause=s.pause;knocked=s.knocked;at=s.knockedNs;}
 Context c{};uint64_t mine{};if(!input||!state||!flags||!Local(&c)||!Read(c.actor+0x240,&mine)||mine!=state)return;
 if(!always&&LocalHas(c,0xc29078a0u))return;
 uint64_t bits{};if(!Read(flags,&bits)||((bits>>2)&1)||((bits>>32)&1))return;
 if(LocalFlying(c)||LocalHas(c,0x78e89f39u)||LocalHas(c,0x832a2768u))return;
 if(knocked&&(double)(MoveNowNs()-at)/1e6<=(double)pause)return;
 float* v=reinterpret_cast<float*>(state+0x18);float* z=reinterpret_cast<float*>(state+0x20);const float vx=*v,vz=*z;
 float inX{},inZ{};if(!Read(input+0x24,&inX)||!Read(input+0x28,&inZ))return;const float eps=1e-4f;const float wish=std::sqrt(inX*inX+inZ*inZ),current=std::sqrt(vx*vx+vz*vz);
 if(wish<=eps){if(!stop||current<=eps)return;const float drop=std::fmin(accel,std::fmax(-accel,-current)),next=current+drop;*v=vx/current*next;*z=vz/current*next;return;}
 if(current<=eps&&!fixed)return;
 uint64_t rotation{};float yaw{};if(!Read(c.actor+0x250,&rotation)||!rotation||!Read(rotation+4,&yaw)||!std::isfinite(yaw))return;
 const float r=(yaw+90)*.0174532925f,f19=std::cos(r),f20=std::sin(r),a0=-inX/wish,a1=inZ/wish,dz=f19*a0+f20*a1,dx=f19*a1-f20*a0;
 const float target=fixed?speed:current;float change=std::fmax(-accel,target-current);change=std::fmin(accel,change);const float next=current+change;
 *v=dx*next;*z=dz*next;}
inline void StrafeSprint(uint64_t input,uint64_t state,uint64_t flags){__try{StrafeWork(input,state,flags);}__except(EXCEPTION_EXECUTE_HANDLER){}}
// PacketReceive: SetActorMotion targeting the local player -> knockback timestamp.
inline void StrafeReceiveWork(uintptr_t packet,int id){if(id!=0x28)return;auto& s=Strafe();{std::lock_guard<std::mutex> lock(s.guard);if(!s.enabled)return;}
 uint64_t target{},mine{};if(!Read(packet+0x30,&target)||!LocalIdentity(&mine,nullptr,0)||target!=mine)return;std::lock_guard<std::mutex> lock(s.guard);s.knocked=true;s.knockedNs=MoveNowNs();}
inline void MovementSprint(uint64_t input,uint64_t state,uint64_t flags){StrafeSprint(input,state,flags);}
inline void MovementReceive(uintptr_t packet,int id){__try{StrafeReceiveWork(packet,id);}__except(EXCEPTION_EXECUTE_HANDLER){}}
}
