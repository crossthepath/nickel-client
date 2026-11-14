#include "nickel/visual_world.hpp"
#include <cmath>
#include <cstdio>
#include <limits>
int main(){
 using nickel::visual::Vec3;using nickel::visual::reach_ring;using nickel::visual::cancel_out_of_reach;using nickel::visual::world_frame_fresh;using nickel::visual::same_world;
 int failures=0;
 auto check=[&](bool ok,const char* name){if(!ok){std::fprintf(stderr,"%s failed\n",name);++failures;}};
 auto near=[&](float a,float b){return std::fabs(a-b)<1e-4f;};
 {auto ring=reach_ring({10,20,30},.9f,3);
  check(ring.size()==49,"ring has 49 points");
  check(near(ring[0].x,13)&&near(ring[0].y,20.9f)&&near(ring[0].z,30),"ring first point");
  check(near(ring[12].x,10)&&near(ring[12].y,20.9f)&&near(ring[12].z,33),"ring point 12");
  bool closed=near(ring[48].x,ring[0].x)&&near(ring[48].y,ring[0].y)&&near(ring[48].z,ring[0].z);check(closed,"ring closes");
  bool flat=true,round=true;for(auto& p:ring){flat=flat&&near(p.y,20.9f);const float dx=p.x-10,dz=p.z-30;round=round&&near(dx*dx+dz*dz,9);}check(flat,"ring fixed height");check(round,"ring radius");}
 check(!cancel_out_of_reach(false,true,true,4,3),"cancel disabled");
 check(!cancel_out_of_reach(true,false,true,4,3),"cancel flag off");
 check(!cancel_out_of_reach(true,true,false,4,3),"cancel unknown");
 check(!cancel_out_of_reach(true,true,true,3,3),"cancel equality");
 check(cancel_out_of_reach(true,true,true,3.01f,3),"cancel above limit");
 check(!cancel_out_of_reach(true,true,true,std::numeric_limits<float>::quiet_NaN(),3),"cancel NaN distance");
 check(!cancel_out_of_reach(true,true,true,std::numeric_limits<float>::infinity(),3),"cancel infinite distance");
 check(!cancel_out_of_reach(true,true,true,-1,3),"cancel negative distance");
 check(!cancel_out_of_reach(true,true,true,4,0),"cancel zero range");
 check(!cancel_out_of_reach(true,true,true,4,-3),"cancel negative range");
 check(!cancel_out_of_reach(true,true,true,4,std::numeric_limits<float>::quiet_NaN()),"cancel NaN range");
 check(!cancel_out_of_reach(true,true,true,4,std::numeric_limits<float>::infinity()),"cancel infinite range");
 check(!cancel_out_of_reach(true,true,true,0,3),"cancel zero distance valid input no cancel");
 check(world_frame_fresh(1000,1000,7,7,3,3,0,0),"fresh same tick");
 check(world_frame_fresh(1250,1000,7,7,3,3,0,0),"fresh age 250");
 check(!world_frame_fresh(1251,1000,7,7,3,3,0,0),"fresh age 251");
 check(!world_frame_fresh(1000,0,7,7,3,3,0,0),"fresh zero tick");
 check(!world_frame_fresh(999,1000,7,7,3,3,0,0),"fresh future tick");
 check(!world_frame_fresh(1000,1000,0,0,3,3,0,0),"fresh zero actor");
 check(!world_frame_fresh(1000,1000,7,8,3,3,0,0),"fresh changed actor");
 check(!world_frame_fresh(1000,1000,7,7,3,4,0,0),"fresh changed serial");
 check(!world_frame_fresh(1000,1000,7,7,3,3,0,1),"fresh changed dimension");
 check(same_world(7,7,3,3,0,0),"same world");
 check(!same_world(0,0,3,3,0,0),"same world zero actor");
 check(!same_world(7,8,3,3,0,0),"same world actor mismatch");
 check(!same_world(7,7,3,4,0,0),"same world serial mismatch");
 check(!same_world(7,7,3,3,0,1),"same world dimension mismatch");
 {auto feet=nickel::visual::feet_position({10,20,30},1.62f);check(near(feet.x,10)&&near(feet.y,18.38f)&&near(feet.z,30),"feet offset 1.62");
  feet=nickel::visual::feet_position({10,20,30},0);check(near(feet.x,10)&&near(feet.y,20)&&near(feet.z,30),"feet offset zero");
  feet=nickel::visual::feet_position({10,20,30},.9f);check(near(feet.x,10)&&near(feet.y,19.1f)&&near(feet.z,30),"feet offset .9");}
 if(failures){std::fprintf(stderr,"%d checks failed\n",failures);return 1;}
 std::printf("visual world checks passed\n");return 0;
}
