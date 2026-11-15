// Path Finder characterization ("golden") checks. Each case builds a small synthetic world (every cell the search can
// query is pre-filled in PfWorld::cells, so no game memory is read), runs the real search from game_movement_e.hpp to
// completion and compares a text serialization of the result with a value captured from the current code:
//   status, cost of the end node, then one line per path step: move type, from, to, step cost (rounded to 1e-6),
//   cells broken, cell placed, door cell, bucket flag.
// These values pin today's behaviour, they do not claim it is ideal. A refactor of the search must keep every case
// identical; a deliberate behaviour change updates the expected value (a failing case prints the actual value as a C++
// literal ready to paste).
#include "game_movement_e.hpp"
#include <cstdio>
#include <string>

namespace {
using namespace game;
constexpr int kHalf=24,kLow=-4,kHigh=8;   // the pre-filled box: x, z in [-kHalf, kHalf], y in [kLow, kHigh]

PfRec Air(){PfRec r;r.known=1;r.passable=1;r.replace=1;r.hard=-1.f;return r;}
PfRec Stone(float hard){PfRec r;r.known=1;r.solid=1;r.top=1.f;r.place=1;r.hard=hard;return r;}
PfRec Water(){PfRec r;r.known=1;r.passable=1;r.replace=1;r.cls=1;r.hard=-1.f;return r;}
PfRec Ladder(){PfRec r;r.known=1;r.passable=1;r.climb=1;r.hard=-1.f;return r;}
PfRec Door(){PfRec r;r.known=1;r.door=1;r.hard=-1.f;return r;}   // a closed door half: not passable, opened on the way

struct TestWorld{
  PfWorld w;
  TestWorld(){
    w.chunkAt=0;w.ocx=w.ocz=0;w.radius=64;w.minY=-64;w.maxY=319;
    // flat world: unbreakable floor at y = -1 and below, air above
    for(int x=-kHalf;x<=kHalf;++x)for(int z=-kHalf;z<=kHalf;++z)for(int y=kLow;y<=kHigh;++y)Set({x,y,z},y<0?Stone(-1.f):Air());
  }
  void Set(const PfP& p,const PfRec& r){w.cells[PfKey(p)]=r;}
  // Fills x in [x0, x1], y in [y0, y1] across the whole box in z.
  void Slab(int x0,int x1,int y0,int y1,const PfRec& r){for(int x=x0;x<=x1;++x)for(int z=-kHalf;z<=kHalf;++z)for(int y=y0;y<=y1;++y)Set({x,y,z},r);}
};

struct Options{bool brk=false,doors=true;int blocks=0;};   // blocks > 0 allows placing up to that many blocks

PfGoal Block(const PfP& p){PfGoal g;g.kind=0;g.p=p;return g;}

// Same setup as the module's search (PfStartSearch): goal entry estimate, placement budget = blocks; parkour off so the cases
// stay about the move under test. The time limit is generous: every world here is small enough to finish.
PfSearch Run(TestWorld& world,const PfGoal& goal,const PfP& start,const Options& op){
  PfSearch S;S.view.w=&world.w;S.view.o=PfOpt();S.view.o.brk=op.brk;S.view.o.doors=op.doors;S.view.o.place=op.blocks>0;S.view.o.blocks=op.blocks;
  S.view.o.parkour=false;S.goal=goal;S.goal.extra=PfGoalEntry(S.view,S.goal);S.start=start;S.grounded=true;S.greed=1.25;S.maxMs=60000;S.cap=op.blocks;
  world.w.edge=false;PfSearchBegin(S);
  while(S.status==0&&ActionMs(S.began)<S.maxMs)PfSearchStep(S,256);
  return S;
}

std::string Cell(const PfP& p){char b[48];std::snprintf(b,sizeof b,"(%d,%d,%d)",p.x,p.y,p.z);return b;}

std::string Serialize(PfSearch& S,const PfWorld& w){
  char b[128];std::string out;
  if(S.status!=1){std::snprintf(b,sizeof b,"status=%d edge=%d\n",S.status,w.edge?1:0);return b;}
  PfBuildPath(S,S.endNode);
  std::snprintf(b,sizeof b,"status=1 g=%.6f steps=%zu\n",S.nodes[S.endNode].g,S.moves.size());out+=b;
  for(const PfStep& s:S.moves){
    std::snprintf(b,sizeof b,"t%d %s->%s c=%.6f",(int)s.type,Cell(s.from).c_str(),Cell(s.to).c_str(),s.cost);out+=b;
    if(s.nbrk){out+=" brk=";for(int i=0;i<s.nbrk;++i)out+=Cell(s.brk[i]);}
    if(s.hasPlace)out+=" place="+Cell(s.place);
    if(s.hasDoor)out+=" door="+Cell(s.door);
    if(s.bucket)out+=" bucket";
    out+="\n";
  }
  return out;
}

// Prints a serialization as C++ string literal lines, ready to paste as the expected value.
void PrintLiteral(const std::string& text){
  size_t at=0;
  while(at<text.size()){const size_t nl=text.find('\n',at);const size_t end=nl==std::string::npos?text.size():nl;
    std::printf("      \"%s%s\"\n",text.substr(at,end-at).c_str(),nl==std::string::npos?"":"\\n");at=end+1;}
}
}

int main(){
  int failed=0;
  auto golden=[&](const char* name,TestWorld& world,const PfGoal& goal,const PfP& start,const Options& op,const char* expected){
    PfSearch S=Run(world,goal,start,op);const std::string actual=Serialize(S,world.w);const bool ok=actual==expected;
    std::printf("%s %s\n",ok?"ok  ":"FAIL",name);
    if(!ok){++failed;std::printf("    actual:\n");PrintLiteral(actual);}
  };

  {TestWorld world;   // 1. flat ground, straight line
   golden("flat walk, straight",world,Block({6,0,0}),{0,0,0},{},
      "status=1 g=21.382751 steps=6\n"
      "t0 (0,0,0)->(1,0,0) c=3.563792\n"
      "t0 (1,0,0)->(2,0,0) c=3.563792\n"
      "t0 (2,0,0)->(3,0,0) c=3.563792\n"
      "t0 (3,0,0)->(4,0,0) c=3.563792\n"
      "t0 (4,0,0)->(5,0,0) c=3.563792\n"
      "t0 (5,0,0)->(6,0,0) c=3.563792\n");}

  {TestWorld world;   // 2. flat ground, diagonal
   golden("flat walk, diagonal",world,Block({4,0,4}),{0,0,0},{},
      "status=1 g=20.159851 steps=4\n"
      "t1 (0,0,0)->(1,0,1) c=5.039963\n"
      "t1 (1,0,1)->(2,0,2) c=5.039963\n"
      "t1 (2,0,2)->(3,0,3) c=5.039963\n"
      "t1 (3,0,3)->(4,0,4) c=5.039963\n");}

  {TestWorld world;world.Slab(3,kHalf,0,0,Stone(-1.f));   // 3. the ground rises by one block at x = 3
   golden("step up one block",world,Block({5,1,0}),{0,0,0},{},
      "status=1 g=24.051412 steps=5\n"
      "t0 (0,0,0)->(1,0,0) c=3.563792\n"
      "t0 (1,0,0)->(2,0,0) c=3.563792\n"
      "t3 (2,0,0)->(3,1,0) c=9.796244\n"
      "t0 (3,1,0)->(4,1,0) c=3.563792\n"
      "t0 (4,1,0)->(5,1,0) c=3.563792\n");}

  {TestWorld world;world.Slab(-kHalf,2,0,0,Stone(-1.f));   // 4. the ground drops by one block after x = 2
   golden("step down one block",world,Block({5,0,0}),{0,1,0},{},
      "status=1 g=23.502738 steps=5\n"
      "t0 (0,1,0)->(1,1,0) c=3.563792\n"
      "t0 (1,1,0)->(2,1,0) c=3.563792\n"
      "t4 (2,1,0)->(3,0,0) c=9.247571\n"
      "t0 (3,0,0)->(4,0,0) c=3.563792\n"
      "t0 (4,0,0)->(5,0,0) c=3.563792\n");}

  {TestWorld world;world.Slab(-kHalf,1,0,2,Stone(-1.f));   // 5. a three-block drop after x = 1 (the default Max Fall)
   golden("safe fall of three blocks",world,Block({4,0,0}),{0,3,0},{},
      "status=1 g=23.792896 steps=4\n"
      "t0 (0,3,0)->(1,3,0) c=3.563792\n"
      "t5 (1,3,0)->(2,0,0) c=13.101520\n"
      "t0 (2,0,0)->(3,0,0) c=3.563792\n"
      "t0 (3,0,0)->(4,0,0) c=3.563792\n");}

  {TestWorld world;world.Slab(3,5,kLow,-1,Air());   // 6. a gap three blocks wide down to the bottom of the known world
   golden("gap without blocks to place",world,Block({7,0,0}),{0,0,0},{},
      "status=2 edge=1\n");
   golden("gap bridged by placing blocks",world,Block({7,0,0}),{0,0,0},{false,true,8},
      "status=1 g=120.409014 steps=7\n"
      "t0 (0,0,0)->(1,0,0) c=3.563792\n"
      "t0 (1,0,0)->(2,0,0) c=3.563792\n"
      "t2 (2,0,0)->(3,0,0) c=35.384615 place=(3,-1,0)\n"
      "t2 (3,0,0)->(4,0,0) c=35.384615 place=(4,-1,0)\n"
      "t2 (4,0,0)->(5,0,0) c=35.384615 place=(5,-1,0)\n"
      "t0 (5,0,0)->(6,0,0) c=3.563792\n"
      "t0 (6,0,0)->(7,0,0) c=3.563792\n");}

  {TestWorld world;world.Slab(3,3,0,kHigh,Stone(1.5f));   // 7. a breakable wall up to the top of the known world
   golden("wall without Allow Break",world,Block({6,0,0}),{0,0,0},{},
      "status=2 edge=1\n");
   golden("wall broken through",world,Block({6,0,0}),{0,0,0},{true,true,0},
      "status=1 g=116.451806 steps=6\n"
      "t0 (0,0,0)->(1,0,0) c=3.563792\n"
      "t0 (1,0,0)->(2,0,0) c=3.563792\n"
      "t0 (2,0,0)->(3,0,0) c=98.632847 brk=(3,1,0)(3,0,0)\n"
      "t0 (3,0,0)->(4,0,0) c=3.563792\n"
      "t0 (4,0,0)->(5,0,0) c=3.563792\n"
      "t0 (5,0,0)->(6,0,0) c=3.563792\n");}

  {TestWorld world;world.Slab(3,3,0,kHigh,Stone(-1.f));world.Set({3,0,0},Door());world.Set({3,1,0},Door());   // 8. a door in an unbreakable wall
   golden("closed door opened",world,Block({6,0,0}),{0,0,0},{},
      "status=1 g=30.451806 steps=6\n"
      "t0 (0,0,0)->(1,0,0) c=3.563792\n"
      "t0 (1,0,0)->(2,0,0) c=3.563792\n"
      "t0 (2,0,0)->(3,0,0) c=12.632847 door=(3,0,0)\n"
      "t0 (3,0,0)->(4,0,0) c=3.563792\n"
      "t0 (4,0,0)->(5,0,0) c=3.563792\n"
      "t0 (5,0,0)->(6,0,0) c=3.563792\n");
   golden("closed door with doors off",world,Block({6,0,0}),{0,0,0},{false,false,0},
      "status=2 edge=1\n");}

  {TestWorld world;world.Slab(3,kHalf,0,3,Stone(-1.f));for(int y=0;y<=3;++y)world.Set({2,y,0},Ladder());   // 9. a four-block cliff with a ladder
   golden("ladder up a cliff",world,Block({5,4,0}),{0,0,0},{},
      "status=1 g=49.583327 steps=8\n"
      "t0 (0,0,0)->(1,0,0) c=3.563792\n"
      "t0 (1,0,0)->(2,0,0) c=3.563792\n"
      "t8 (2,0,0)->(2,1,0) c=8.510638\n"
      "t8 (2,1,0)->(2,2,0) c=8.510638\n"
      "t8 (2,2,0)->(2,3,0) c=8.510638\n"
      "t3 (2,3,0)->(3,4,0) c=9.796244\n"
      "t0 (3,4,0)->(4,4,0) c=3.563792\n"
      "t0 (4,4,0)->(5,4,0) c=3.563792\n");}

  {TestWorld world;world.Slab(3,5,-2,-1,Water());   // 10. a water channel two blocks deep, three wide
   golden("water channel crossed",world,Block({7,0,0}),{0,0,0},{},
      "status=1 g=47.917009 steps=7\n"
      "t0 (0,0,0)->(1,0,0) c=3.563792\n"
      "t0 (1,0,0)->(2,0,0) c=3.563792\n"
      "t4 (2,0,0)->(3,-1,0) c=9.247571\n"
      "t0 (3,-1,0)->(4,-1,0) c=9.090909\n"
      "t0 (4,-1,0)->(5,-1,0) c=9.090909\n"
      "t3 (5,-1,0)->(6,0,0) c=9.796244\n"
      "t0 (6,0,0)->(7,0,0) c=3.563792\n");}

  std::printf("%s\n",failed?"path finder golden checks FAILED":"path finder golden checks passed");return failed?1:0;
}
