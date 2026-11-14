#pragma once
#include <array>
#include <cmath>
#include <cstdint>
namespace nickel::visual {
struct Vec3{float x,y,z;};
inline Vec3 feet_position(Vec3 position,float offset){return {position.x,position.y-offset,position.z};}
inline std::array<Vec3,49> reach_ring(Vec3 feet,float offset,float radius){
 std::array<Vec3,49> ring{};
 for(int i=0;i<49;++i){const double angle=(double)i*6.283185307/48.0;ring[i]={feet.x+(float)(std::cos(angle)*radius),feet.y+offset,feet.z+(float)(std::sin(angle)*radius)};}
 return ring;
}
inline bool cancel_out_of_reach(bool enabled,bool cancel,bool known,float distance,float range){
 if(!enabled||!cancel||!known)return false;
 if(!std::isfinite(distance)||distance<0||!std::isfinite(range)||range<=0)return false;
 return distance>range;
}
inline bool same_world(uintptr_t actor,uintptr_t currentActor,uint32_t serial,uint32_t currentSerial,int dimension,int currentDimension){
 return actor!=0&&actor==currentActor&&serial==currentSerial&&dimension==currentDimension;
}
inline bool world_frame_fresh(uint64_t now,uint64_t tick,uintptr_t actor,uintptr_t currentActor,uint32_t serial,uint32_t currentSerial,int dimension,int currentDimension){
 if(!tick||now<tick||now-tick>250)return false;
 return same_world(actor,currentActor,serial,currentSerial,dimension,currentDimension);
}
}
