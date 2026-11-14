#include "nickel/visual_free_camera.hpp"
#include <cstdio>
#include <limits>
int main(){using namespace nickel::visual;int failed=0;auto check=[&](bool ok,const char* name){if(!ok){std::fprintf(stderr,"%s failed\n",name);++failed;}};auto near=[](float a,float b){return std::fabs(a-b)<.002f;};CameraSettings settings{};FreeCamera state{};
 check(!camera_turn(state,5,5),"uninitialized turn rejected");check(!camera_move(state,{},settings),"uninitialized movement rejected");
 check(camera_initialize(state,7,2,0,{10,20,30},0,0,settings),"initialization");CameraInput input{};input.forward=true;check(camera_move(state,input,settings)&&near(state.target.z,31)&&near(state.target.x,10),"yaw zero forward");
 camera_initialize(state,7,2,0,{0,0,0},90,0,settings);camera_move(state,input,settings);check(near(state.target.x,-1)&&near(state.target.z,0),"yaw ninety forward");
 camera_initialize(state,7,2,0,{0,0,0},0,0,settings);input.left=true;camera_move(state,input,settings);check(near(std::hypot(state.target.x,state.target.z),1)&&state.target.x>0&&state.target.z>0,"diagonal speed normalized");
 input.backward=input.right=true;const auto old=state.target;camera_move(state,input,settings);check(near(state.target.x,old.x)&&near(state.target.z,old.z),"opposing movement cancels");
 input={};input.up=input.down=true;camera_move(state,input,settings);check(near(state.target.y,1),"up takes precedence");
 state.position={0,0,0};state.target={10,20,30};camera_smooth(state,settings);check(near(state.position.x,.2f)&&near(state.position.y,.4f)&&near(state.position.z,.6f),"smoothing per callback without dt");
 settings.smoothing=1;camera_smooth(state,settings);check(near(state.position.z,30),"smoothing one reaches target");
 camera_turn(state,1080+45,200);check(near(state.yaw,45)&&near(state.pitch,89.9f),"turn wrap and upper pitch clamp");camera_turn(state,0,-400);check(near(state.pitch,-89.9f),"lower pitch clamp");
 check(camera_same_world(state,7,2,0)&&!camera_same_world(state,8,2,0)&&!camera_same_world(state,7,3,0)&&!camera_same_world(state,7,2,1),"actor world and dimension isolation");
 for(float yaw:{-179.f,-90.f,0.f,90.f,179.f})for(float pitch:{-89.f,-30.f,0.f,30.f,89.f}){auto q=camera_quaternion(yaw,pitch);float y{},p{};check(camera_angles(q,&y,&p)&&near(std::remainder(y-yaw,360.f),0)&&near(p,pitch),"quaternion angle round trip");}
 float y{},p{};check(!camera_angles({0,0,0,0},&y,&p),"zero quaternion rejected");
 const auto previous=state;check(!camera_turn(state,std::numeric_limits<float>::quiet_NaN(),0)&&state.yaw==previous.yaw,"NaN turn preserves state");settings.horizontal=std::numeric_limits<float>::infinity();check(!camera_move(state,{},settings),"invalid speed rejected");
 check(!camera_initialize(state,0,0,0,{0,0,0},0,0,CameraSettings{})&&!state.initialized,"invalid initialization clears state");
 return failed?1:0;}
