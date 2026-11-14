#pragma once
#include "visual_world.hpp"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <string_view>
namespace nickel::visual {
inline bool nametag_target(bool player,bool living,bool mobs){return player||(mobs&&living);}
inline float nametag_scale(float distance,float settingScale){
 if(!std::isfinite(distance)||distance<0||!std::isfinite(settingScale)||settingScale<=0)return 0;
 return std::round(18.f*std::max(.75f,std::sqrt(8.f/std::max(8.f,distance)))*settingScale)/18.f;
}
inline Vec3 nametag_anchor(Vec3 low,Vec3 high){return {(low.x+high.x)*.5f,high.y+.2f,(low.z+high.z)*.5f};}
inline bool tag_space(char c){return c==' '||c=='\t'||c=='\n'||c=='\v'||c=='\f'||c=='\r';}
inline bool tag_alnum(unsigned char c){return (c>='0'&&c<='9')||(c>='a'&&c<='z')||(c>='A'&&c<='Z');}
inline std::string clean_nametag(std::string_view raw,bool formatted){
 if(!formatted){size_t a=0,b=raw.size();while(a<b&&tag_space(raw[a]))++a;while(b>a&&tag_space(raw[b-1]))--b;return std::string(raw.substr(a,b-a));}
 std::string out;out.reserve(raw.size());
 for(size_t i=0;i<raw.size();++i){const unsigned char c=(unsigned char)raw[i];
  if(c==0xc2&&i+2<raw.size()&&(unsigned char)raw[i+1]==0xa7&&tag_alnum((unsigned char)raw[i+2])){i+=2;continue;}
  out+=(c=='\r'||c=='\n')?' ':(char)c;}
 size_t a=0,b=out.size();while(a<b&&tag_space(out[a]))++a;while(b>a&&tag_space(out[b-1]))--b;return std::string(out.substr(a,b-a));
}
inline std::string nametag_name(bool player,bool useDisplay,std::string_view plain,std::string_view display){
 if(player){if(useDisplay){std::string custom=clean_nametag(display,true);if(!custom.empty())return custom;}return clean_nametag(plain,false);}
 return clean_nametag(display,true);
}
inline uint32_t nametag_health_color(float hp){
 if(!std::isfinite(hp))return 0xffffffffu;
 if(hp<=6)return 0xff5c5cffu;if(hp<=12)return 0xff5cc2ffu;return 0xff9ae870u;
}
inline float nametag_hurt(float previous,int ticks,float dt){
 const float prev=std::isfinite(previous)?std::clamp(previous,0.f,1.f):0.f;
 const float f=std::isfinite(dt)&&dt>0?1.f-std::exp(-18.f*dt):0.f;
 return prev+(std::clamp(ticks/10.f,0.f,1.f)-prev)*f;
}
struct NametagLayout{float left,top,right,bottom,nameX,healthX,distanceX,textY;};
inline NametagLayout nametag_layout(float anchorX,float anchorY,float k,float nameW,float healthW,float distanceW){
 const float content=nameW+(healthW>0?5*k+healthW:0)+(distanceW>0?5*k+distanceW:0);
 NametagLayout l{};l.left=std::round(anchorX-content*.5f-4*k);l.top=std::round(anchorY-22*k);l.right=l.left+content+8*k;l.bottom=l.top+22*k;
 l.nameX=l.left+4*k;l.textY=l.top+2*k;l.healthX=l.nameX+nameW+5*k;l.distanceX=l.nameX+nameW+(healthW>0?5*k+healthW:0)+5*k;return l;
}
inline uint32_t nametag_durability_color(float ratio){
 if(!std::isfinite(ratio))ratio=0;ratio=std::clamp(ratio,0.f,1.f);
 return 0xff000000u|(uint32_t)(std::min(1.f,2.f*ratio)*255.f)<<8|(uint32_t)(std::min(1.f,2.f*(1.f-ratio))*255.f);
}
inline bool nametag_frame_usable(bool enabled,bool fault,uint64_t now,uint64_t tick,uintptr_t actor,uintptr_t currentActor,uint32_t serial,uint32_t currentSerial,int dimension,int currentDimension){
 return enabled&&!fault&&world_frame_fresh(now,tick,actor,currentActor,serial,currentSerial,dimension,currentDimension);
}
}
