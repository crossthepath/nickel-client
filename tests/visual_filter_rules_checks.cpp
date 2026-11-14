#include "../include/nickel/visual_filter_rules.hpp"
#include <cstdio>
#include <limits>
#include <unordered_map>
using namespace nickel::visual;
static int checks=0,failures=0;
#define CHECK(expr) do{++checks;if(!(expr)){++failures;std::printf("FAIL %d %s\n",__LINE__,#expr);}}while(0)
struct Mem{std::unordered_map<uintptr_t,uint64_t> m;bool read(uintptr_t a,uint64_t* v)const{auto it=m.find(a);if(it==m.end())return false;*v=it->second;return true;}};
static auto rd(const Mem& mem){return [&mem](uintptr_t a,uint64_t* v){return mem.read(a,v);};}
static void put(Mem& mem,uintptr_t at,uint64_t v){mem.m[at]=v;}
static Mem twoNode(){
 Mem mem;put(mem,0x1008,0x2000);put(mem,0x1010,2);put(mem,0x2000,0x2100);
 put(mem,0x2100,0x2200);put(mem,0x2120,101);put(mem,0x2200,0x2000);put(mem,0x2220,202);return mem;
}
static Mem foliage(uintptr_t renderer,uint64_t head,int nodes){
 Mem mem;put(mem,renderer+0x30,head);if(!head)return mem;
 put(mem,head,nodes?(uintptr_t)0x4100:head);
 for(int i=0;i<nodes;++i){const uintptr_t n=0x4100+i*0x200;put(mem,n,i+1<nodes?(uintptr_t)(0x4100+(i+1)*0x200):head);put(mem,n+0x18,0x100000+i*0x1000);put(mem,0x100000+i*0x1000,1);}
 return mem;
}
int main(){
 {VisualRoster r=read_visual_roster(0x1000,rd(twoNode()));CHECK(r.known);CHECK(r.ids.size()==2);CHECK(r.ids.count(101)&&r.ids.count(202));}
 {Mem mem;put(mem,0x1008,0x2000);put(mem,0x1010,0);put(mem,0x2000,0x2000);
  VisualRoster r=read_visual_roster(0x1000,rd(mem));CHECK(r.known);CHECK(r.ids.empty());}
 {Mem mem;put(mem,0x1008,0x2000);put(mem,0x1010,0);put(mem,0x2000,0x2000);
  auto sneaky=[&](uintptr_t a,uint64_t* v){const bool ok=mem.read(a,v);if(ok&&a==0x2000)mem.m[0x1010]=1;return ok;};
  VisualRoster r=read_visual_roster(0x1000,sneaky);CHECK(!r.known);CHECK(r.ids.empty());}
 {CHECK(!read_visual_roster(0,rd(twoNode())).known);
  Mem bad;put(bad,0x1010,1);CHECK(!read_visual_roster(0x1000,rd(bad)).known);
  Mem over=twoNode();over.m[0x1010]=4097;CHECK(!read_visual_roster(0x1000,rd(over)).known);}
 {Mem mem=twoNode();mem.m.erase(0x2200);VisualRoster r=read_visual_roster(0x1000,rd(mem));CHECK(!r.known);CHECK(r.ids.empty());}
 {Mem mem=twoNode();mem.m.erase(0x2220);VisualRoster r=read_visual_roster(0x1000,rd(mem));CHECK(!r.known);CHECK(r.ids.empty());}
 {Mem mem=twoNode();put(mem,0x2100,0x2000);put(mem,0x1010,2);VisualRoster r=read_visual_roster(0x1000,rd(mem));CHECK(!r.known);CHECK(r.ids.empty());}
 {Mem mem=twoNode();put(mem,0x2200,0x2300);put(mem,0x2300,0x2000);put(mem,0x2320,303);VisualRoster r=read_visual_roster(0x1000,rd(mem));CHECK(!r.known);CHECK(r.ids.empty());}
 {Mem mem=twoNode();put(mem,0x2100,0x2100);put(mem,0x1010,1);VisualRoster r=read_visual_roster(0x1000,rd(mem));CHECK(!r.known);CHECK(r.ids.empty());}
 {Mem mem=twoNode();put(mem,0x2000,0x2000);VisualRoster r=read_visual_roster(0x1000,rd(mem));CHECK(!r.known);CHECK(r.ids.empty());}
 {Mem mem=twoNode();put(mem,0x1010,3);VisualRoster r=read_visual_roster(0x1000,rd(mem));CHECK(!r.known);CHECK(r.ids.empty());}
 {CHECK(!read_visual_roster(std::numeric_limits<uintptr_t>::max()-8,rd(twoNode())).known);}
 {Mem mem=twoNode();int headReads=0;auto r=[&](uintptr_t a,uint64_t* v){const bool ok=mem.read(a,v);if(ok&&a==0x1008&&++headReads>1)*v=0x2008;return ok;};
  VisualRoster out=read_visual_roster(0x1000,r);CHECK(!out.known);CHECK(out.ids.empty());}
 {Mem mem=twoNode();int countReads=0;auto r=[&](uintptr_t a,uint64_t* v){const bool ok=mem.read(a,v);if(ok&&a==0x1010&&++countReads>1)*v=3;return ok;};
  VisualRoster out=read_visual_roster(0x1000,r);CHECK(!out.known);CHECK(out.ids.empty());}
 {Mem mem=twoNode();put(mem,0x2200,0);VisualRoster r=read_visual_roster(0x1000,rd(mem));CHECK(!r.known);CHECK(r.ids.empty());}
 {Mem mem=twoNode();put(mem,0x1008,0);VisualRoster r=read_visual_roster(0x1000,rd(mem));CHECK(!r.known);CHECK(r.ids.empty());}
 {Mem mem=twoNode();mem.m.erase(0x1010);VisualRoster r=read_visual_roster(0x1000,rd(mem));CHECK(!r.known);CHECK(r.ids.empty());}
 {Mem mem;put(mem,0x1008,0x2000);put(mem,0x1010,1);put(mem,0x2000,std::numeric_limits<uintptr_t>::max()-0x10);VisualRoster r=read_visual_roster(0x1000,rd(mem));CHECK(!r.known);CHECK(r.ids.empty());}
 {Mem mem;put(mem,0x1008,0x2000);put(mem,0x1010,2);put(mem,0x2000,0x2100);put(mem,0x2100,0x2200);put(mem,0x2120,7);put(mem,0x2200,0x2000);put(mem,0x2220,7);
  VisualRoster r=read_visual_roster(0x1000,rd(mem));CHECK(r.known);CHECK(r.ids.size()==1);CHECK(r.ids.count(7));}
 {VisualRoster roster;roster.known=true;roster.ids.insert(101);roster.ids.insert(202);
  CHECK(!visual_bot(false,true,false,roster,true,999));
  CHECK(!visual_bot(true,false,false,roster,true,999));
  CHECK(!visual_bot(true,true,true,roster,true,999));
  VisualRoster unknown;CHECK(!visual_bot(true,true,false,unknown,true,999));
  VisualRoster empty;empty.known=true;CHECK(!visual_bot(true,true,false,empty,true,999));
  CHECK(!visual_bot(true,true,false,roster,false,999));
  CHECK(!visual_bot(true,true,false,roster,true,101));
  CHECK(visual_bot(true,true,false,roster,true,999));
  CHECK(visual_bot(true,true,false,roster,true,0x8000000000000001ull));
  VisualRoster high;high.known=true;high.ids.insert(0x8000000000000001ull);
  CHECK(!visual_bot(true,true,false,high,true,0x8000000000000001ull));}
 {FoliageRefreshState st;CHECK(!foliage_refresh_pending(st));
  request_foliage(st,false);CHECK(!foliage_refresh_pending(st));CHECK(st.generation==0);
  request_foliage(st,true);CHECK(foliage_refresh_pending(st));CHECK(st.generation==1);
  request_foliage(st,true);CHECK(st.generation==1);CHECK(foliage_refresh_pending(st));
  request_foliage(st,false);CHECK(st.generation==2);CHECK(foliage_refresh_pending(st));
  acknowledge_foliage(st,1);CHECK(foliage_refresh_pending(st));CHECK(st.applied==1);
  acknowledge_foliage(st,2);CHECK(!foliage_refresh_pending(st));CHECK(st.applied==2);
  request_foliage(st,false);CHECK(st.generation==2);CHECK(!foliage_refresh_pending(st));
  acknowledge_foliage(st,9);CHECK(st.applied==2);
  request_foliage(st,true);CHECK(st.generation==3);
  acknowledge_foliage(st,2);CHECK(st.applied==2);CHECK(foliage_refresh_pending(st));
  acknowledge_foliage(st,3);CHECK(!foliage_refresh_pending(st));}
 {auto ft=read_foliage_targets(0x3000,rd(foliage(0x3000,0x4000,2)));
  CHECK(ft.known);CHECK(ft.values.size()==2);CHECK(ft.values[0]==0x100000&&ft.values[1]==0x101000);}
 {auto ft=read_foliage_targets(0x3000,rd(foliage(0x3000,0x4000,0)));CHECK(ft.known);CHECK(ft.values.empty());}
 {auto bad=read_foliage_targets(0,rd(foliage(0x3000,0x4000,1)));CHECK(!bad.known);CHECK(bad.values.empty());
  bad=read_foliage_targets(std::numeric_limits<uintptr_t>::max()-0x20,rd(foliage(0x3000,0x4000,1)));CHECK(!bad.known);CHECK(bad.values.empty());
  bad=read_foliage_targets(0x3000,rd(foliage(0x3000,0,1)));CHECK(!bad.known);CHECK(bad.values.empty());}
 {Mem mem=foliage(0x3000,0x4000,1);mem.m.erase(0x3030);auto ft=read_foliage_targets(0x3000,rd(mem));CHECK(!ft.known);CHECK(ft.values.empty());
  mem=foliage(0x3000,0x4000,1);mem.m.erase(0x4100);ft=read_foliage_targets(0x3000,rd(mem));CHECK(!ft.known);CHECK(ft.values.empty());
  mem=foliage(0x3000,0x4000,1);mem.m.erase(0x4118);ft=read_foliage_targets(0x3000,rd(mem));CHECK(!ft.known);CHECK(ft.values.empty());
  mem=foliage(0x3000,0x4000,1);mem.m.erase(0x100000);ft=read_foliage_targets(0x3000,rd(mem));CHECK(!ft.known);CHECK(ft.values.empty());}
 {Mem mem=foliage(0x3000,0x4000,2);put(mem,0x4100,0);auto ft=read_foliage_targets(0x3000,rd(mem));CHECK(!ft.known);CHECK(ft.values.empty());
  mem=foliage(0x3000,0x4000,2);put(mem,0x4300,0x4100);ft=read_foliage_targets(0x3000,rd(mem));CHECK(!ft.known);CHECK(ft.values.empty());
  mem=foliage(0x3000,0x4000,1);put(mem,0x4000,std::numeric_limits<uintptr_t>::max()-0x10);ft=read_foliage_targets(0x3000,rd(mem));CHECK(!ft.known);CHECK(ft.values.empty());}
 {Mem mem=foliage(0x3000,0x4000,2);int headReads=0;auto r=[&](uintptr_t a,uint64_t* v){const bool ok=mem.read(a,v);if(ok&&a==0x3030&&++headReads>1)*v=0x4008;return ok;};
  auto ft=read_foliage_targets(0x3000,r);CHECK(!ft.known);CHECK(ft.values.empty());}
 {Mem mem=foliage(0x3000,0x4000,2);int firstReads=0;auto r=[&](uintptr_t a,uint64_t* v){const bool ok=mem.read(a,v);if(ok&&a==0x4000&&++firstReads>1)*v=0x4300;return ok;};
  auto ft=read_foliage_targets(0x3000,r);CHECK(!ft.known);CHECK(ft.values.empty());}
 {Mem mem=foliage(0x3000,0x4000,0);int firstReads=0;auto r=[&](uintptr_t a,uint64_t* v){const bool ok=mem.read(a,v);if(ok&&a==0x4000){*v=++firstReads==1?0x4000:0x4100;return true;}return ok;};
  auto ft=read_foliage_targets(0x3000,r);CHECK(!ft.known);CHECK(ft.values.empty());}
 {Mem mem=foliage(0x3000,0x4000,256);auto ft=read_foliage_targets(0x3000,rd(mem));CHECK(ft.known);CHECK(ft.values.size()==256);
  mem=foliage(0x3000,0x4000,257);ft=read_foliage_targets(0x3000,rd(mem));CHECK(!ft.known);CHECK(ft.values.empty());}
 std::printf("%s: %d checks, %d failures\n",failures?"FAIL":"PASS",checks,failures);
 return failures?1:0;
}
