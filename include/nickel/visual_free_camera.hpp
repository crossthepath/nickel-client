#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
namespace nickel::visual {
// Free camera math: per-tick movement, turn delta, and per-camera-update smoothing and quaternion.
struct CameraVector {float x=0,y=0,z=0;};
struct CameraSettings {float horizontal=1,vertical=1,smoothing=.02f;CameraVector offset{};};
struct CameraInput {bool forward=false,backward=false,left=false,right=false,up=false,down=false;};
struct FreeCamera {CameraVector target{},position{};float yaw=0,pitch=0;uintptr_t actor=0;uint32_t world=0;int dimension=0;bool initialized=false;};
inline bool camera_finite(CameraVector v){return std::isfinite(v.x)&&std::isfinite(v.y)&&std::isfinite(v.z);}
inline bool camera_settings_valid(const CameraSettings& s){return std::isfinite(s.horizontal)&&s.horizontal>=.1f&&s.horizontal<=5&&std::isfinite(s.vertical)&&s.vertical>=.1f&&s.vertical<=5&&std::isfinite(s.smoothing)&&s.smoothing>=.02f&&s.smoothing<=1&&camera_finite(s.offset)&&std::fabs(s.offset.x)<=100&&std::fabs(s.offset.y)<=100&&std::fabs(s.offset.z)<=100;}
inline float camera_wrap(float angle){if(!std::isfinite(angle))return 0;return std::remainder(angle,360.f);}
inline bool camera_initialize(FreeCamera& state,uintptr_t actor,uint32_t world,int dimension,CameraVector position,float yaw,float pitch,const CameraSettings& settings){
 if(!actor||!camera_finite(position)||!std::isfinite(yaw)||!std::isfinite(pitch)||!camera_settings_valid(settings)){state={};return false;}
 FreeCamera next{};next.actor=actor;next.world=world;next.dimension=dimension;next.target=next.position={position.x+settings.offset.x,position.y+settings.offset.y,position.z+settings.offset.z};next.yaw=camera_wrap(yaw);next.pitch=std::clamp(pitch,-89.9f,89.9f);next.initialized=true;state=next;return true;
}
inline bool camera_same_world(const FreeCamera& state,uintptr_t actor,uint32_t world,int dimension){return state.initialized&&actor!=0&&state.actor==actor&&state.world==world&&state.dimension==dimension;}
inline bool camera_turn(FreeCamera& state,float yawDelta,float pitchDelta){
 if(!state.initialized||!std::isfinite(yawDelta)||!std::isfinite(pitchDelta))return false;
 const double yaw=(double)state.yaw+yawDelta,pitch=(double)state.pitch+pitchDelta;state.yaw=(float)std::remainder(yaw,360.0);state.pitch=(float)std::clamp(pitch,-89.9,89.9);return true;
}
inline bool camera_move(FreeCamera& state,const CameraInput& input,const CameraSettings& settings){
 if(!state.initialized||!camera_settings_valid(settings))return false;
 const double radians=(double)state.yaw*3.14159265358979323846/180;const double sn=std::sin(radians),cs=std::cos(radians);
 const int f=(int)input.forward-(int)input.backward,l=(int)input.left-(int)input.right;
 double x=-sn*f+cs*l,z=cs*f+sn*l;const double length=std::hypot(x,z);if(length>0){x=x/length*settings.horizontal;z=z/length*settings.horizontal;}
 // Up wins over down when both are held.
 const double y=input.up?settings.vertical:input.down?-settings.vertical:0;
 CameraVector next{state.target.x+(float)x,state.target.y+(float)y,state.target.z+(float)z};if(!camera_finite(next))return false;state.target=next;return true;
}
inline bool camera_smooth(FreeCamera& state,const CameraSettings& settings){
 if(!state.initialized||!camera_settings_valid(settings)||!camera_finite(state.target)||!camera_finite(state.position))return false;
 state.position.x+=(state.target.x-state.position.x)*settings.smoothing;state.position.y+=(state.target.y-state.position.y)*settings.smoothing;state.position.z+=(state.target.z-state.position.z)*settings.smoothing;return camera_finite(state.position);
}
inline std::array<float,4> camera_quaternion(float yaw,float pitch){
 if(!std::isfinite(yaw)||!std::isfinite(pitch))return {0,0,0,1};
 const double y=(180.0-yaw)*3.14159265358979323846/360,p=-pitch*3.14159265358979323846/360;const double sy=std::sin(y),cy=std::cos(y),sp=std::sin(p),cp=std::cos(p);
 return {(float)(sp*cy),(float)(cp*sy),(float)(-sp*sy),(float)(cp*cy)};
}
// Analytic inverse of the quaternion expression above.
inline bool camera_angles(const std::array<float,4>& q,float* yaw,float* pitch){
 if(!yaw||!pitch)return false;double norm=0;for(float x:q){if(!std::isfinite(x))return false;norm+=(double)x*x;}if(norm<1e-12)return false;
 const double inverse=1/std::sqrt(norm),x=q[0]*inverse,y=q[1]*inverse,z=q[2]*inverse,w=q[3]*inverse;
 *pitch=(float)(-std::asin(std::clamp(2*(w*x-y*z),-1.0,1.0))*180/3.14159265358979323846);
 *yaw=camera_wrap((float)(180-std::atan2(2*(w*y-x*z),1-2*(y*y+z*z))*180/3.14159265358979323846));return true;
}
}
