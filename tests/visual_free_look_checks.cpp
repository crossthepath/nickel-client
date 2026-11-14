#include "nickel/visual_free_look.hpp"
#include "nickel/visual_free_camera.hpp"
#include <cstdio>
#include <limits>
int main(){using namespace nickel::visual;int failed=0;auto check=[&](bool ok,const char* name){if(!ok){std::fprintf(stderr,"%s failed\n",name);++failed;}};auto near=[](float a,float b){return std::fabs(a-b)<.01f;};
 // onEnable: ThirdPerson -> 1, FirstPerson -> 0, Current -> nothing; no call if already at the target.
 check(free_look_perspective_target(0,0)==1&&free_look_perspective_target(0,2)==1&&free_look_perspective_target(0,1)==-1,"third person target");
 check(free_look_perspective_target(1,1)==0&&free_look_perspective_target(1,0)==-1&&free_look_perspective_target(2,0)==-1&&free_look_perspective_target(2,1)==-1,"first person and current");
 // Mode 2: the player keeps the saved orientation whatever the current orientation.
 LookRotation saved{20,35,40},out{};check(free_look_apply(saved,-60,170,&out)&&near(out.pitch,20)&&near(out.yaw,40)&&near(out.headYaw,35),"saved rotation reapplied");
 saved={95,0,-10};check(free_look_apply(saved,0,350,&out)&&near(out.pitch,90),"pitch clamped to 90 like the manager");
 check(free_look_apply({0,0,370},0,0,&out)&&near(out.yaw,10),"yaw wrapped by fmodf 360");
 check(!free_look_apply({std::numeric_limits<float>::quiet_NaN(),0,0},0,0,&out),"NaN rejected");
 // Camera read against Freecam's quaternion write formula: round trip.
 for(float yaw:{-170.f,-90.f,-30.f,0.f,45.f,90.f,170.f})for(float pitch:{-80.f,-30.f,0.f,30.f,80.f}){float p{},y{};const auto q=camera_quaternion(yaw,pitch);
  check(free_look_camera_angles(q,false,&p,&y)&&near(std::remainder(y-yaw,360.f),0)&&near(p,pitch),"original camera reader inverts the written quaternion");
  float pf{},yf{};check(free_look_camera_angles(q,true,&pf,&yf)&&near(pf,-pitch)&&near(std::remainder(yf-yaw-180.f,360.f),0),"front view reverses the forward vector");}
 float p{},y{};check(!free_look_camera_angles({0,std::numeric_limits<float>::infinity(),0,1},false,&p,&y),"non finite quaternion rejected");
 // onDisable: Vec2{-(saved.pitch-camera.pitch), fmodf(saved.yaw-camera.yaw,360)}.
 const auto turn=free_look_release_turn({10,0,400},4,20);check(near(turn[0],-6)&&near(turn[1],20),"release turn vector");
 return failed?1:0;}
