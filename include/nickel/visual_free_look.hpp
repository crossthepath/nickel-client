#pragma once
// Free Look: portable maths, testable outside the game.
// On enable a target perspective is chosen; while active a rotation request (mode 2, priority 4, speed 0, absolute,
// head yaw forced) holds the player's rotation; camera angles drive the view; on disable the rotation is restored.
#include <algorithm>
#include <array>
#include <cmath>
namespace nickel::visual {
struct LookRotation{float pitch=0,headYaw=0,yaw=0;};
// Perspective: 0 ThirdPerson -> 1, 1 FirstPerson -> 0, 2 Current -> none; -1 = leave unchanged (already the target
// or mode 2).
inline int free_look_perspective_target(int mode,int current){const int target=mode==0?1:mode==1?0:-1;return target<0||target==current?-1:target;}
// Applies a mode 2 request with speed=0 (factor 1), not relative and without step clamping:
// d = fmodf(target - current, 360); yaw = fmodf(d + current, 360); pitch clamped to [-90, 90]; head yaw = the saved one.
inline bool free_look_apply(const LookRotation& saved,float currentPitch,float currentYaw,LookRotation* out){
 if(!out||!std::isfinite(saved.pitch)||!std::isfinite(saved.yaw)||!std::isfinite(saved.headYaw)||!std::isfinite(currentPitch)||!std::isfinite(currentYaw))return false;
 const float dYaw=std::fmod(saved.yaw-currentYaw,360.f),dPitch=saved.pitch-currentPitch;
 const float yaw=std::fmod(dYaw+currentYaw,360.f),pitch=dPitch+currentPitch;
 out->pitch=pitch>90.f?90.f:(pitch<-90.f?-90.f:pitch);out->headYaw=saved.headYaw;out->yaw=yaw;return true;
}
// Camera angles: forward vector from the camera quaternion (x,y,z,w), inverted in front third person (option 2);
// yaw = atan2(-Z, X)·57.29578, pitch = atan2(Y, hypot(Z, X))·-57.29578.
inline bool free_look_camera_angles(const std::array<float,4>& q,bool front,float* pitch,float* yaw){
 if(!pitch||!yaw)return false;for(float v:q)if(!std::isfinite(v))return false;
 const float qx=q[0],qy=q[1],qz=q[2],qw=q[3];
 const float a=qx*0.f-qy*0.f,b=-qy-qz*0.f,c=qz*0.f+qx;
 const float t0=a*qw+(qx*c-qy*b),t1=c*qw+(qz*b-qx*a),t2=b*qw+(qy*a-qz*c);
 float x=t0+t0-1.f,y=t1+t1+0.f,z=t2+t2+0.f;if(front){x=0.f-x;y=0.f-y;z=0.f-z;}
 const float h=std::hypot(z,x);*yaw=std::atan2(-z,x)*57.29578f;*pitch=std::atan2(y,h)*-57.29578f;return std::isfinite(*yaw)&&std::isfinite(*pitch);
}
// On disable without Keep Camera Rotation: Vec2 argument of game function RVA 0x6da57d0 =
// { -(saved.pitch - camera.pitch), fmodf(saved.yaw - camera.yaw, 360) }.
inline std::array<float,2> free_look_release_turn(const LookRotation& saved,float cameraPitch,float cameraYaw){
 return {-(saved.pitch-cameraPitch),std::fmod(saved.yaw-cameraYaw,360.f)};
}
}
