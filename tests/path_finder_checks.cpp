// Path Finder search regressions, run on a synthetic world: every cell the search can query is pre-filled in the world
// cache (PfWorld::cells), so no game memory is read. Checks:
// - a goal inside a breakable block is reached by digging into it, and the break-aware estimate (PfGoalEntry) stays a
//   lower bound of the real entry cost;
// - a goal behind a long wall is found through the detour, never reported as proven unreachable;
// - a sealed goal is proven unreachable (every reachable cell expanded without touching the edge of the known world);
// - PfGoalProblem refuses goals in unbreakable blocks and in the air, and accepts a goal inside a breakable block.
#include "game_movement_e.hpp"
#include <cstdio>

namespace {
using namespace game;
constexpr int kHalf=40,kLow=-4,kHigh=12;   // the pre-filled box: x, z in [-kHalf, kHalf], y in [kLow, kHigh]
PfRec Air(){PfRec r;r.known=1;r.passable=1;r.replace=1;r.hard=-1.f;return r;}
PfRec Stone(float hard){PfRec r;r.known=1;r.solid=1;r.top=1.f;r.place=1;r.hard=hard;return r;}
struct TestWorld{
 PfWorld w;
 TestWorld(){w.chunkAt=0;w.ocx=w.ocz=0;w.radius=64;w.minY=-64;w.maxY=319;
  // flat world: unbreakable floor at y = -1 and below, air above
  for(int x=-kHalf;x<=kHalf;++x)for(int z=-kHalf;z<=kHalf;++z)for(int y=kLow;y<=kHigh;++y)Set({x,y,z},y<0?Stone(-1.f):Air());}
 void Set(const PfP& p,const PfRec& r){w.cells[PfKey(p)]=r;}
};
PfSearch Run(TestWorld& world,const PfGoal& goal,const PfP& start,bool brk,double ms=20000){
 PfSearch S;S.view.w=&world.w;S.view.o=PfOpt();S.view.o.brk=brk;S.view.o.place=false;S.view.o.parkour=false;S.goal=goal;S.goal.extra=PfGoalEntry(S.view,S.goal);
 S.start=start;S.grounded=true;S.greed=1.25;S.maxMs=ms;S.cap=0;world.w.edge=false;PfSearchBegin(S);
 while(S.status==0&&ActionMs(S.began)<S.maxMs)PfSearchStep(S,256);return S;}
PfGoal Block(const PfP& p){PfGoal g;g.kind=0;g.p=p;return g;}
bool Same(const PfP& a,const PfP& b){return a.x==b.x&&a.y==b.y&&a.z==b.z;}
}

int main(){
 int failed=0;auto check=[&](bool ok,const char* name){std::printf("%s %s\n",ok?"ok  ":"FAIL",name);if(!ok)++failed;};

 {  // 1. dig-in goal: (6,0,0) and (6,1,0) are breakable stone (hardness 1.5), the player walks on the floor
  TestWorld world;world.Set({6,0,0},Stone(1.5f));world.Set({6,1,0},Stone(1.5f));
  PfView v;v.w=&world.w;v.o=PfOpt();v.o.brk=true;v.o.place=false;
  check(PfGoalProblem(v,{6,0,0}).empty(),"a goal inside a breakable block is accepted");
  PfSearch S=Run(world,Block({6,0,0}),{0,0,0},true);
  check(S.status==1&&Same(S.nodes[S.endNode].p,{6,0,0}),"a goal inside a block is reached by digging into it");
  const double entry=PfBreakCost(v,{6,0,0})+PfBreakCost(v,{6,1,0});
  check(S.goal.extra>0&&S.goal.extra<=entry+1e-9,"the break-aware estimate stays a lower bound of the entry cost");
  check(S.status==1&&S.nodes[S.endNode].g>=S.goal.extra,"the path into the goal pays at least the estimated entry");
  PfSearch none=Run(world,Block({6,0,0}),{0,0,0},false);
  check(none.status!=1,"without Allow Break the block goal is not reached");
  PfView off=v;off.o.brk=false;check(!PfGoalProblem(off,{6,0,0}).empty(),"without Allow Break the goal is refused with a reason");}

 {  // 2. long detour: an unbreakable wall at x = 3 from z = -30 to z = 30, the goal behind it; the way round is ~70 blocks
  TestWorld world;for(int z=-30;z<=30;++z)for(int y=0;y<=6;++y)world.Set({3,y,z},Stone(-1.f));
  PfSearch S=Run(world,Block({6,0,0}),{0,0,0},true);
  check(S.status==1&&Same(S.nodes[S.endNode].p,{6,0,0}),"a goal behind a long wall is reached through the detour");
  check(!PfProven(S,world.w,false),"a goal with a detour is never proven unreachable");
  PfSearch quick=Run(world,Block({6,0,0}),{0,0,0},true,0.01);   // a search that runs out of time proves nothing
  check(!PfProven(quick,world.w,false),"a timed-out search does not prove the goal unreachable");}

 {  // 3. sealed: the goal sits in a closed unbreakable room inside the player's closed area (all walls unbreakable)
  TestWorld world;
  for(int x=-kHalf;x<=kHalf;++x)for(int z=-kHalf;z<=kHalf;++z)for(int y=0;y<=kHigh;++y){
   const bool area=std::abs(x)<=10&&std::abs(z)<=10&&y<=4,shell=x>=4&&x<=8&&z>=-2&&z<=2,room=x>=5&&x<=7&&z>=-1&&z<=1&&y<=2;
   world.Set({x,y,z},(area&&!shell)||room?Air():Stone(-1.f));}
  PfSearch S=Run(world,Block({6,0,0}),{0,0,0},true);
  check(PfProven(S,world.w,false),"a sealed goal is proven unreachable");
  check(!PfProven(S,world.w,true),"a later segment's search proves nothing (it does not start at the player)");
  {PfSearch air=S;air.grounded=false;check(!PfProven(air,world.w,false),"a search from a cell in the air proves nothing");}
  {PfSearch placing=S;placing.cap=2;placing.view.o.place=true;check(!PfProven(placing,world.w,false),"a search limited by a placement budget proves nothing");}
  TestWorld open;   // the same room in the open world: the search reaches the edge of the known box, so nothing is proven
  for(int x=4;x<=8;++x)for(int z=-2;z<=2;++z)for(int y=0;y<=kHigh;++y){const bool room=x>=5&&x<=7&&z>=-1&&z<=1&&y<=2;open.Set({x,y,z},room?Air():Stone(-1.f));}
  PfSearch distant=Run(open,Block({6,0,0}),{0,0,0},true,3000);
  check(!PfProven(distant,open.w,false),"an unreachable goal in a world that goes on is not proven unreachable");}

 {  // 4. goal problems
  TestWorld world;world.Set({2,0,2},Stone(-1.f));PfView v;v.w=&world.w;v.o=PfOpt();v.o.brk=true;v.o.place=false;
  check(!PfGoalProblem(v,{2,0,2}).empty(),"a goal in an unbreakable block is refused");
  check(!PfGoalProblem(v,{2,5,2}).empty(),"a goal in the air is refused");
  check(PfGoalProblem(v,{2,0,3}).empty(),"a goal on the ground is accepted");}

 std::printf("%s\n",failed?"path finder checks FAILED":"path finder checks passed");return failed?1:0;}
