#include "nickel/visual_nametags.hpp"
#include <cmath>
#include <cstdio>
#include <limits>
int main(){
 using nickel::visual::nametag_target;using nickel::visual::nametag_scale;using nickel::visual::nametag_anchor;using nickel::visual::clean_nametag;using nickel::visual::nametag_name;using nickel::visual::nametag_health_color;using nickel::visual::nametag_hurt;using nickel::visual::nametag_layout;using nickel::visual::nametag_durability_color;using nickel::visual::nametag_frame_usable;
 int failures=0;
 auto check=[&](bool ok,const char* name){if(!ok){std::fprintf(stderr,"%s failed\n",name);++failures;}};
 auto near=[&](float a,float b){return std::fabs(a-b)<1e-4f;};
 check(nametag_target(true,false,false),"target player no living no mobs");
 check(nametag_target(true,true,false),"target player living no mobs");
 check(!nametag_target(false,true,false),"target mob no mobs");
 check(!nametag_target(false,false,true),"target nonliving mobs");
 check(nametag_target(false,true,true),"target mob with mobs");
 check(nametag_target(false,true,true)&&!nametag_target(false,false,true),"target mob gate");
 check(near(nametag_scale(0,1),1),"scale distance 0");
 check(near(nametag_scale(8,1),1),"scale distance 8");
 check(near(nametag_scale(32,1),14.f/18.f),"scale distance 32");
 check(near(nametag_scale(32,2),1.5f),"scale distance 32 scale 2");
 check(near(nametag_scale(32,.5f),7.f/18.f),"scale distance 32 scale .5");
 check(nametag_scale(-1,1)==0,"scale negative distance");
 check(nametag_scale(std::numeric_limits<float>::quiet_NaN(),1)==0,"scale NaN distance");
 check(nametag_scale(std::numeric_limits<float>::infinity(),1)==0,"scale infinite distance");
 check(nametag_scale(8,0)==0&&nametag_scale(8,-1)==0,"scale bad scale");
 check(nametag_scale(8,std::numeric_limits<float>::quiet_NaN())==0,"scale NaN scale");
 check(nametag_scale(8,std::numeric_limits<float>::infinity())==0,"scale infinite scale");
 {auto a=nametag_anchor({1,2,3},{3,6,7});check(near(a.x,2)&&near(a.y,6.2f)&&near(a.z,5),"anchor bbox");}
 {const char raw[]="Alex" "\xC2\xA7" "a" "Smith";check(clean_nametag(raw,true)=="AlexSmith","formatting strips section");
  check(clean_nametag("Alex\r\nSmith",true)=="Alex  Smith","formatting CRLF two spaces");
  check(clean_nametag("A" "\xC2\xA7" "Z" "B",true)=="AB","formatting section Z removed");
  check(clean_nametag("A" "\xC2\xA7" "!" "B",true)==std::string("A")+"\xC2\xA7" "!B","formatting section bang kept");
  check(clean_nametag("A" "\xC2\xA7",true)==std::string("A")+"\xC2\xA7","formatting bare section kept");
  check(clean_nametag("  pad  ",false)=="pad","plain trim only");
  check(clean_nametag("A" "\xC2\xA7" "a" "  B  ",true)=="A  B","formatting trims edges");}
 {check(nametag_name(true,true,"Plain","Custom")=="Custom","name player display custom");
  check(nametag_name(true,true,"Plain","")=="Plain","name player empty custom falls back");
  check(nametag_name(true,false," Plain ","Custom")=="Plain","name player flag off plain");
  check(nametag_name(false,true,"Plain","Custom")=="Custom","name mob custom preferred");
  check(nametag_name(false,true,"Plain","").empty(),"name mob empty custom fallback signal");
  check(nametag_name(false,false,"Plain"," Custom ")=="Custom","name mob custom regardless flag");}
 check(nametag_health_color(6)==0xff5c5cffu,"health color 6");
 check(nametag_health_color(6.01f)==0xff5cc2ffu,"health color 6.01");
 check(nametag_health_color(12)==0xff5cc2ffu,"health color 12");
 check(nametag_health_color(12.01f)==0xff9ae870u,"health color 12.01");
 check(nametag_health_color(std::numeric_limits<float>::quiet_NaN())==0xffffffffu,"health color NaN");
 {const float half=std::log(2.f)/18.f;
  check(near(nametag_hurt(0,10,half),.5f),"hurt 0 to .5 half life");
  check(near(nametag_hurt(1,0,half),.5f),"hurt 1 to .5 half life");
  check(nametag_hurt(.4,10,0)==.4f,"hurt dt zero unchanged");
  check(nametag_hurt(0,10,.5f)>.99f,"hurt dt .5 full response");}
 {check(nametag_frame_usable(true,false,1000,1000,7,7,3,3,0,0),"frame usable same context");
  check(!nametag_frame_usable(true,false,1000,1000,7,8,3,3,0,0),"frame usable actor changed");
  check(!nametag_frame_usable(true,false,1000,1000,7,7,3,4,0,0),"frame usable serial changed");
  check(!nametag_frame_usable(true,false,1000,1000,7,7,3,3,0,1),"frame usable dimension changed");
  check(!nametag_frame_usable(true,false,1000,0,7,7,3,3,0,0),"frame usable zero tick");
  check(!nametag_frame_usable(true,false,1000,1000,0,0,3,3,0,0),"frame usable zero actor");
  check(nametag_frame_usable(true,false,1000,1000,7,7,0,0,0,0),"frame usable serial zero");
  check(!nametag_frame_usable(true,false,1000,1000,7,7,0,1,0,0),"frame usable serial zero mismatch");
  check(nametag_frame_usable(true,false,1250,1000,7,7,3,3,0,0),"frame usable age 250");
  check(!nametag_frame_usable(true,false,1251,1000,7,7,3,3,0,0),"frame usable expired");
  check(!nametag_frame_usable(true,false,999,1000,7,7,3,3,0,0),"frame usable future tick");
  check(!nametag_frame_usable(false,false,1000,1000,7,7,3,3,0,0),"frame usable disabled");
  check(!nametag_frame_usable(true,true,1000,1000,7,7,3,3,0,0),"frame usable fault");}
 {auto l=nametag_layout(100,200,1,40,20,30);
  check(near(l.left,46)&&near(l.top,178)&&near(l.right,154)&&near(l.bottom,200),"layout bounds");
  check(near(l.nameX,50)&&near(l.healthX,95)&&near(l.distanceX,120)&&near(l.textY,180),"layout positions");
  auto m=nametag_layout(100,200,1,40,0,30);
  check(near(m.left,59)&&near(m.distanceX,108),"layout missing health gap");}
 check(nametag_durability_color(0)==0xff0000ffu,"durability ratio 0");
 check(nametag_durability_color(1)==0xff00ff00u,"durability ratio 1");
 check(nametag_durability_color(.5f)==0xff00ffffu,"durability ratio .5");
 check(nametag_durability_color(.25f)==0xff007fffu,"durability ratio .25");
 check(nametag_durability_color(.75f)==0xff00ff7fu,"durability ratio .75");
 if(failures){std::fprintf(stderr,"%d checks failed\n",failures);return 1;}
 std::printf("visual nametags checks passed\n");return 0;
}
