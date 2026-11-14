#include "nickel/visual_entity_rules.hpp"
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <string>
#include <vector>
int main(){
 using nickel::visual::TeamRules;using nickel::visual::teammate;using nickel::visual::within_visual_range;
 int failures=0;
 auto check=[&](bool ok,const char* name){if(!ok){std::fprintf(stderr,"%s failed\n",name);++failures;}};
 auto enabled=[]{TeamRules r;r.enabled=true;return r;};
 {TeamRules r;r.enabled=false;r.names={"Steve"};check(!teammate(r,"Steve","","Steve",""),"disabled rejects manual name");check(!teammate(r,"Steve","§aSteve","Alex","§aAlex"),"disabled rejects color");}
 {auto r=enabled();check(teammate(r,"Steve","§aSteve","Alex","§aAlex"),"same color auto");check(!teammate(r,"Steve","§aSteve","Alex","§cAlex"),"different color auto");}
 {auto r=enabled();check(!teammate(r,"Steve","Steve","Alex","Alex"),"uncolored both no match");check(!teammate(r,"Steve","","",""),"empty displays no match");}
 {auto r=enabled();check(teammate(r,"Steve","§a§cSteve","Alex","§cAlex"),"last color wins match");check(!teammate(r,"Steve","§a§cSteve","Alex","§aAlex"),"last color wins mismatch");}
 {auto r=enabled();check(teammate(r,"Steve","§A Steve","Alex","§aAlex"),"uppercase code matches");}
 {auto r=enabled();check(!teammate(r,"Steve","§a§rSteve","Alex","§a§rAlex"),"reset before name clears");check(teammate(r,"Steve","§a§r§cSteve","Alex","§cAlex"),"reset then color restores");}
 {auto r=enabled();check(teammate(r,"Steve","§a§kSteve","Alex","§a§lAlex"),"style codes preserve color");check(!teammate(r,"Steve","§a§kSteve","Alex","§cAlex"),"style codes do not set color");}
 {auto r=enabled();check(teammate(r,"Steve","§aSteve","Alex","§aAlex\n§cTail"),"newline ends color scan");check(teammate(r,"Steve","§aSteve","Alex","§aGhost\n§cTail"),"newline bounds scan without name");check(!teammate(r,"Steve","§aSteve","Alex","§cGhost\n§aTail"),"color before newline kept");}
 {auto r=enabled();check(teammate(r,"Steve","§aSteve","Alex","§aAlex§c"),"formatting after plain name ignored");check(!teammate(r,"Steve","§aSteve","Alex","§cAlex§a"),"formatting after name does not recolor");}
 {auto r=enabled();check(teammate(r,"Steve","§gSteve","Alex","§gAlex"),"extended code auto match");r.auto_color=false;r.colors=0xffff;check(!teammate(r,"Steve","§gSteve","Alex","§gAlex"),"extended code outside 16 mask");}
 {auto r=enabled();r.auto_color=false;r.colors=(uint16_t)(1u<<10);check(teammate(r,"Steve","Steve","Alex","§aAlex"),"explicit green bit auto off");check(!teammate(r,"Steve","Steve","Alex","§cAlex"),"wrong color bit auto off");r.auto_color=true;r.colors=0;check(teammate(r,"Steve","§aSteve","Alex","§aAlex"),"auto color ignores mask");}
 {auto r=enabled();r.names={"steve","Alex"};check(teammate(r,"Steve","","STEVE",""),"manual ascii insensitive empty display");check(teammate(r,"Steve","","alex",""),"manual second entry");check(!teammate(r,"Steve","","Ste",""),"manual rejects substring");check(!teammate(r,"Steve","","SteveX",""),"manual rejects superstring");}
 {auto r=enabled();r.auto_color=false;r.colors=0;
  check(teammate(r,"Steve","§a[ABC] Steve","Alex","§c[ABC]  Alex"),"prefix match stripped");
  check(teammate(r,"Steve","§a[ABC] Steve","Alex","§c§k[ABC] Alex"),"prefix ignores formatting");
  check(!teammate(r,"Steve","§a[ABC] Steve","Alex","§c[ABD] Alex"),"prefix differs");
  check(!teammate(r,"Steve","§a[abc] Steve","Alex","§c[ABC] Alex"),"prefix case sensitive");
  check(!teammate(r,"Steve","§aSteve","Alex","§cAlex"),"empty prefixes no match");
  r.name_prefix=false;check(!teammate(r,"Steve","§a[ABC] Steve","Alex","§c[ABC] Alex"),"name_prefix off prevents match");}
 check(!within_visual_range(-1.f,10.f),"negative squared rejected");
 check(!within_visual_range(std::numeric_limits<float>::quiet_NaN(),10.f),"NaN squared rejected");
 check(!within_visual_range(std::numeric_limits<float>::infinity(),10.f),"infinite squared rejected");
 check(!within_visual_range(100.f,0.f),"zero range rejected");
 check(!within_visual_range(100.f,-5.f),"negative range rejected");
 check(!within_visual_range(100.f,std::numeric_limits<float>::quiet_NaN()),"NaN range rejected");
 check(!within_visual_range(100.f,std::numeric_limits<float>::infinity()),"infinite range rejected");
 check(within_visual_range(65536.f,256.f),"boundary distance included");
 check(!within_visual_range(2560000.f,256.f),"1600 blocks excluded at 256");
 check(within_visual_range(2560000.f,std::numeric_limits<float>::max()),"1600 blocks included at FLT_MAX");
 check(within_visual_range(0.f,1.f),"zero distance included");
 {
  std::vector<unsigned char> mem(8192,0);
  auto put64=[&](size_t at,uint64_t v){memcpy(mem.data()+at,&v,sizeof v);};
  auto base=[&](uintptr_t at,auto* value)->bool{const size_t n=sizeof(*value);if(at>mem.size()||n>mem.size()-at)return false;memcpy(value,mem.data()+at,n);return true;};
  const uintptr_t S=0x100,H=0x400;char out[64];
  const char sso[]="§aHi\nThere";const size_t ssoSize=sizeof(sso)-1;
  put64(S+0x10,ssoSize);put64(S+0x18,15);memcpy(mem.data()+S,sso,ssoSize);
  memset(out,'#',sizeof out);
  check(nickel::visual::read_visual_text(S,out,sizeof out,base),"sso text read");
  check(out[ssoSize]==0&&memcmp(out,sso,ssoSize)==0,"sso utf8 newline preserved");
  const char heap[]="§cHeapName\n12345678";const size_t heapSize=sizeof(heap)-1;
  put64(S+0x10,heapSize);put64(S+0x18,64);put64(S,H);memcpy(mem.data()+H,heap,heapSize);
  memset(out,'#',sizeof out);
  check(nickel::visual::read_visual_text(S,out,sizeof out,base),"heap text read");
  check(out[heapSize]==0&&memcmp(out,heap,heapSize)==0,"heap content preserved");
  put64(S+0x10,20);put64(S+0x18,10);put64(S,H);memset(out,'#',sizeof out);
  check(!nickel::visual::read_visual_text(S,out,sizeof out,base)&&out[0]==0,"size above room rejected empty");
  put64(S+0x10,64);put64(S+0x18,64);memset(out,'#',sizeof out);
  check(!nickel::visual::read_visual_text(S,out,sizeof out,base)&&out[0]==0,"size at capacity rejected empty");
  memset(out,'#',sizeof out);
  check(!nickel::visual::read_visual_text(mem.size(),out,sizeof out,base)&&out[0]==0,"missing metadata rejected empty");
  put64(S+0x10,heapSize);put64(S+0x18,64);put64(S,0);memset(out,'#',sizeof out);
  check(!nickel::visual::read_visual_text(S,out,sizeof out,base)&&out[0]==0,"null heap pointer rejected empty");
  put64(S+0x10,ssoSize);put64(S+0x18,15);memcpy(mem.data()+S,sso,ssoSize);
  int byteReads=0;auto flakyByte=[&](uintptr_t at,auto* value)->bool{if(sizeof(*value)==1){++byteReads;if(byteReads>=3)return false;}return base(at,value);};
  memset(out,'#',sizeof out);
  check(!nickel::visual::read_visual_text(S,out,sizeof out,flakyByte)&&out[0]==0&&byteReads==3,"byte read failure empties output");
  int sizeReads=0;auto flakySize=[&](uintptr_t at,auto* value)->bool{if constexpr(sizeof(*value)==8){if(at==S+0x10){++sizeReads;if(sizeReads>=2){uint64_t changed=12;memcpy(value,&changed,sizeof changed);return true;}}}return base(at,value);};
  memset(out,'#',sizeof out);
  check(!nickel::visual::read_visual_text(S,out,sizeof out,flakySize)&&out[0]==0&&sizeReads==2,"size change on reread empties output");
  int calls=0;auto counting=[&](uintptr_t at,auto* value)->bool{++calls;return base(at,value);};
  check(!nickel::visual::read_visual_text(S,nullptr,sizeof out,counting)&&calls==0,"null output rejected without reads");
  check(!nickel::visual::read_visual_text(S,out,0,counting)&&calls==0,"zero capacity rejected without reads");
 }
 return failures?1:0;
}
