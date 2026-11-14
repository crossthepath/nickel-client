#pragma once
#include <cctype>
#include <cmath>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>
namespace nickel::visual {
struct TeamRules{bool enabled=false,auto_color=true,name_prefix=true;uint16_t colors=0;std::vector<std::string> names;};
inline bool within_visual_range(float squaredDistance,float range){
 if(!std::isfinite(squaredDistance)||squaredDistance<0||!std::isfinite(range)||range<=0)return false;
 return double(squaredDistance)<=double(range)*double(range);
}
template<class Reader>
bool read_visual_text(uintptr_t address,char* out,size_t capacity,Reader read){
 if(!out||capacity==0)return false;
 out[0]=0;
 if(address+0x18<address)return false;
 uint64_t size{},room{},again{};
 if(!read(address+0x10,&size)||!read(address+0x18,&room)||size>room||size>=capacity)return false;
 uintptr_t data=address;
 if(room>15&&(!read(address,&data)||!data))return false;
 for(uint64_t i=0;i<size;++i){char byte{};if(!read(data+i,&byte)){out[0]=0;return false;}out[i]=byte;}
 out[size]=0;
 if(!read(address+0x10,&again)||again!=size){out[0]=0;return false;}
 return true;
}
inline int display_color(std::string_view display,std::string_view plain){
 size_t limit=display.size();const size_t newline=display.find('\n');if(newline!=std::string_view::npos&&newline<limit)limit=newline;
 if(!plain.empty()){const size_t at=display.find(plain);if(at!=std::string_view::npos&&at<limit)limit=at;}
 static const std::string_view valid="0123456789abcdefghijmnpqstuv";int color=-1;
 for(size_t i=0;i+2<limit;){
  if((unsigned char)display[i]==0xC2&&(unsigned char)display[i+1]==0xA7){
   const char code=(char)std::tolower((unsigned char)display[i+2]);
   if(code=='r')color=-1;else{const size_t hit=valid.find(code);if(hit!=std::string_view::npos)color=(int)hit;}
   i+=3;continue;
  }
  ++i;
 }
 return color;
}
inline std::string display_prefix(std::string_view display,std::string_view plain){
 if(plain.empty())return{};const size_t at=display.find(plain);if(at==std::string_view::npos)return{};
 std::string prefix;prefix.reserve(at);
 for(size_t i=0;i<at;){if((unsigned char)display[i]==0xC2&&i+2<at&&(unsigned char)display[i+1]==0xA7){i+=3;continue;}prefix+=display[i++];}
 while(!prefix.empty()&&std::isspace((unsigned char)prefix.back()))prefix.pop_back();
 return prefix;
}
inline bool ascii_name_equal(std::string_view a,std::string_view b){
 if(a.size()!=b.size())return false;
 for(size_t i=0;i<a.size();++i){char x=a[i],y=b[i];if(x>='A'&&x<='Z')x=(char)(x+32);if(y>='A'&&y<='Z')y=(char)(y+32);if(x!=y)return false;}
 return true;
}
inline bool teammate(const TeamRules& rules,std::string_view selfName,std::string_view selfDisplay,std::string_view otherName,std::string_view otherDisplay){
 if(!rules.enabled)return false;
 for(const auto& name:rules.names)if(!name.empty()&&ascii_name_equal(name,otherName))return true;
 const int selfColor=display_color(selfDisplay,selfName),otherColor=display_color(otherDisplay,otherName);
 if(rules.auto_color){if(selfColor>=0&&otherColor>=0&&selfColor==otherColor)return true;}
 else if(otherColor>=0&&otherColor<16&&(rules.colors&(uint16_t)(1u<<otherColor)))return true;
 if(rules.name_prefix){const std::string a=display_prefix(selfDisplay,selfName),b=display_prefix(otherDisplay,otherName);if(!a.empty()&&a==b)return true;}
 return false;
}
}
