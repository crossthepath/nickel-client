// Movement group E: Path Finder and Pearl Chaser.
//
// ---- Path Finder (LevelTick on the game thread, SprintIntent for the movement keys)
// World model: a lazy per-position cache of block records over the chunks within Cache Radius of the player (outside them,
// in unloaded chunks or outside the world height a cell is unknown: impassable). A record is built once per block state
// from the block name, the BlockLegacy material byte / flag byte (+0x28 / +0x29), the "open_bit" state and the block's
// collision shapes (BlockSource vtable +0xb0 queried with the cell box, shapes of that cell without the flag at +0x2c):
//   liquid (flag 2): passable, replaceable, water 1 / lava 2 ("lava" in the name), never broken;
//   door / fence gate (not trapdoor, not iron): passable when open, otherwise a closed door;
//   climbable (ladder, vine, cave / twisting / weeping vines): passable; web: blocked;
//   shapes higher than 0.25: blocked, standable when the top is <= 1.001 and the union covers >= 0.5 of the cell,
//   height min(top, 1); otherwise passable. Replaceable = air or fern / deadbush / seagrass / tall grass / large fern /
//   snow layer / short grass / structure void. Hazard = flag 0x20, material 10, or fire / magma / campfire / cactus /
//   sweet berry bush / wither rose / powder snow. Speed factor 0.4 (soul sand, honey), 0.6 (slime), 1.25 (friction > 0.95:
//   ice family) else 1. Gravity: sand, red sand, gravel, anvil, suspicious sand / gravel, concrete powder. Hardness =
//   (ticks - 0.01) / 30 where ticks = break ticks with the best hotbar tool (game destroy progress); Protected Blocks
//   (exact ids) and liquids are -1 (never broken). A liquid in the second block layer replaces a passable non-climbable cell.
//   A* break time = max(1, ceil(1.5 x hardness x 20)).
// Search: weighted A* (Search Greed) over a hash map of nodes, open heap ordered by f then h; moves: traverse / diagonal /
// bridge / ascend / descend / fall (Max Fall, water-bucket clutch up to Max Clutch Fall, fall cap 24) / parkour / pillar /
// ladder / swim / dig down. Cost factors: cells within the avoid regions (squared distance <= radius^2) multiply the step
// cost: hostile mobs (ActorType & 0xb00 == 0xb00, within Cache Radius x 16 blocks) radius 4 factor 6, mob / trial
// spawners (sub-chunk palettes within Cache Radius chunks, nearest 16, refreshed every 40 ticks) radius 8 factor 4, and
// cells where the player got stuck in the last 1200 ticks radius 0 factor 20. Partial result: the best of seven
// coefficient scores farther than 5 blocks from the start (near the goal: the best estimate when it is a block closer than
// the start); the path then ends "incomplete" and a new search starts from its end. A block goal adds the least cost of
// breaking into its cell and head cell to every other estimate. A search that expands every reachable cell without
// touching an unloaded chunk or the Cache Radius proves the goal unreachable. The search runs in 4 ms slices on the tick
// thread up to Search Time.
// Execution (per tick): steps already reached (the next four are checked) are dropped; a finished complete path is
// "arrived". A step first breaks its listed cells (face chosen by view direction and free neighbours, aim within 12 deg,
// start / continue destroy through the game mode, 600 tick limit, best hotbar tool; Save Tools keeps tools with more than
// 10 durability and stops when only a worn tool would be faster), opens its door (use on block, 10 tick cooldown), places
// its support block (Throwaway Blocks from the hotbar, placement face search, aim within 12 deg, 3 tick cooldown, fails
// after 8 / 12 tries), standing towards the start cell meanwhile. Otherwise it steers: the yaw turns by at most Turn
// Speed per tick towards the step (or the next step when both are walks and the current one is within 0.55), the move
// vector is relative to that yaw (shortened near the end), sprint on walk / ascend / parkour steps when allowed, sneak on
// bridges and down swims, jump for pillars / ladders / swim up / in water / ascends and parkour; the view pitches to the
// first step at least 3 blocks away (+1.2, clamped -20..40). Long falls with the bucket flag place water on the highest
// full block under the player (within 12 blocks, aim within 12 deg, before the fall speed would make it late) and pick
// it up again on landing. Stuck 100 ticks -> stuck spot + new search; far from the path -> new search; a step that is no
// longer valid (checked every 10 ticks) -> new search; new searches are at least 500 ms apart. Progress watchdog: 200 ticks
// of steering without entering a cell not visited in the last 600 ticks or getting a block closer -> stuck spot + new
// search; the third time in a row the goal is given up with the place and the move that kept failing.
// Movement keys are written into the SprintIntent input (strafe / forward from the difference between the steering yaw
// and the head yaw, 0.3 thresholds, sprint when 0.9 x length <= forward, jump bits 7 / 25 / 26 and flag 0x10, sneak bits
// 0 / 21); on the ground a jump is requested through the jump hook instead. Pause On Input pauses for 1500 ms after a
// movement key the player holds (the held-keys word at +0x10, not the effective word the modules write). Rotations: Visible = mode 1, Silent = mode 2, priority 3; Smoothing s > 0 (visible only) gives speed
// max(30 - 27 s, 3) on a persistent request.
// Processes (one tick each, giving: continue / finished / failed, a goal or none, break and place permissions, a status):
//   Goto (position, near, column, level, waypoint, last death, nearest axis): a block goal inside a block is dug into, one
//   in the air moves down to the ground up to 3 below, and one that cannot be stood in (lava, unbreakable block, no room,
//   nothing to stand on) is refused with the reason. Done when arrived (search cell or feet
//   cell); fails at once when proven unreachable or given up by the watchdog, else on the third no-path, saying why.
//   Wander: random column goal at radius x 0.5..1 in a random direction, renewed when reached or without a path.
//   Follow: the named player's cell (kept while the player is airborne); stop within Follow Distance (1.5 more to start
//   again), new near goal when the player moved more than max(distance / 2, 2) and 500 ms passed; dead / missing: wait or go
//   to the last seen cell once; 30 s lost -> finished.
//   Mine / Chop: targets from the sub-chunk palettes within Cache Radius x 16 (nearest 96, without blocks skipped for 600 ticks,
//   24 kept, rescanned every 20 ticks); "next to any target" goal; a target in reach with a free side is broken in place (3
//   attempts, then skipped), drops are walked over for 60 ticks, Fast Mine takes the target nearest to the view and skips the
//   drops; no target: random column goal within 48 blocks; Junk Items dropped (When Full / Always) with Inventory::dropSlot
//   every 1000 ms, otherwise Stop When Full; count reached -> finished. Partial paths: next segment searched 1500 ms later.
//   Tunnel: 1x2 along the facing axis, liquid next to the next cells or no path stops it, goal up to 8 blocks ahead.
//   Build (".build"): needs a shown, resolved schematic (game_schematic.hpp), else fails "Load and show a schematic before
//   building.". Every 20 ticks the cells of the shown schematics that differ from the world are listed (whole box, layer 0:
//   a non-air palette entry whose resolved block is not in the world, or an air entry over a block that is neither air nor
//   liquid -> clear it), sorted by Y then by horizontal distance to the player cell, nearest 256 kept; cells holding our own
//   path support blocks (scaffolding, taken from the executor's placed cells; forgotten once air / liquid / the schematic
//   block) are left out, and the missing-material counts are reset. Target choice: skip cells remembered as failed (400
//   ticks); a cell to fill needs a matching stack in the inventory (else its block name is counted as missing) and prefers a
//   solid neighbour; the first one that can be acted on now is taken, else the first reachable-by-walking one. None ->
//   fails "Missing materials: name xN, ..." ("-" when empty). Acting now = the player box is outside the cell and, from the
//   eye, the cell centre is within reach and either a free face of a block to clear is visible (game clip toward the face
//   point 0.4 out) or a placement face passes the search (allowed faces from pillar_axis, slab / stair half from
//   vertical_half / upside_down_bit -> click height 0.25 / 0.75, line-of-sight clip). Otherwise the goal is the set of
//   stand cells within ceil(reach) (two passable non-liquid cells, not the target column, floor solid or passable, eye
//   x+0.5 / y+1.62 / z+0.5 within reach + 1 and passing the same test), solid-floor cells preferred; none, or no path /
//   arrived on that goal -> the cell is remembered as failed. Placing: the stack goes to the hotbar first (main inventory
//   slot -> hotbar slot 8 through the inventory scheduler, 1000 ms apart, 3 tries per block), then the executor's place
//   routine with the block's facing as a fixed yaw (cardinal_direction / weirdo_direction), click height and allowed faces;
//   a failed constrained placement is retried unconstrained. Wrong blocks are broken first (Allow Break). Done -> clear the
//   scaffolding (highest first, broken in reach when visible, else walked to), then "Schematic complete (N block(s) placed).".
//   Search permissions: no breaking, placing allowed.
// Auto Eat starts eating (hunger <= 14, food from the hotbar, 1000 ms between meals) before the process tick. The last
// death is the start cell when the health attribute reaches 0.
//
// ---- Pearl Chaser (PacketReceive AddActor, LevelTick, action queue, rotation manager)
// Enemy ender pearls (AddActorPacket 0x0d, type ender_pearl) are remembered with their position and velocity. Each tick, with no
// target fixed and no sequence in the last 1000 ms and a pearl in the inventory: the thrower is the player nearest to the pearl
// (within Max Distance of the local player; bots, teammates unless Teammates, and the local player are skipped), the landing point
// comes from the pearl flight simulation (game block clip), and a target is fixed when the pearl hits and the point is at least
// 3 blocks from the eye. A sequence is queued: Reaction Delay x 50 ms, wait for the rotation to be reached, find ender_pearl
// (silent switch with Silent Switch), use, switch back. Every tick with a target the throw angles come from the closed-form
// ballistic solver (200 fractional-tick steps, 20 bisection steps) checked by simulation within Tolerance; otherwise a pitch scan
// from -89 to 89 deg in 2 deg steps, then +-2 deg in 0.25 deg steps around the previous pitch, with the yaw towards the landing
// point. The rotation is requested at priority 3, mode 2 with Silent Aim and mode 0 without; no solution cancels the sequence.
#pragma once
#include "game_combat.hpp"
#include "game_world.hpp"
#include "game_sus.hpp"
#include "game_bed_breaker.hpp"
#include "game_trajectory.hpp"
#include "game_auto_anchor.hpp"
#include "game_movement_b.hpp"
#include "game_player.hpp"
#include "game_movement_sim.hpp"
#include "game_schematic.hpp"
#include <algorithm>
#include <map>
#include <array>
#include <cctype>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <random>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace game {
constexpr int kRotPathFinder=19,kRotPearlChaser=20,kActPathFinder=18,kActPearlChaser=19;

// ============================================================================================ settings and shared state
struct PfConfig{bool allowBreak=true,allowPlace=true,allowParkour=true,allowSprint=true,avoidMobs=true,pauseOnInput=true,bucketClutch=true,fastMine=false,stopWhenFull=true,saveTools=true,autoEat=true,renderPath=true,showThinking=true,throughWalls=true,silentRotations=false;
 int maxFall=3,maxClutchFall=40,searchTime=2000,cacheRadius=8,wanderRadius=96,junkMode=0;float smoothing=50,turnSpeed=45,searchGreed=1.25f,followDistance=3;
 std::vector<std::string> throwaway,protectedBlocks,mineBlocks,junkItems,waypoints;float pathColor[4]{1,.541f,.239f,1},thinkingColor[4]{1,.541f,.239f,.5f},goalColor[4]{1,.698f,.478f,1};};
struct PfCommandRequest{int kind=0;int pos[3]{};int radius=0;std::string name;int count=0;std::vector<std::string> blocks;};   // 1 goto 2 follow 3 mine 4 wander 5 tunnel 6 chop 7 stop 8 pause 9 resume 10 recalculate 11 goto death 12 goto axis 13 build
struct PfShared{std::mutex guard;PfConfig cfg;std::atomic<bool> enabled{false};std::deque<PfCommandRequest> queue;
 int state=0,process=0,remaining=0;std::string detail;   // status: 0 Idle 1 Planning 2 Moving 3 Acting 4 Paused
 std::vector<std::array<float,3>> pathPoints,thinkPoints;bool hasGoal=false;float goalBox[6]{};PfConfig drawCfg;
 // executor input, written on the tick thread, applied in SprintIntent
 std::atomic<bool> active{false},sprint{false},jump{false},sneak{false},holdJump{false},lastJump{false};std::atomic<float> yaw{0},mvX{0},mvZ{0};
 // Pause On Input: last time a movement key was held (SprintIntent)
 std::atomic<int64_t> keyNs{0};std::atomic<bool> keyPause{false},pauseOnInput{true};volatile long fault=0;};
inline PfShared& PathFinder(){static auto* s=new PfShared;return *s;}
inline void(*volatile pfSay)(const std::string&)=nullptr;   // chat feedback (wire to ScSay)
inline void PfMessage(const std::string& text){if(auto f=pfSay)f(text);else if(actionLog)actionLog(("Path Finder: "+text).c_str());}

// ============================================================================================ world model
struct PfP{int x=0,y=0,z=0;};
inline bool operator==(const PfP& a,const PfP& b){return a.x==b.x&&a.y==b.y&&a.z==b.z;}
inline bool operator!=(const PfP& a,const PfP& b){return !(a==b);}
inline PfP PfAdd(const PfP& a,int dx,int dy,int dz){return PfP{a.x+dx,a.y+dy,a.z+dz};}
inline uint64_t PfKey(const PfP& p){return ((uint64_t)(uint32_t)(p.x&0x3ffffff)<<38)|((uint64_t)(uint32_t)(p.z&0x3ffffff)<<12)|(uint64_t)((p.y+64)&0xfff);}
// Record fields: passable, solid = standable, top = standing height, speed factor, climbable, closed door, replaceable,
// place = full solid face to place against, gravity, avoid = hazard, cls = liquid (1 water, 2 lava), hard = hardness.
struct PfRec{uint8_t known=0,passable=0,solid=0,climb=0,door=0,replace=0,place=0,gravity=0,avoid=0,cls=0;float top=1.f,speed=1.f,hard=-1.f;uint64_t block=0;};
struct PfWorld{Context c{};uintptr_t actor=0;uint64_t region=0,chunkAt=0,layerAt=0,fetch=0,gm=0,level=0;int ocx=0,ocz=0,radius=8,minY=-64,maxY=319;bool edge=false;   // edge: a query reached an unloaded chunk or the Cache Radius
 std::unordered_map<uint64_t,PfRec> cells,byBlock;std::unordered_map<uint64_t,BbCost> breakCost;std::unordered_map<uint64_t,int8_t> chunks;std::vector<std::string> protectedNames;};
inline uint64_t PfBlockRaw(const Context& c,uint64_t region,int x,int y,int z){const int32_t p[3]={x,y,z};__try{return RegionBlock(c,region,p);}__except(EXCEPTION_EXECUTE_HANDLER){return 0;}}
inline uint64_t PfLayerRaw(uint64_t fn,uint64_t region,int x,int y,int z){const int32_t p[3]={x,y,z};__try{return reinterpret_cast<uint64_t(*)(uint64_t,const int32_t*)>(fn)(region,p);}__except(EXCEPTION_EXECUTE_HANDLER){return 0;}}
inline bool PfChunkRaw(uint64_t fn,uint64_t region,int cx,int cz){int cp[2]={cx,cz};__try{return reinterpret_cast<uint64_t(*)(uint64_t,int*)>(fn)(region,cp)!=0;}__except(EXCEPTION_EXECUTE_HANDLER){return false;}}
inline bool PfFetchRaw(uint64_t fn,uint64_t region,uint64_t* vec,const float* box){__try{reinterpret_cast<void(*)(uint64_t,void*,const float*,int,uint64_t,uint64_t)>(fn)(region,vec,box,0,0,0);return true;}__except(EXCEPTION_EXECUTE_HANDLER){return false;}}
inline std::string PfLower(std::string s){for(auto& ch:s)ch=(char)std::tolower((unsigned char)ch);return s;}
inline std::string PfFullId(std::string s){s=PfLower(s);if(!s.empty()&&s.find(':')==std::string::npos)s="minecraft:"+s;return s;}
// Collision shapes of one cell, relative to the cell: BlockSource vtable +0xb0 (box = the cell), shapes of this cell only, flag +0x2c clear.
struct PfBox{float v[6];};
inline bool PfShapes(PfWorld& w,const PfP& p,std::vector<PfBox>& out){
 out.clear();if(!w.fetch)return false;static uint64_t buf[3]{};constexpr uint64_t elements=0x400,stride=0x30;
 if(!buf[0]){const uint64_t b=MoveBAllocate(elements*stride);if(!b)return false;buf[0]=buf[1]=b;buf[2]=b+elements*stride;}
 buf[1]=buf[0];const float box[6]={(float)p.x,(float)p.y,(float)p.z,(float)p.x+1.f,(float)p.y+1.f,(float)p.z+1.f};
 if(!PfFetchRaw(w.fetch,w.region,buf,box)){buf[1]=buf[0];return false;}
 for(uint64_t e=buf[0];e<buf[1];e+=stride){int32_t bp[3]{};uint8_t flagged{};float s[6]{};
  if(!Read(e+0x2c,&flagged)||!Floats(e+8,s,6)||!Read(e+0x20,&bp[0])||!Read(e+0x24,&bp[1])||!Read(e+0x28,&bp[2]))return false;
  if(bp[0]!=p.x||bp[1]!=p.y||bp[2]!=p.z||flagged)continue;out.push_back(PfBox{{s[0]-p.x,s[1]-p.y,s[2]-p.z,s[3]-p.x,s[4]-p.y,s[5]-p.z}});}
 return true;}
// Static block properties by id: friction, bounce (bed 2, slime 1), hazard (powder snow 2, sweet berry bush 1), climbable, web, honey.
struct PfProps{float friction=0.6f;uint8_t bounce=0,hazard=0;bool climb=false,web=false,honey=false;};
inline PfProps PfBlockProps(const std::string& n){PfProps r;
 static const char* climb[]={"minecraft:ladder","minecraft:vine","minecraft:cave_vines","minecraft:cave_vines_body_with_berries","minecraft:cave_vines_head_with_berries","minecraft:twisting_vines","minecraft:weeping_vines"};
 for(auto t:climb)if(n==t)r.climb=true;r.web=n=="minecraft:web";r.honey=n=="minecraft:honey_block";
 static const std::pair<const char*,float> slip[]={{"minecraft:ice",.98f},{"minecraft:frosted_ice",.98f},{"minecraft:packed_ice",.98f},{"minecraft:blue_ice",.989f},{"minecraft:slime",.8f},{"minecraft:honey_block",.8f}};
 for(auto& e:slip)if(n==e.first){r.friction=e.second;break;}
 if(n=="minecraft:bed")r.bounce=2;else if(n=="minecraft:slime")r.bounce=1;
 if(n=="minecraft:powder_snow")r.hazard=2;else if(n=="minecraft:sweet_berry_bush")r.hazard=1;return r;}
inline bool PfOpenBit(const Context& c,uint64_t block,bool* open){const uint64_t states=block?SusTag(c,block+0xf0,"states",10):0;const uint64_t tag=states?SusTag(c,states,"open_bit",1):0;
 if(!tag)return false;uint8_t v{};if(!Read(tag+8,&v))return false;*open=v!=0;return true;}
inline PfRec PfClassify(PfWorld& w,uint64_t block,const PfP& p){
 PfRec r;uint8_t a{},b{};if(!block||!BlockFlags(block,&a,&b))return r;std::string n;XrayBlockName((uintptr_t)block,&n);r.known=1;r.block=block;
 uint8_t mat=3;if(!(b&2)){if((uint8_t)(a-8)<2)mat=2;else if(a==0)mat=0;else if(a==10)mat=4;else mat=(b&0x10)?1:5;}
 const bool f21=(b>>4)&1,f22=(b>>2)&1,f23=b&1,f24=(b>>5)&1;bool open=false;const bool hasOpen=PfOpenBit(w.c,block,&open);
 BbCtx x;x.c=w.c;x.actor=w.actor;x.region=w.region;x.gm=w.gm;const BbCost bc=w.gm?BbCostOf(x,w.breakCost,block):BbCost{};
 std::vector<PfBox> shapes;PfShapes(w,p,shapes);const PfProps pr=PfBlockProps(n);
 float top=0,area=0;if(!shapes.empty()){float mnx=shapes[0].v[0],mnz=shapes[0].v[2],mxx=shapes[0].v[3],mxz=shapes[0].v[5];top=shapes[0].v[4];
  for(auto& s:shapes){top=std::max(top,s.v[4]);mnx=std::min(mnx,s.v[0]);mnz=std::min(mnz,s.v[2]);mxx=std::max(mxx,s.v[3]);mxz=std::max(mxz,s.v[5]);}area=(mxx-mnx)*(mxz-mnz);}
 const auto has=[&](const char* t){return n.find(t)!=std::string::npos;};
 uint8_t liquid=0;if(mat==3)liquid=(n.size()>=4&&has("lava"))?2:1;
 bool hazard=true;if(!f24&&mat!=4){static const char* bad[]={"fire","magma","campfire","cactus","sweet_berry_bush","wither_rose","powder_snow"};bool any=false;for(auto t:bad)if(has(t))any=true;if(!any)hazard=pr.hazard!=0;}
 r.speed=(n=="minecraft:soul_sand"||pr.honey)?.4f:(pr.bounce==1?.6f:(pr.friction>.95f?1.25f:1.f));
 r.climb=pr.climb;r.place=!f23&&liquid==0&&f22&&f21;
 static const char* grav[]={"minecraft:sand","minecraft:anvil","minecraft:gravel","minecraft:red_sand","minecraft:suspicious_sand","minecraft:suspicious_gravel"};
 for(auto t:grav)if(n==t)r.gravity=1;if(has("concrete_powder"))r.gravity=1;
 r.avoid=hazard;r.cls=liquid;r.hard=bc.ok?((float)bc.ticks-0.01f)/30.f:-1.f;r.top=1.f;
 if(liquid){r.passable=1;r.replace=1;r.hard=-1.f;}
 else if(n.size()>3&&((has("door")&&(n.size()<8||!has("trapdoor")))||(n.size()>9&&has("fence_gate")))&&!has("iron")){r.passable=hasOpen&&open;r.door=!r.passable;}
 else{bool rep=true;if(mat!=0){static const char* plants[]={"minecraft:fern","minecraft:deadbush","minecraft:seagrass","minecraft:tall_grass","minecraft:large_fern","minecraft:snow_layer","minecraft:short_grass","minecraft:structure_void"};
   rep=false;for(auto t:plants)if(n==t)rep=true;}
  r.replace=rep;bool blocked=false;
  if(!pr.climb){if(pr.web)blocked=true;else if(!shapes.empty()&&top>0.25f){r.solid=top<=1.001f&&area>=0.5f;r.top=std::min(top,1.f);blocked=true;}}
  if(!blocked)r.passable=1;}
 for(auto& t:w.protectedNames)if(n==t)r.hard=-1.f;
 return r;}
inline const PfRec& PfRecOf(PfWorld& w,uint64_t block,const PfP& p){auto f=w.byBlock.find(block);if(f==w.byBlock.end())f=w.byBlock.emplace(block,PfClassify(w,block,p)).first;return f->second;}
inline const PfRec& PfRecAt(PfWorld& w,const PfP& p){
 static const PfRec unknown;const uint64_t key=PfKey(p);auto f=w.cells.find(key);if(f!=w.cells.end())return f->second;
 const int cx=p.x>>4,cz=p.z>>4;if(p.y<w.minY||p.y>w.maxY)return unknown;if(std::abs(cx-w.ocx)>w.radius||std::abs(cz-w.ocz)>w.radius){w.edge=true;return unknown;}
 const uint64_t ck=((uint64_t)(uint32_t)cx<<32)|(uint32_t)cz;auto cf=w.chunks.find(ck);int8_t loaded;
 if(cf==w.chunks.end()){loaded=w.chunkAt&&PfChunkRaw(w.chunkAt,w.region,cx,cz)?1:0;w.chunks.emplace(ck,loaded);}else loaded=cf->second;
 if(!loaded){w.edge=true;return unknown;}
 const uint64_t block=PfBlockRaw(w.c,w.region,p.x,p.y,p.z);PfRec use=PfRecOf(w,block,p);
 if(use.known&&use.passable&&!use.climb&&w.layerAt){const uint64_t extra=PfLayerRaw(w.layerAt,w.region,p.x,p.y,p.z);if(extra&&extra!=block){const PfRec& er=PfRecOf(w,extra,p);if(er.cls)use=er;}}
 return w.cells.emplace(key,use).first->second;}
inline void PfWorldBegin(PfWorld& w,const Context& c,const PfConfig& cfg,int px,int pz){
 w.cells.clear();w.chunks.clear();w.byBlock.clear();w.breakCost.clear();w.protectedNames.clear();w.edge=false;w.c=c;w.actor=c.actor;w.region=ActorRegion(c);
 w.chunkAt=w.region?Virtual(c,w.region,0x148):0;w.layerAt=w.region?Virtual0(c,w.region,0x28):0;w.fetch=w.region?Virtual0(c,w.region,0xb0):0;uint64_t gm{};Read(c.actor+0xac8,&gm);w.gm=gm;
 uint64_t level{};Read(c.actor+0x200,&level);w.level=level;
 int16_t lowest{};if(w.region&&Read(w.region+0x3a,&lowest)){w.minY=lowest;w.maxY=lowest+383;}w.ocx=px>>4;w.ocz=pz>>4;w.radius=std::max(2,cfg.cacheRadius);
 for(auto& t:cfg.protectedBlocks){const std::string id=PfFullId(t);if(id.size()>10)w.protectedNames.push_back(id);}}
// ============================================================================================ move generation
inline constexpr double kPfInf=1000000.0,kPfWalk=4.63284688441047,kPfSprint=3.563791874554526,kPfSneak=15.384615384615383,kPfSwimWalk=9.09090909090909,kPfJump=3.163397248098741,
 kPfLadderUp=8.51063829787234,kPfLadderDown=6.666666666666667,kPfFallOne=4.614724090637822,kPfHalf=0.5625,kPfMaxStep=1.25,kPfSqrt2=1.4142135623730951,kPfVertSwim=10.0,kPfDoor=4.0,kPfFallTail=3.7062775075283763+0.9265693768820937,kPfFallHuge=1000004.6328468843;
inline double PfFallTable(int d){static double t[256];static bool built=false;if(!built){built=true;t[0]=0;for(int i=1;i<256;++i){double tmp=i;for(int k=0;;++k){const double f=(std::pow(0.98,(double)k)-1.0)*-3.92;if(tmp<=f){t[i]=(double)k+tmp/f-1.0;break;}tmp-=f;}}}return t[d];}
inline double PfFall(int d){return d<256?PfFallTable(d)+kPfFallTail:kPfFallHuge;}
struct PfOpt{bool sprint=true,diag=true,parkour=true,sprintJump=true,lava=false,brk=true,place=true,doors=true,swim=true;int maxFall=3,fallCap=24,bucketFall=0,blocks=0;double bucketCost=20,placeCost=20,breakPenalty=2,jumpPenalty=2;};
struct PfRes{int breaks[4][3]{};uint8_t nbreaks=0;bool support=false;PfP supportCell;bool door=false;PfP doorCell;bool bucket=false;};
struct PfMove{PfP pos;uint8_t type=0;double cost=0;PfRes res;};
struct PfView{PfWorld* w=nullptr;PfOpt o;std::unordered_set<uint64_t> avoid;double avoidFactor=3.0;struct Region{PfP c;int r2;double f;};std::vector<Region> regions;};
inline const PfRec& PfRc(PfView& v,const PfP& p){return PfRecAt(*v.w,p);}
inline bool PfFree(PfView& v,const PfP& p){const PfRec& r=PfRc(v,p);return r.passable&&!r.avoid&&(r.cls==2?false:(r.cls==1?v.o.swim:true));}
inline bool PfSwim(PfView& v,const PfP& p){return PfRc(v,p).cls==1&&PfFree(v,p);}
inline bool PfClimb(PfView& v,const PfP& p){return PfRc(v,p).climb&&PfFree(v,p);}
inline bool PfPass2(PfView& v,const PfP& p){return PfFree(v,p)&&PfFree(v,PfAdd(p,0,1,0));}
inline bool PfStand(PfView& v,const PfP& p){const PfRec& r=PfRc(v,p);return r.solid&&!r.avoid;}
inline double PfHeight(PfView& v,const PfP& p){const PfRec& r=PfRc(v,PfAdd(p,0,-1,0));return r.solid&&!r.avoid?(double)r.top:1.0;}
inline double PfSpeed(PfView& v,const PfP& p){const PfRec& r=PfRc(v,PfAdd(p,0,-1,0));return r.solid&&!r.avoid?(double)r.speed:1.0;}
inline bool PfSupport(PfView& v,const PfP& p,const PfP& q){static const int d[6][3]={{0,-1,0},{0,1,0},{0,0,-1},{0,0,1},{-1,0,0},{1,0,0}};
 for(auto& k:d){const PfP n=PfAdd(p,k[0],k[1],k[2]);if(n==q)continue;if(PfRc(v,n).place)return true;}return false;}
// Break time of a record: hardness < 0 never, 0 one tick, else max(1, ceil(1.5 x hardness x 20)) (no tool list, no haste / fatigue).
inline double PfBreakTime(PfView&,const PfRec& r){if(!r.known)return kPfInf;const double h=(double)r.hard;if(h<0.0)return kPfInf;if(h==0.0)return 1.0;return std::max(1.0,std::ceil(1.5*h/1.0*20.0));}
inline double PfBreakCost(PfView& v,const PfP& p){
 if(!v.o.brk||!PfRc(v,p).known)return kPfInf;static const int off[5][3]={{0,1,0},{0,0,-1},{0,0,1},{-1,0,0},{1,0,0}};
 for(auto& k:off)if(PfRc(v,PfAdd(p,k[0],k[1],k[2])).cls!=0)return kPfInf;
 double total=PfBreakTime(v,PfRc(v,p));if(total>=kPfInf)return kPfInf;double extra=0;
 for(int i=1;i<=16;++i){const PfRec& r=PfRc(v,PfAdd(p,0,i,0));if(!r.gravity){extra=v.o.breakPenalty;break;}total+=PfBreakTime(v,r)+v.o.breakPenalty;extra=v.o.breakPenalty;}
 return extra+total;}
// Cost of entering a cell: 0 when free, closed door 4 (once per move), otherwise the break cost; breaks are recorded in the move.
inline double PfCost(PfView& v,const PfP& p,PfRes& res,bool doorOk){
 const PfRec& r=PfRc(v,p);if(r.passable&&!r.avoid&&r.cls!=2&&!(r.cls==1&&!v.o.swim))return 0.0;
 if(!(doorOk&&r.door&&v.o.doors)){const double d=PfBreakCost(v,p);if(d<kPfInf&&res.nbreaks<4){res.breaks[res.nbreaks][0]=p.x;res.breaks[res.nbreaks][1]=p.y;res.breaks[res.nbreaks][2]=p.z;++res.nbreaks;return d;}return kPfInf;}
 if(res.door){if(res.doorCell!=p){res.doorCell=p;return kPfDoor;}return 0.0;}res.door=true;res.doorCell=p;return kPfDoor;}
inline void PfEmit(std::vector<PfMove>& out,const PfP& pos,int type,double cost,const PfRes& res,bool bucket=false){PfMove m;m.pos=pos;m.type=(uint8_t)type;m.cost=cost;m.res=res;m.res.bucket=bucket;out.push_back(m);}
inline void PfMoves(PfView& v,const PfP& s,std::vector<PfMove>& out){
 const PfOpt& o=v.o;static const int card[4][2]={{0,-1},{0,1},{-1,0},{1,0}};
 for(auto& cd:card){const int dx=cd[0],dz=cd[1];const PfP dest{s.x+dx,s.y,s.z+dz};
  // 1. traverse / bridge
  {PfRes r;const double c1=PfCost(v,PfAdd(dest,0,1,0),r,true);
   if(c1<kPfInf){const double c2=PfCost(v,dest,r,true);
    if(c2<kPfInf&&c1+c2<kPfInf){const PfP below=PfAdd(dest,0,-1,0);const PfRec& rb=PfRc(v,below);
     if((rb.solid&&!rb.avoid)||PfSwim(v,dest)||PfClimb(v,dest)){
      const double hd=PfHeight(v,dest),hs=PfHeight(v,s);double step=kPfSwimWalk;
      if(!PfSwim(v,s)&&!PfSwim(v,dest)){double u=kPfWalk;
       if(o.sprint){const double d32=r.door?kPfWalk:kPfSprint;const double d33=r.support?kPfWalk:d32;if(r.nbreaks==0)u=d33;}
       step=u/((PfSpeed(v,s)+PfSpeed(v,dest))*0.5);}
      if(hd-hs>kPfHalf)step+=o.jumpPenalty+kPfJump;
      PfEmit(out,dest,0,c1+c2+step,r);}
     else if(o.place&&o.blocks>0&&PfRc(v,below).known&&PfRc(v,below).replace){r.support=true;r.supportCell=below;PfEmit(out,dest,2,c1+c2+o.placeCost+kPfSneak,r);}}}}
  // 2. ascend
  {PfRes r;const double c1=PfCost(v,PfAdd(s,0,2,0),r,false);
   if(c1<kPfInf){const double c2=PfCost(v,PfAdd(dest,0,2,0),r,false);
    if(c2<kPfInf){const PfP up1=PfAdd(dest,0,1,0);const double c3=PfCost(v,up1,r,false);
     if(c3<kPfInf&&c2+c3<kPfInf){const PfRec& R=PfRc(v,dest);double d30=0,d31=0;bool ok=false;
      if(R.solid&&!R.avoid){d31=(double)R.top+1.0;ok=true;}
      else if(o.place&&o.blocks>0&&R.known&&R.replace&&PfSupport(v,dest,s)){r.support=true;r.supportCell=dest;d30=o.placeCost;d31=2.0;ok=true;}
      if(ok){const double d33=PfHeight(v,s);
       if(d31-d33<=kPfMaxStep){double d34=kPfWalk;if(kPfHalf<d31-d33)d34=o.jumpPenalty+kPfJump+kPfWalk;PfEmit(out,up1,3,d30+c2+c3+c1+d34,r);}}}}}}
  // 3. descend / fall
  {PfRes r;const double c1=PfCost(v,PfAdd(dest,0,1,0),r,false);
   if(c1<kPfInf){const double c2=PfCost(v,dest,r,false);
    if(c2<kPfInf&&c1+c2<kPfInf){const PfP b1=PfAdd(dest,0,-1,0);
     if(!PfStand(v,b1)&&!PfSwim(v,dest)&&!PfClimb(v,dest)){const int maxAll=std::max({o.maxFall,o.fallCap,o.bucketFall});
      if(maxAll>0){const double c3=PfCost(v,b1,r,false);const double total=c1+c2+c3;
       if(total<kPfInf){int d=1;PfP cur=b1;
        for(;;){
         if(PfSwim(v,cur)||PfClimb(v,cur)){if(d<=o.fallCap)PfEmit(out,cur,d==1?4:5,total+PfFall(d),r);break;}
         const PfP under=PfAdd(cur,0,-1,0);const PfRec& ru=PfRc(v,under);
         if(ru.solid&&!ru.avoid){const int type=d==1?4:5;
          if(o.maxFall<d){if(d<=o.bucketFall)PfEmit(out,cur,type,total+PfFall(d)+o.bucketCost,r,true);}else PfEmit(out,cur,type,total+PfFall(d),r);break;}
         if(!(ru.passable&&!ru.avoid))break;
         if(ru.cls==1){if(!(maxAll>d)||!o.swim)break;}else if(ru.cls==2||!(maxAll>d))break;
         ++d;cur=under;}}}}}}}
  // 4. parkour
  if(o.parkour&&!PfSwim(v,s)&&!PfClimb(v,s)&&PfStand(v,PfAdd(s,0,-1,0))){
   const PfP d0=dest;
   if(PfPass2(v,d0)){const PfP d0b=PfAdd(d0,0,-1,0);
    if(!PfStand(v,d0b)&&!PfSwim(v,d0)&&!PfClimb(v,d0)&&PfFree(v,PfAdd(s,0,2,0))){
     const double hs=PfHeight(v,s);const double per=o.sprint?kPfSprint:kPfWalk;
     if(PfFree(v,PfAdd(d0,0,2,0))&&!(!o.lava&&PfRc(v,d0b).cls==2)){
      const int maxk=(o.sprint?1:0)+3;const PfRes none;
      for(int k=2;;++k){const PfP pk{s.x+k*dx,s.y,s.z+k*dz};
       if(k<maxk&&o.sprintJump){const PfRec& rk=PfRc(v,pk);
        if(rk.solid&&!rk.avoid){const PfP pk1=PfAdd(pk,0,1,0);
         if(PfPass2(v,pk1)&&(double)rk.top+1.0-hs<=kPfMaxStep)PfEmit(out,pk1,6,k*per+o.jumpPenalty+kPfJump,none);break;}}
       if(!PfPass2(v,pk))break;const PfP kb=PfAdd(pk,0,-1,0);
       if(PfStand(v,kb)){if(PfHeight(v,pk)<=hs)PfEmit(out,pk,6,k*per+o.jumpPenalty,none);break;}
       if(!PfFree(v,PfAdd(pk,0,2,0)))break;
       if(!o.lava&&PfRc(v,kb).cls==2)break;
       if(k==maxk)break;}}}}}}
 // 5. diagonals
 if(o.diag){static const int dg[4][2]={{-1,-1},{1,-1},{-1,1},{1,1}};
  for(auto& g:dg){const PfP D{s.x+g[0],s.y,s.z+g[1]},Bx{s.x+g[0],s.y,s.z},Bz{s.x,s.y,s.z+g[1]};
   if(!PfPass2(v,D)||!PfPass2(v,Bx)||!PfPass2(v,Bz))continue;
   if(!(PfStand(v,PfAdd(D,0,-1,0))||PfSwim(v,D)||PfClimb(v,D)))continue;
   const double hD=PfHeight(v,D),hs=PfHeight(v,s);double base=kPfSwimWalk;
   if(!PfSwim(v,s)&&!PfSwim(v,D))base=(o.sprint?kPfSprint:kPfWalk)/((PfSpeed(v,D)+PfSpeed(v,s))*0.5);
   double extra=0;if(hD-hs>kPfHalf)extra=o.jumpPenalty+kPfJump;PfEmit(out,D,1,base*kPfSqrt2+extra,PfRes());}}
 // 6. vertical
 const PfP up1=PfAdd(s,0,1,0);PfRes r;
 if(!(PfClimb(v,s)&&PfClimb(v,up1))){
  if(!PfSwim(v,s)){
   if(!PfClimb(v,s)&&o.place&&o.blocks>0&&PfRc(v,s).known&&PfRc(v,s).replace){const double c=PfCost(v,PfAdd(s,0,2,0),r,false);
    if(c<kPfInf){r.support=true;r.supportCell=s;PfEmit(out,up1,7,c+o.jumpPenalty+kPfJump+o.placeCost,r);}}}
  else if(PfSwim(v,up1)&&PfFree(v,PfAdd(s,0,2,0)))PfEmit(out,up1,10,kPfVertSwim,PfRes());}
 else{const double c=PfCost(v,PfAdd(s,0,2,0),r,false);if(c<kPfInf)PfEmit(out,up1,8,c+kPfLadderUp,r);}
 {const PfP dn=PfAdd(s,0,-1,0);PfRes r2;
  if(PfClimb(v,dn))PfEmit(out,dn,9,kPfLadderDown,PfRes());
  else if(PfSwim(v,dn))PfEmit(out,dn,11,kPfVertSwim,PfRes());
  else if(!PfFree(v,dn)){const PfP dn2=PfAdd(s,0,-2,0);
   if(PfStand(v,dn2)){const double c=PfCost(v,dn,r2,false);if(c<kPfInf)PfEmit(out,dn,12,c+kPfFallOne,r2);}}}
}

// ============================================================================================ goals and search
// Goals: 0 block, 1 near (squared radius), 2 column (XZ), 3 level (Y), 4 next to any block of a list (|dx| + |dy'| + |dz| < 2
// with dy' = dy + 1 when dy < 0), 5 drop pick-up (the cell or the one below), 6 axis (x = 0, z = 0 or |x| = |z|, level optional),
// 7 any cell of a set (Build stand cells; estimate = block goal at the target p).
struct PfGoal{int kind=0;PfP p;int radius=0;std::vector<PfP> list;bool hasY=false;std::shared_ptr<const std::unordered_set<uint64_t>> cells;
 double extra=0;   // block goal: least cost of breaking into the goal cell (its blocks), added to the estimate of every other cell
 static double Flat(const PfP& p,const PfP& q){const int a=std::abs(p.x-q.x),b=std::abs(p.z-q.z);return ((double)std::min(a,b)*kPfSqrt2+(double)std::abs(a-b))*kPfSprint;}
 static double Block(const PfP& p,const PfP& q){return Flat(p,q)+(double)std::abs(p.y-q.y)*3.3940392020355534;}
 bool at(const PfP& q)const{switch(kind){case 0:return q==p;case 1:{const int a=p.x-q.x,b=p.y-q.y,c=p.z-q.z;return a*a+b*b+c*c<=radius*radius;}case 2:return q.x==p.x&&q.z==p.z;case 3:return q.y==p.y;
   case 4:for(auto& t:list){int dy=q.y-t.y;dy+=dy<0?1:0;if(std::abs(q.x-t.x)+std::abs(dy)+std::abs(q.z-t.z)<2)return true;}return false;
   case 5:return q==p||(q.x==p.x&&q.y==p.y-1&&q.z==p.z);
   case 7:return cells&&cells->count(PfKey(q))!=0;
   default:if(!(q.x==0||q.z==0||std::abs(q.x)==std::abs(q.z)))return false;return !hasY||q.y==p.y;}}
 double h(const PfP& q)const{const double v=est(q);return extra>0&&!at(q)?v+extra:v;}
 double est(const PfP& q)const{switch(kind){case 2:return Flat(p,q);case 3:return (double)std::abs(p.y-q.y)*3.3940392020355534;
   case 4:{double best=1.7976931348623157e+308;for(auto& t:list)best=std::min(Block(t,q),best);return best;}
   case 5:return Flat(p,q)+(double)std::abs(p.y-q.y-1)*3.3940392020355534;
   case 6:{const int ax=std::abs(q.x),az=std::abs(q.z);const double m=std::min({(double)ax,(double)az,(double)std::abs(ax-az)*0.7071067811865476});return m*kPfSprint+(hasY?(double)std::abs(p.y-q.y)*3.3940392020355534:0.0);}
   case 7:return Block(p,q);
   default:return Block(p,q);}}};
struct PfNode{PfP p;double g=0,h=0;int parent=-1;uint8_t type=0;double step=0;int placed=0;};
struct PfHeapEntry{double f,h,g;uint32_t idx;};
// Path step (0x80 bytes in the original): start cell, target cell, move type, cost, cells to break, support cell, door, bucket flag.
struct PfStep{PfP from,to;uint8_t type=0;double cost=0;PfP brk[4];uint8_t nbrk=0;bool hasPlace=false;PfP place;bool hasDoor=false;PfP door;bool bucket=false;};
inline PfStep PfStepOf(const PfP& from,const PfMove& m){PfStep s;s.from=from;s.to=m.pos;s.type=m.type;s.cost=m.cost;s.nbrk=std::min<uint8_t>(m.res.nbreaks,4);
 for(int i=0;i<s.nbrk;++i)s.brk[i]=PfP{m.res.breaks[i][0],m.res.breaks[i][1],m.res.breaks[i][2]};s.hasPlace=m.res.support;s.place=m.res.supportCell;s.hasDoor=m.res.door;s.door=m.res.doorCell;s.bucket=m.res.bucket;return s;}
struct PfSearch{PfView view;PfGoal goal;PfP start;std::vector<PfNode> nodes;std::unordered_map<uint64_t,uint32_t> index;std::vector<PfHeapEntry> heap;
 int status=0;bool grounded=false;uint32_t endNode=0;double bestVal[7]{};uint32_t bestNode[7]{};double greed=1.25;int64_t began=0;double maxMs=2000;int cap=0;std::vector<PfStep> moves;
 std::vector<std::array<float,3>> sample;};
inline bool PfHeapLess(const PfHeapEntry& a,const PfHeapEntry& b){if(a.f!=b.f)return a.f>b.f;return a.h>b.h;}   // std heap: "less" = worse
inline void PfBest(PfSearch& S,uint32_t i){const PfNode& n=S.nodes[i];static const double div[7]={1.5,0,2.5,3.0,0,5.0,10.0};
 for(int k=0;k<7;++k){const double val=k==1?0.5*n.g+n.h:(k==4?0.25*n.g+n.h:n.g/div[k]+n.h);if(val<S.bestVal[k]){S.bestVal[k]=val;S.bestNode[k]=i;}}}
inline void PfSearchBegin(PfSearch& S){S.nodes.clear();S.index.clear();S.heap.clear();S.status=0;S.began=ActionNs();PfNode n;n.p=S.start;n.h=S.goal.h(S.start)*S.greed;n.g=0;S.nodes.push_back(n);S.index[PfKey(S.start)]=0;
 S.heap.push_back({n.h,n.h,0.0,0});for(int k=0;k<7;++k){S.bestVal[k]=n.h;S.bestNode[k]=0;}}
inline double PfFactor(PfSearch& S,const PfP& p){double f=1.0;if(S.view.avoid.count(PfKey(p)))f=S.view.avoidFactor;
 for(auto& r:S.view.regions){const int a=p.x-r.c.x,b=p.y-r.c.y,c=p.z-r.c.z;if(a*a+b*b+c*c<=r.r2)f*=r.f;}return f;}
inline void PfExpand(PfSearch& S,uint32_t i){std::vector<PfMove> moves;const PfP from=S.nodes[i].p;PfMoves(S.view,from,moves);
 for(auto& m:moves){const int placed=S.nodes[i].placed+(m.res.support?1:0);if(placed>S.cap)continue;
  const double factor=PfFactor(S,m.pos);const double step=factor*m.cost;const double g2=S.nodes[i].g+step;const uint64_t key=PfKey(m.pos);auto f=S.index.find(key);uint32_t idx;
  if(f==S.index.end()){PfNode nn;nn.p=m.pos;nn.h=S.goal.h(m.pos)*S.greed;nn.g=1e300;idx=(uint32_t)S.nodes.size();S.nodes.push_back(nn);S.index.emplace(key,idx);}
  else{idx=f->second;if(S.nodes[idx].g<=g2)continue;}
  PfNode& n=S.nodes[idx];n.g=g2;n.parent=(int)i;n.type=m.type;n.step=step;n.placed=placed;
  S.heap.push_back({g2+n.h,n.h,g2,idx});std::push_heap(S.heap.begin(),S.heap.end(),PfHeapLess);PfBest(S,idx);}}
// One slice: `steps` expansions. Returns the status (0 running, 1 found, 2 failed).
inline int PfSearchStep(PfSearch& S,int steps){
 while(steps-->0&&S.status==0){
  if(S.heap.empty()){S.status=2;break;}
  std::pop_heap(S.heap.begin(),S.heap.end(),PfHeapLess);const PfHeapEntry e=S.heap.back();S.heap.pop_back();
  if(e.g<=S.nodes[e.idx].g){if(S.goal.at(S.nodes[e.idx].p)){S.endNode=e.idx;S.status=1;break;}PfExpand(S,e.idx);if((S.nodes.size()&15)==0&&S.sample.size()<2048)S.sample.push_back({(float)S.nodes[e.idx].p.x+.5f,(float)S.nodes[e.idx].p.y+.1f,(float)S.nodes[e.idx].p.z+.5f});}}
 return S.status;}
// Path reconstruction: walk the parents, regenerate the move of each step (match position and type), reverse.
inline bool PfBuildPath(PfSearch& S,uint32_t endIdx){S.moves.clear();uint32_t cur=endIdx;
 while(S.nodes[cur].parent>=0){const PfNode& n=S.nodes[cur];const PfNode& pn=S.nodes[(size_t)n.parent];std::vector<PfMove> cand;PfMoves(S.view,pn.p,cand);bool found=false;
  for(auto& m:cand)if(m.pos==n.p&&m.type==n.type){S.moves.push_back(PfStepOf(pn.p,m));found=true;break;}
  if(!found){PfMove m;m.pos=n.p;m.type=n.type;S.moves.push_back(PfStepOf(pn.p,m));}cur=(uint32_t)n.parent;}
 std::reverse(S.moves.begin(),S.moves.end());return !S.moves.empty();}
// Partial result: first of the seven bests that is far enough from the start.
// Near the goal none of them may be 5 blocks away: then the node with the best estimate, when it is at least a block closer
// to the goal than the start (without it the path finder gave up right next to a goal it had to dig into).
inline bool PfPartial(PfSearch& S){for(int k=0;k<7;++k){const PfP& p=S.nodes[S.bestNode[k]].p;const int a=p.x-S.start.x,b=p.y-S.start.y,c=p.z-S.start.z;
  if((unsigned)(a*a+b*b+c*c)>=26u)return PfBuildPath(S,S.bestNode[k]);}
 uint32_t best=0;for(uint32_t i=1;i<S.nodes.size();++i)if(S.nodes[i].h<S.nodes[best].h)best=i;
 return best!=0&&S.nodes[best].h<S.nodes[0].h-kPfSprint*S.greed&&PfBuildPath(S,best);}

// ============================================================================================ controller
enum PfProcess{kPfNone=0,kPfGoto,kPfWander,kPfFollow,kPfMine,kPfTunnel,kPfChop,kPfBuild};
inline constexpr float kPfOffPath=2.5f;   // off-path limit of every search request
struct PfSelf{Context c{};uintptr_t actor=0;uint64_t region=0;float eye[3]{},bb[6]{},pos[3]{},vel[3]{},yaw=0,pitch=0;PfP cell;bool onGround=false,inWater=false,climb=false;};
// Executor state: input (yaw of the move vector, move vector, sprint / jump / sneak, active, jump through the jump hook),
// stall counter, water pick-up, placed cells, and the block interaction (look target, destroy, place, door, tool wear).
struct PfExec{float yaw=0,mv[2]{};bool sprint=false,jump=false,sneak=false,active=false,holdJump=false;int stall=0;
 PfP water;bool waterPending=false;int waterTicks=0;std::vector<PfP> placed;
 float look[3]{};bool hasLook=false;float fixedYaw=0;bool fixedOn=false;PfP brkPos;bool breaking=false;uint8_t face=0xff;int brkTicks=0;double clock=0;
 PfP placePos;bool placing=false;int placeTries=0,placeCooldown=0;PfP doorPos;bool doorOn=false;int doorCooldown=0;float tolerance=12.f;int minDurability=0;bool toolWorn=false;};
struct PfOut{uint8_t status=0;float yaw=0,pitch=0;bool hasRot=false,instant=false,acted=false;};   // status: 0 moving, 1 arrived, 2 stuck, 3 off path, 4 failed
struct PfStuck{PfP p;int64_t at=0;int count=0;};
// Process tick result: 0 continue (with or without a goal), 1 finished, 2 failed; the goal id changes when the process makes a new
// goal; break / place restrict the search options; retarget keeps following the current path while the new search runs.
struct PfProcOut{int result=0;bool hasGoal=false;uint64_t goalId=0;PfGoal goal;bool brk=true,place=true,retarget=false;std::string status,message;};
struct PfGotoState{PfGoal goal;std::string label;int fails=0;uint64_t goalId=0;};
struct PfWanderState{int radius=96;bool hasGoal=false;PfP target;uint64_t goalId=0;std::mt19937 rng{std::random_device{}()};};
struct PfFollowState{std::string name;float distance=3;int radius=3;bool hasLast=false;PfP last;bool hasGoal=false;PfP goalAt;uint64_t goalId=0;int64_t goalNs=0;bool goalTimer=true;
 bool lastSeenGoal=false;uint64_t lastSeenId=0;bool lost=false,lostTimer=false;int64_t lostNs=0;bool close=false,chaseLast=true;};
struct PfMineTry{int count=0;int64_t at=0;};
struct PfMineState{std::vector<std::string> patterns;std::string label;int count=0,radius=128,mined=0,rescan=0;int64_t tick=0;std::vector<PfP> targets;bool anyGoal=false;uint64_t anyId=0;
 std::unordered_map<uint64_t,int64_t> skip;std::unordered_map<uint64_t,PfMineTry> tries;bool fixed=false;PfP target;bool collecting=false;PfP pickup;int pickupTicks=0;uint64_t pickupId=0;
 bool searching=false;PfP searchAt;uint64_t searchId=0;std::mt19937 rng{std::random_device{}()};std::unordered_map<uint64_t,uint8_t> match;};
struct PfTunnelState{int dx=0,dz=1,length=0,progress=0,goalIdx=0;bool started=false,gap=false,hasGoal=false;PfP origin;PfP goalAt;uint64_t goalId=0;};
// Build: a schematic cell to fill (block) or to clear (block 0); half 0 none / 1 bottom / 2 top; occupied = the world cell holds
// a block that is neither air nor liquid.
struct PfBuildTarget{PfP p;uint64_t block=0;uint8_t half=0;bool occupied=false;};
struct PfBuildState{std::vector<PfBuildTarget> targets;PfBuildTarget cur;bool placing=false;int64_t tick=0;int refresh=0,placed=0;
 std::unordered_map<uint64_t,int64_t> failed;std::map<std::string,int> missing;std::unordered_set<uint64_t> orientFailed;   // failed: cell -> tick it is retried from
 bool goalOn=false;PfP goalFor;PfGoal goal;uint64_t goalId=0;   // last stand-cell goal and the cell it was made for
 std::vector<PfP> scaffold;std::unordered_set<uint64_t> scaffoldSet;bool scBreaking=false;PfP scAt;
 bool moveTimer=true;int64_t moveNs=0;uint64_t moveBlock=0;int moveCount=0;};   // inventory -> hotbar moves
struct PfController{
 int proc=kPfNone,state=0;PfGoal goal;bool hasGoal=false;uint64_t goalId=0,nextId=1;std::string status;std::unique_ptr<PfSearch> search;bool replanSearch=false;PfWorld world;PfOpt lastOpt;bool procBreak=true,procPlace=true;
 std::vector<PfStep> path;bool hasPath=false,pathComplete=false,arrived=false,noPath=false;PfExec exec;int64_t tick=0,planAt=0;
 int dimension=-1;   // dimension of the last tick (-1 unknown): a change ends the running process
 std::vector<PfStuck> stuck;std::vector<PfView::Region> mobRegions;std::vector<PfP> spawners;std::unordered_map<uint64_t,uint8_t> spawnerBlocks;
 PfGotoState gotoS;PfWanderState wander;PfFollowState follow;PfMineState mine;PfTunnelState tunnel;PfBuildState build;bool userPaused=false;
 bool eating=false,eatTimer=false;int64_t eatNs=0;bool eatCool=true;int64_t eatCoolNs=0;
 bool retry=false;int64_t retryNs=0;bool contPending=false,contSearch=false;int64_t contNs=0;   // retry 1000 ms after nothing found; next segment 1500 ms after a partial one
 int64_t junkNs=0;bool junkTimer=false;   // retry of a search that found nothing, 1000 ms later
 bool deathKnown=false,wasDead=false;PfP death;
 // Progress watchdog: ticks spent steering without reaching a new cell or getting closer to the goal, consecutive
 // watchdog replans, recently visited cells (key -> tick), best goal estimate so far. giveUp / unreachable tell the
 // process that the goal cannot be reached (with failReason); goalNote is told once when a goal was adjusted.
 int progTicks=0,progFires=0;double progBest=1e300;std::unordered_map<uint64_t,int64_t> visited;bool giveUp=false,unreachable=false;std::string failReason;};
inline PfController& PfCtl(){static auto* c=new PfController;return *c;}
inline int64_t PfNow(){return ActionNs();}

// ---- player state
inline bool PfHasComp(const Context& c,uint32_t hash){uintptr_t st{};uint32_t pk{};return Storage(c.registry,hash,&st)&&Packed(st,c.entity,&pk);}
inline bool PfReadSelf(PfSelf& s){if(!Local(&s.c))return false;s.actor=s.c.actor;uint64_t st{},bb{};
 if(!Read(s.actor+0x240,&st)||!st||!Floats(st,s.eye,3))return false;if(!Floats(st+0x18,s.vel,3))s.vel[0]=s.vel[1]=s.vel[2]=0;
 if(!Read(s.actor+0x248,&bb)||!bb||!Floats(bb,s.bb,6))return false;s.pos[0]=(s.bb[0]+s.bb[3])*.5f;s.pos[1]=s.bb[1];s.pos[2]=(s.bb[2]+s.bb[5])*.5f;
 if(!RotRead(s.actor,&s.pitch,&s.yaw))return false;s.onGround=ActionOnGround(s.actor);s.inWater=PfHasComp(s.c,0x78e89f39u);s.region=ActorRegion(s.c);
 s.cell=PfP{(int)std::floor(s.pos[0]),(int)std::floor(s.pos[1]),(int)std::floor(s.pos[2])};s.climb=false;
 if(s.region){const uint64_t b=PfBlockRaw(s.c,s.region,(int)std::floor(s.pos[0]),(int)std::floor(s.pos[1]+0.01f),(int)std::floor(s.pos[2]));std::string n;if(b&&XrayBlockName((uintptr_t)b,&n))s.climb=PfBlockProps(n).climb;}
 return true;}
// Start cell: the cell under the feet, one higher when that cell is standable but not passable.
inline PfP PfStartCell(PfController& k,const PfSelf& s){if(!s.region)return s.cell;const PfRec& r=PfRecOf(k.world,PfBlockRaw(s.c,s.region,s.cell.x,s.cell.y,s.cell.z),s.cell);return r.solid&&!r.passable?PfAdd(s.cell,0,1,0):s.cell;}

// ---- angles and frames
inline float PfYawFromTo(float ax,float az,float bx,float bz){return std::atan2(-(bx-ax),bz-az)*57.2957763671875f;}
inline void PfToLocal(float vx,float vz,float yaw,float* lx,float* lz){const float r=yaw*0.01745329238474369f,sn=sinf(r),cs=cosf(r);*lx=cs*vx+sn*vz;*lz=cs*vz-sn*vx;}
inline void PfToWorld(float lx,float lz,float yaw,float* wx,float* wz){const float r=yaw*0.01745329238474369f,sn=sinf(r),cs=cosf(r);*wx=cs*lx-sn*lz;*wz=cs*lz+sn*lx;}
// Yaw step limited by Turn Speed: wrapped difference clamped to +-turn.
inline float PfTurn(float cur,float want,float turn){float d=fmodf(want-cur+180.f,360.f);if(d<0.f)d+=360.f;d-=180.f;d=std::max(-turn,std::min(turn,d));float r=fmodf(cur+d+180.f,360.f);if(r<0.f)r+=360.f;return r-180.f;}
inline bool PfLookAngles(const PfExec& e,const PfSelf& s,float* yaw,float* pitch){if(!e.hasLook)return false;const float dx=e.look[0]-s.eye[0],dy=e.look[1]-s.eye[1],dz=e.look[2]-s.eye[2];
 *pitch=std::atan2(dy,std::sqrt(dx*dx+dz*dz))*-57.29578f;*yaw=e.fixedOn?e.fixedYaw:std::atan2(-dx,dz)*57.29578f;return true;}
inline bool PfWithin(const PfSelf& s,float yaw,float pitch,float tol){return std::fabs(fmodf(yaw-s.yaw,360.f))<=tol&&std::fabs(pitch-s.pitch)<=tol;}

// ---- live block checks (game blocks, not the cache)
inline uint64_t PfLiveBlock(const PfSelf& s,const PfP& p){return s.region?PfBlockRaw(s.c,s.region,p.x,p.y,p.z):0;}
inline bool PfCellClear(const PfSelf& s,const PfP& p){uint8_t a{},b{};if(!BlockFlags(PfLiveBlock(s,p),&a,&b))return true;return a==0||(b&2)||!(b&4);}
inline bool PfReplaceableAt(const PfSelf& s,const PfP& p){return BlockReplaceable(PfLiveBlock(s,p));}
inline bool PfLiquidAt(const PfSelf& s,const PfP& p){uint8_t a{},b{};return BlockFlags(PfLiveBlock(s,p),&a,&b)&&(b&2);}
// Door open: open_bit of the cell, the one below (either open -> true), then the one above (its value).
inline bool PfDoorOpen(const PfSelf& s,const PfP& p){bool o=false;if(PfOpenBit(s.c,PfLiveBlock(s,p),&o)&&o)return true;if(PfOpenBit(s.c,PfLiveBlock(s,PfAdd(p,0,-1,0)),&o)&&o)return true;
 if(PfOpenBit(s.c,PfLiveBlock(s,PfAdd(p,0,1,0)),&o))return o;return false;}
inline uint64_t PfGm(const PfSelf& s){uint64_t gm{};Read(s.actor+0xac8,&gm);return gm;}
inline bool PfHotbarFind(const PfSelf& s,const std::function<bool(uint64_t)>& match,int* slot){int32_t* sel=ActionSelected(s.actor);const uint64_t box=ActionContainer(s.actor);if(!sel||!box)return false;
 if(match(ActionStackAt(s.c,box,*sel))){*slot=*sel;return true;}const int n=std::min(ActionSize(s.c,box),9);for(int i=0;i<n;++i)if(match(ActionStackAt(s.c,box,i))){*slot=i;return true;}return false;}
inline bool PfHotbarName(const PfSelf& s,const char* name,int* slot){return PfHotbarFind(s,[&](uint64_t st){return ActionNameIs(st,name);},slot);}
inline bool PfThrowawayMatch(uint64_t stack,const std::vector<std::string>& list){if(!BlockStack(stack))return false;std::string n;if(!ActionName(stack,&n))return false;n=PfFullId(n);for(auto& t:list)if(PfFullId(t)==n)return true;return false;}
inline int PfThrowawayCount(const PfSelf& s,const std::vector<std::string>& list){const uint64_t box=ActionContainer(s.actor);const int n=std::min(ActionSize(s.c,box),9);int total=0;
 for(int i=0;i<n;++i){const uint64_t st=ActionStackAt(s.c,box,i);uint8_t cnt{};if(PfThrowawayMatch(st,list)&&Read(st+0x22,&cnt))total+=cnt;}return total;}
// Use the held item on a block face with a given hit point (transaction type 0).
inline void PfUseOn(const PfSelf& s,const PfP& p,uint8_t face,const float* rel){int32_t* sel=ActionSelected(s.actor);float me[3]{};if(!sel||!ActionPosition(s.actor,me))return;const int32_t at[3]={p.x,p.y,p.z};
 const uint64_t stack=ActionHeld(s.c,s.actor);int32_t runtime{};if(const uint64_t block=PfLiveBlock(s,p))Read(block+0x114,&runtime);
 uint8_t* t=NewUseTransaction(s.c,stack);*(int32_t*)(t+0x68)=0;memcpy(t+0x70,at,12);*(int32_t*)(t+0x7c)=runtime;*(int32_t*)(t+0x84)=*sel;*(uint8_t*)(t+0x88)=0;*(uint8_t*)(t+0x80)=face;
 memcpy(t+0xf0,me,12);memcpy(t+0xfc,rel,12);SendUseTransaction(s.c,s.actor,t);}

// ---- executor input helpers
inline void PfInputHold(PfExec& e,const PfSelf& s){e.yaw=s.yaw;e.mv[0]=e.mv[1]=0;e.sprint=e.jump=e.sneak=false;e.active=true;e.holdJump=false;}
inline void PfLookOut(const PfExec& e,const PfSelf& s,PfOut& out){float y,p;out.hasRot=PfLookAngles(e,s,&y,&p);if(out.hasRot){out.yaw=y;out.pitch=p;}}
inline void PfStopDestroy(PfExec& e,const PfSelf* s){if(e.breaking&&s){const uint64_t gm=PfGm(*s);const uint64_t f=gm?Virtual0(s->c,gm,0x20):0;if(f){int32_t at[3]={e.brkPos.x,e.brkPos.y,e.brkPos.z};reinterpret_cast<void(*)(uint64_t,int32_t*)>(f)(gm,at);}}e.breaking=false;}
inline void PfResetInteraction(PfExec& e,const PfSelf* s){PfStopDestroy(e,s);e.placing=false;e.doorOn=false;e.hasLook=false;e.placeTries=0;e.placeCooldown=0;e.doorCooldown=0;}
inline void PfExecClear(PfExec& e,const PfSelf* s){PfResetInteraction(e,s);e.waterPending=false;e.waterTicks=0;e.yaw=0;e.mv[0]=e.mv[1]=0;e.sprint=e.jump=e.sneak=false;e.active=false;e.holdJump=false;}
// Stand towards point P while acting: move at min(2d, 1) towards P in the look yaw when farther than 0.2.
inline void PfHold(PfExec& e,const PfSelf& s,const float* P,PfOut& out){
 const float dz=P[2]-(s.bb[5]+s.bb[2])*.5f,dx=P[0]-(s.bb[0]+s.bb[3])*.5f,d=std::sqrt(dx*dx+dz*dz);
 PfInputHold(e,s);e.stall=0;out=PfOut();PfLookOut(e,s,out);
 if(d>.2f&&out.hasRot){e.yaw=out.yaw;const float m=std::min(d+d,1.f);float lx,lz;PfToLocal(dx/d,dz/d,out.yaw,&lx,&lz);e.mv[0]=m*lx;e.mv[1]=m*lz;}}
inline void PfIdle(PfExec& e,const PfSelf& s,PfOut& out){PfInputHold(e,s);e.stall=0;out=PfOut();PfLookOut(e,s,out);}

// ---- block interaction: face choice, breaking, doors, placing
// Face: highest dot(view direction to the face centre, normal) + 2 unless the neighbour is a solid non-liquid block.
inline uint8_t PfFace(const PfSelf& s,const PfP& p){static const int n[6][3]={{0,-1,0},{0,1,0},{0,0,-1},{0,0,1},{-1,0,0},{1,0,0}};uint8_t best=1;float score=-3.4028234663852886e+38f;
 for(int f=0;f<6;++f){const float fc[3]={p.x+.5f+n[f][0]*.5f,p.y+.5f+n[f][1]*.5f,p.z+.5f+n[f][2]*.5f};const float d[3]={s.eye[0]-fc[0],s.eye[1]-fc[1],s.eye[2]-fc[2]};
  const float inv=1.f/std::sqrt(d[0]*d[0]+d[1]*d[1]+d[2]*d[2]);uint8_t a{},b{};float bonus=2.f;if(BlockFlags(PfLiveBlock(s,PfAdd(p,n[f][0],n[f][1],n[f][2])),&a,&b)&&a!=0&&(b&6)==4)bonus=0.f;
  const float v=inv*d[0]*n[f][0]+d[1]*inv*n[f][1]+d[2]*inv*n[f][2]+bonus;if(v>score){score=v;best=(uint8_t)f;}}
 return best;}
inline void PfAimFace(PfExec& e,const PfP& p,uint8_t face){static const float o[6][3]={{0,-.5f,0},{0,.5f,0},{0,0,-.5f},{0,0,.5f},{-.5f,0,0},{.5f,0,0}};
 e.look[0]=p.x+.5f+(face<6?o[face][0]:0);e.look[1]=p.y+.5f+(face<6?o[face][1]:0);e.look[2]=p.z+.5f+(face<6?o[face][2]:0);e.hasLook=true;e.fixedOn=false;}
inline bool PfInReach(const PfSelf& s,const PfP& p,float reach){const float dx=p.x+.5f-s.eye[0],dy=p.y+.5f-s.eye[1],dz=p.z+.5f-s.eye[2];return dx*dx+dy*dy+dz*dz<=(reach+.5f)*(reach+.5f);}
// Tools: hotbar slot with the highest destroy speed (strictly better than the current one); with a durability floor, tools
// whose remaining durability is not above it are skipped (an empty current slot counts as speed 1).
inline float PfToolSpeed(const Context& c,uint64_t stack,uint64_t block){return ActionItem(stack)?BbSpeed(c,stack,block):1.f;}
inline bool PfToolOk(const Context& c,uint64_t stack,int minDur){const uint64_t item=ActionItem(stack);if(!item||minDur<1)return true;const uint64_t top=Virtual0(c,item,0x120);if(!top)return true;
 const int maximum=reinterpret_cast<short(*)(uint64_t)>(top)(item);if(maximum<1)return true;return ArmorDurability(c,stack)>minDur;}
inline int PfBestTool(const PfSelf& s,uint64_t block,int current,int minDur){const uint64_t box=ActionContainer(s.actor);const int n=std::min(ActionSize(s.c,box),9);if(current<0||current>=n)return -1;
 int best=-1;float score=-1.f;const uint64_t cur=ActionStackAt(s.c,box,current);if(PfToolOk(s.c,cur,minDur)){best=current;score=PfToolSpeed(s.c,cur,block);}
 for(int i=0;i<n;++i){const uint64_t st=ActionStackAt(s.c,box,i);if(!PfToolOk(s.c,st,minDur))continue;const float v=PfToolSpeed(s.c,st,block);if(v>score){score=v;best=i;}}return best;}
// Break one cell: 1 done (air or liquid), 0 working / waiting for the aim, 2 failed (out of reach, no game mode, 600 ticks, worn tool).
inline int PfBreak(PfExec& e,const PfSelf& s,const PfP& p,float reach){
 uint8_t a{},b{};const uint64_t block=PfLiveBlock(s,p);if(!BlockFlags(block,&a,&b)||a==0||(b&2)){if(e.breaking&&e.brkPos==p)e.breaking=false;e.hasLook=false;return 1;}
 if(!PfInReach(s,p,reach))return 2;
 const uint8_t face=(e.breaking&&e.brkPos==p&&e.face<6)?e.face:PfFace(s,p);PfAimFace(e,p,face);
 float yaw,pitch;PfLookAngles(e,s,&yaw,&pitch);if(!PfWithin(s,yaw,pitch,e.tolerance))return 0;
 const uint64_t gm=PfGm(s);const uint64_t box=ActionContainer(s.actor);int32_t* sel=ActionSelected(s.actor);if(!gm||!box||!sel)return 2;
 uint8_t* destroying=(uint8_t*)(s.actor+0xb4a);const uint8_t was=*destroying;*destroying=1;int32_t at[3]={p.x,p.y,p.z};bool done=false;int result=0;
 if(e.breaking&&e.brkPos==p){
  if(BbGmClock(gm)<=e.clock){const uint64_t f=Virtual0(s.c,gm,0x48);if(f)reinterpret_cast<void(*)(uint64_t)>(f)(gm);}e.clock=BbGmClock(gm);
  const uint64_t f=Virtual0(s.c,gm,0x18);float eye[3]={s.eye[0],s.eye[1],s.eye[2]};if(f)reinterpret_cast<bool(*)(uint64_t,int32_t*,uint8_t,float*,bool*)>(f)(gm,at,face,eye,&done);ActionSwing(s.c,s.actor,2);
  if(done)e.breaking=false;else if(e.brkTicks++>599){PfStopDestroy(e,&s);result=2;}}
 else{PfStopDestroy(e,&s);const int any=PfBestTool(s,block,*sel,0),safe=PfBestTool(s,block,*sel,e.minDurability);bool go=false;
  if(any<0){if(safe>=0)*sel=safe;go=true;}
  else if(safe>=0&&PfToolSpeed(s.c,ActionStackAt(s.c,box,any),block)<=PfToolSpeed(s.c,ActionStackAt(s.c,box,safe),block)){*sel=safe;go=true;}
  if(!go){e.toolWorn=true;result=2;}
  else{const uint64_t f=Virtual0(s.c,gm,0x08);if(f)reinterpret_cast<bool(*)(uint64_t,int32_t*,uint8_t,bool*)>(f)(gm,at,face,&done);ActionSwing(s.c,s.actor,2);
   e.brkPos=p;e.breaking=true;e.face=face;e.brkTicks=0;e.clock=BbGmClock(gm);if(done)e.breaking=false;}}
 *destroying=was;return result;}
// Door: 1 used, 0 cooling down / waiting for the aim, 2 out of reach.
inline int PfDoor(PfExec& e,const PfSelf& s,const PfP& p,float reach){
 if(e.doorOn&&e.doorPos==p&&e.doorCooldown>0){--e.doorCooldown;return 0;}
 if(!PfInReach(s,p,reach))return 2;const uint8_t face=PfFace(s,p);PfAimFace(e,p,face);float yaw,pitch;PfLookAngles(e,s,&yaw,&pitch);if(!PfWithin(s,yaw,pitch,e.tolerance))return 0;
 static const float n[6][3]={{0,-1,0},{0,1,0},{0,0,-1},{0,0,1},{-1,0,0},{1,0,0}};float rel[3];for(int i=0;i<3;++i)rel[i]=n[face][i]*.5f+.5f;
 PfUseOn(s,p,face,rel);ActionSwing(s.c,s.actor,3);e.doorPos=p;e.doorOn=true;e.doorCooldown=10;return 1;}
// Place a throwaway block: 1 the cell is no longer replaceable, 0 working, 2 no block / too many tries.
inline int PfPlace(PfExec& e,const PfSelf& s,const PfP& p,float reach,const std::vector<std::string>& throwaway){
 if(!e.placing||e.placePos!=p){e.placePos=p;e.placing=true;e.placeTries=0;e.placeCooldown=0;}
 if(!PfReplaceableAt(s,p)){e.placing=false;e.hasLook=false;return 1;}
 if(e.placeCooldown>=1){--e.placeCooldown;return 0;}
 int slot=-1;if(!PfHotbarFind(s,[&](uint64_t st){return PfThrowawayMatch(st,throwaway);},&slot))return 2;
 int32_t* sel=ActionSelected(s.actor);if(!sel)return 2;if(*sel!=slot){*sel=slot;return 0;}
 const int32_t at[3]={p.x,p.y,p.z};AbrPlace pl{};int limit=7;
 if(AbrFind(s.c,s.actor,at,reach,&pl)){e.look[0]=pl.hit[0];e.look[1]=pl.hit[1];e.look[2]=pl.hit[2];e.hasLook=true;e.fixedOn=false;float yaw,pitch;PfLookAngles(e,s,&yaw,&pitch);
  if(!PfWithin(s,yaw,pitch,e.tolerance))return 0;AbrPlaceBlock(s.c,s.actor,pl,reach);ActionSwing(s.c,s.actor,1);e.placeCooldown=3;limit=11;}
 const int tries=e.placeTries++;return limit<tries?2:0;}

// ---- water bucket: clutch while falling, pick-up after landing
struct PfShape{float v[6];int p[3];uint8_t flag;};
inline bool PfFetch(const Context& c,uint64_t region,const float* box,std::vector<PfShape>& out){out.clear();const uint64_t fetch=region?Virtual0(c,region,0xb0):0;if(!fetch)return false;
 static uint64_t buf[3]{};constexpr uint64_t elements=0x2000,stride=0x30;if(!buf[0]){const uint64_t b=MoveBAllocate(elements*stride);if(!b)return false;buf[0]=buf[1]=b;buf[2]=b+elements*stride;}
 buf[1]=buf[0];if(!PfFetchRaw(fetch,region,buf,box)){buf[1]=buf[0];return false;}
 for(uint64_t e=buf[0];e<buf[1];e+=stride){PfShape h{};if(!Read(e+0x2c,&h.flag)||!Floats(e+8,h.v,6)||!Read(e+0x20,&h.p[0])||!Read(e+0x24,&h.p[1])||!Read(e+0x28,&h.p[2]))return false;out.push_back(h);}return true;}
// Clutch target: highest unflagged shapes under the feet (64 blocks), a full block with a replaceable cell above, nearest to the
// player centre, no liquid in between, drop 0..12. Returns the aim; uses the water bucket once the drop is within
// max(2 x fall speed + 0.5, 3.5) and the aim within 12 deg.
inline bool PfClutch(PfExec& e,const PfSelf& s,float* yaw,float* pitch){
 if(s.onGround)return false;
 if(e.waterPending){if(PfLiquidAt(s,e.water)||e.waterTicks<11)return false;e.waterPending=false;}
 const float box[6]={s.bb[0]-0.f,s.bb[1]-64.f,s.bb[2]-0.f,s.bb[3]-0.f,s.bb[1],s.bb[5]};std::vector<PfShape> all;if(!PfFetch(s.c,s.region,box,all))return false;
 std::vector<PfShape> keep;for(auto& h:all)if(!h.flag&&h.v[4]<=s.bb[1]+0.001f)keep.push_back(h);if(keep.empty())return false;
 float topMax=keep[0].v[4];for(auto& h:keep)if(h.v[4]>topMax)topMax=h.v[4];
 const float cx=(s.bb[0]+s.bb[3])*.5f,cz=(s.bb[2]+s.bb[5])*.5f,level=topMax-0.001f;float bestD=3.4028234663852886e+38f;bool found=false;PfP tgt;
 for(auto& h:keep){if(!(level<=h.v[4]))continue;const float ix=(float)h.p[0],iy=(float)h.p[1],iz=(float)h.p[2];
  if(0.001f<h.v[0]-ix||0.001f<h.v[1]-iy||!(h.v[2]-iz<=0.001f))continue;const float hy=h.v[4]-iy;if(!(0.999f<=h.v[3]-ix&&0.999f<=hy&&0.999f<=h.v[5]-iz&&hy<=1.001f))continue;
  if(!PfReplaceableAt(s,PfP{h.p[0],h.p[1]+1,h.p[2]}))continue;const float dx=ix+.5f-cx,dz=iz+.5f-cz,d=std::sqrt(dx*dx+dz*dz);if(d<bestD){bestD=d;tgt=PfP{h.p[0],h.p[1],h.p[2]};found=true;}}
 if(!found)return false;
 const int fx=(int)std::floor(cx),fz=(int)std::floor(cz);for(int y=(int)std::floor(s.bb[1]);tgt.y<y;--y)if(PfLiquidAt(s,PfP{fx,y,fz}))return false;
 const float drop=s.bb[1]-(float)(tgt.y+1);if(!(0.f<drop&&drop<=12.f))return false;
 int slot{};if(!PfHotbarName(s,"water_bucket",&slot))return false;if(int32_t* sel=ActionSelected(s.actor))*sel=slot;
 const float dx=tgt.x+.5f-s.eye[0],dy=(float)(tgt.y+1)-s.eye[1],dz=tgt.z+.5f-s.eye[2];*pitch=std::atan2(dy,std::sqrt(dx*dx+dz*dz))*-57.29578f;*yaw=std::atan2(-dx,dz)*57.29578f;
 float fall=-s.vel[1];if(fall<=0.f)fall=0.f;float lead=fall+fall+.5f;if(lead<=3.5f)lead=3.5f;
 if(drop<=lead&&PfWithin(s,*yaw,*pitch,12.f)){const float rel[3]={.5f,1.f,.5f};PfUseOn(s,tgt,1,rel);ActionSwing(s.c,s.actor,1);e.water=PfP{tgt.x,tgt.y+1,tgt.z};e.waterPending=true;e.waterTicks=0;}
 return true;}
// Pick-up: within 30 ticks of the clutch, on the ground or in water, with the water still there and an empty bucket in the hotbar.
inline bool PfPickup(PfExec& e,const PfSelf& s,PfOut& out){
 const int old=e.waterTicks++;if(old>=30){e.waterPending=false;return false;}if(!(s.onGround||s.inWater))return false;
 if(!PfLiquidAt(s,e.water)){e.waterPending=false;return false;}int slot{};if(!PfHotbarName(s,"bucket",&slot)){e.waterPending=false;return false;}
 if(int32_t* sel=ActionSelected(s.actor))*sel=slot;
 const float dx=e.water.x+.5f-s.eye[0],dy=e.water.y+.1f-s.eye[1],dz=e.water.z+.5f-s.eye[2];const float pitch=std::atan2(dy,std::sqrt(dx*dx+dz*dz))*-57.29578f,yaw=std::atan2(-dx,dz)*57.29578f;
 if(PfWithin(s,yaw,pitch,12.f)&&(e.waterTicks&3)==0){const float rel[3]={.5f,.1f,.5f};PfUseOn(s,e.water,1,rel);ActionSwing(s.c,s.actor,1);}
 PfInputHold(e,s);out=PfOut();out.yaw=yaw;out.pitch=pitch;out.hasRot=true;out.instant=true;return true;}

// ---- steering
inline void PfCentre(const PfP& q,float* o){o[0]=q.x+.5f;o[1]=(float)q.y;o[2]=q.z+.5f;}
// Step reached (the next four are checked): pillar on the ground at the target height, climbs / swims within 0.6 and above
// (below) target - 0.1 (+ 0.1), falls within 0.6 and level (in the air only on a climbable), walks within 0.35 and level
// (in the air only for walks and diagonals).
inline bool PfReachedStep(const PfStep& st,const PfSelf& s){
 float to[3];PfCentre(st.to,to);const float dist=std::sqrt((to[2]-s.pos[2])*(to[2]-s.pos[2])+(to[0]-s.pos[0])*(to[0]-s.pos[0]));const float ty=(float)st.to.y;const uint8_t t=st.type;
 if((uint8_t)(t-4)<9){switch(t){case 7:return s.onGround&&ty-0.1f<=s.pos[1];case 8:case 10:return dist<0.6f&&ty-0.1f<=s.pos[1];case 9:case 11:case 12:return dist<0.6f&&s.pos[1]<=ty+0.1f;default:break;}
  if(0.6f<=dist)return false;const float dy=std::fabs(s.pos[1]-ty);if(!s.onGround&&dy<0.6f&&!s.inWater)return s.climb;return dy<0.6f;}
 const bool ok=s.onGround||s.inWater||s.climb;if(0.35f<=dist)return false;const float dy=std::fabs(s.pos[1]-ty);if(0.6f<=dy)return false;return ok?true:t<2;}
inline size_t PfReachedCount(const std::vector<PfStep>& path,const PfSelf& s){size_t n=0;const size_t end=std::min<size_t>(path.size(),4);for(size_t i=0;i<end;++i)if(PfReachedStep(path[i],s))n=i+1;return n;}
// Parkour take-off passed: progress along the step direction from the start centre >= 0.55.
inline bool PfPastTakeoff(const PfStep& st,const PfSelf& s){const float fx=(float)(st.to.x-st.from.x),fz=(float)(st.to.z-st.from.z),len=std::sqrt(fx*fx+fz*fz);if(len<0.001f)return false;
 return 0.55f<=((s.pos[2]-(st.from.z+.5f))*fz+(s.pos[0]-(st.from.x+.5f))*fx)/len;}
// ---- movement simulation for jump decisions (game_movement_sim.hpp)
// Input record from the executor input: move vector, pitch, steering yaw, simulated position / velocity, sprint and sneak
// start / stop / held bytes against the simulated flags, jump pressed (only on the ground, not riding) and held.
inline void PfSimInput(MsInput& in,const PfExec& e,const MsState& st,bool jump){uint8_t* p=in.m;const uint8_t* s=st.m;memset(p,0,sizeof(in.m));
 memcpy(p,e.mv,8);memcpy(p+0xc,s+0x94,4);memcpy(p+0x10,&e.yaw,4);memcpy(p+0x14,&e.yaw,4);memcpy(p+0x18,s+0x4c,12);memcpy(p+0x24,s+0x64,12);
 const uint8_t sp=s[0x1a2],sn=s[0x1a6];p[0x34]=e.sprint?(uint8_t)!sp:0;p[0x35]=e.sprint?0:sp;p[0x36]=e.sprint;p[0x37]=e.sneak?(uint8_t)!sn:0;p[0x38]=e.sneak?0:sn;p[0x39]=e.sneak;p[0x3a]=e.sneak;
 uint64_t riding{};memcpy(&riding,s+0x1b0,8);p[0x3b]=(uint8_t)(riding==0&&s[0x1d6]&&jump);p[0x3c]=jump;}
inline MsShapes& PfSimShapes(){static MsShapes shapes;return shapes;}
// Closest approach over `ticks` simulated ticks: the first tick uses the given input, the next ones walk forward towards
// the target without jumping; value = 2 |dy| + horizontal distance (smallest); a failed tick or a fall below `floorY` gives
// the largest float.
inline float PfSimApproach(const MsSim& base,PfExec in,const float* target,float floorY,int ticks){
 MsSim sim=base;float best=3.4028234663852886e+38f;
 for(int i=0;i<ticks;++i){MsInput rec;PfSimInput(rec,in,sim.state,in.jump);MsOutput out;MsTick(sim.env,out,sim.state,rec);if(MsOutFailed(out))return 3.4028234663852886e+38f;
  float p[3];memcpy(p,sim.state.m+0x4c,12);if(p[1]<floorY)return 3.4028234663852886e+38f;
  in.jump=false;in.yaw=PfYawFromTo(p[0],p[2],target[0],target[2]);in.mv[0]=0.f;in.mv[1]=1.f;
  const float dy=std::fabs(target[1]-p[1]);const float m=dy+dy+std::sqrt((target[2]-p[2])*(target[2]-p[2])+(target[0]-p[0])*(target[0]-p[0]));if(m<best)best=m;}
 return best;}
// Parkour take-off: one simulated tick with the current input (jump released); jump when the player would leave the ground.
inline bool PfSimLeaves(const MsSim& base,PfExec in){MsSim sim=base;in.jump=false;MsInput rec;PfSimInput(rec,in,sim.state,false);MsOutput out;MsTick(sim.env,out,sim.state,rec);
 return !MsOutGround(out)&&!MsOutFailed(out);}
inline void PfSteer(PfController& k,const PfSelf& s,const PfConfig& cfg,PfOut& out){
 PfExec& e=k.exec;const std::vector<PfStep>& path=k.path;const PfStep& st=path[0];const float px=s.pos[0],py=s.pos[1],pz=s.pos[2];
 float to0[3];PfCentre(st.to,to0);const float d0=std::sqrt((to0[2]-pz)*(to0[2]-pz)+(to0[0]-px)*(to0[0]-px));float tgt[3]={to0[0],to0[1],to0[2]};size_t n=1;
 if(d0<0.55f&&n<path.size()&&st.type<2&&path[1].type<2)PfCentre(path[1].to,tgt);
 const float dx=tgt[0]-px,dz=tgt[2]-pz,dl=std::sqrt(dx*dx+dz*dz);float from[3];PfCentre(st.from,from);
 float want=s.yaw;if((to0[2]-from[2])*(to0[2]-from[2])+(to0[0]-from[0])*(to0[0]-from[0])>9.999999974752427e-07f)want=PfYawFromTo(from[0],from[2],to0[0],to0[2]);
 if(0.4f<dl)want=PfYawFromTo(px,pz,tgt[0],tgt[2]);
 const float yaw=PfTurn(s.yaw,want,cfg.turnSpeed);e.yaw=yaw;e.mv[0]=e.mv[1]=0;e.sprint=e.jump=e.sneak=false;e.active=true;
 const bool closeTo=d0<0.3f,last=n>=path.size(),vert=(uint8_t)(st.type-7)<6;float fwd=0;
 if(0.001f<dl&&!(closeTo&&(last||vert))){float mag=1.f;if(last||vert){const float two=dl+dl;mag=1.f<two?1.f:std::max(0.25f,two);}
  float lx,lz;PfToLocal(dx/dl,dz/dl,yaw,&lx,&lz);e.mv[0]=mag*lx;e.mv[1]=mag*lz;fwd=e.mv[1];}
 const uint8_t t=st.type;bool sprint=false;
 if(t<7&&((0x4bu>>t)&1)&&cfg.allowSprint&&!s.inWater&&0.8999999761581421f<=fwd)sprint=1.5f<d0||!last;
 e.sprint=sprint;MsSim sim;const bool simOk=MsInit(s.c,s.region,&PfSimShapes(),sim);if(!simOk)sim.state.m[0x1d6]=(uint8_t)s.onGround;
 switch(t){
  case 2:e.sneak=true;sprint=false;break;
  case 4:case 5:case 9:case 12:sprint=false;break;
  case 6:sprint=cfg.allowSprint&&0.8999999761581421f<=fwd;e.sprint=sprint;
   if(sim.state.m[0x1d6]==1){if(PfPastTakeoff(st,s))e.jump=true;else e.jump=simOk&&PfSimLeaves(sim,e);}else e.jump=false;break;
  case 7:case 8:case 10:e.jump=true;break;
  case 11:e.sneak=true;break;
  default:
   if(s.inWater)e.jump=py-0.1f<=to0[1];
   else if(sim.state.m[0x1d6]==1&&py+0.3f<to0[1]){   // jump when 14 simulated ticks with a jump get 0.05 closer than without
    const float floorY=std::min(to0[1],(float)st.from.y)-(st.type==5?1.f:0.f);e.sprint=sprint;PfExec withJump=e;withJump.jump=true;
    const float a=simOk?PfSimApproach(sim,withJump,to0,floorY,14):3.4028234663852886e+38f,b=simOk?PfSimApproach(sim,e,to0,floorY,14):3.4028234663852886e+38f;
    e.jump=a+0.05f<b||(d0<1.1f&&0.6f<to0[1]-py);}
   break;}
 e.sprint=sprint;
 // jump on the ground (not on a climbable, not in water) goes through the jump hook; otherwise the jump key is held
 if(e.jump&&s.onGround&&!s.inWater){e.holdJump=!s.climb;if(!s.climb)JumpRequest(s.actor);}else e.holdJump=false;
 // pitch towards the first step at least 3 blocks away (or the last), 1.2 above it
 float look[3];size_t i=0;for(;i<path.size();++i){PfCentre(path[i].to,look);const float hx=look[0]-s.eye[0],hz=look[2]-s.eye[2];if(3.f<=std::sqrt(hz*hz+hx*hx))break;}
 if(i>=path.size())PfCentre(path.back().to,look);
 const float hx=look[0]+0.f-s.eye[0],hz=look[2]+0.f-s.eye[2];float pitch=std::atan2(look[1]+1.2f-s.eye[1],std::sqrt(hx*hx+hz*hz))*-57.29578f;pitch=std::max(-20.f,std::min(40.f,pitch));
 out=PfOut();out.yaw=e.yaw;out.pitch=pitch;out.hasRot=true;}
// Off path: distance from the feet centre to the nearest step segment (start centre -> target centre) above the limit.
inline bool PfOffPath(const PfController& k,const PfSelf& s){if(k.path.empty())return false;double best=1e300;const double P[3]={s.pos[0],s.pos[1],s.pos[2]};
 for(auto& st:k.path){const double a[3]={st.from.x+.5,(double)st.from.y,st.from.z+.5},d[3]={st.to.x+.5-a[0],(double)st.to.y-a[1],st.to.z+.5-a[2]};const double l2=d[0]*d[0]+d[1]*d[1]+d[2]*d[2];
  double t=0;if(l2>0)t=((P[2]-a[2])*d[2]+(P[0]-a[0])*d[0]+(P[1]-a[1])*d[1])/l2;t=t<0?0:(t>1?1:t);const double q[3]={P[0]-(d[0]*t+a[0]),P[1]-(d[1]*t+a[1]),P[2]-(d[2]*t+a[2])};
  best=std::min(best,std::sqrt(q[0]*q[0]+q[1]*q[1]+q[2]*q[2]));}
 return (double)kPfOffPath<best;}
// Step actions: break the listed cells, open the door, place the support (an ascend places it standing still; a pillar
// jumps first and places once one block up), standing towards the start centre while acting.
inline bool PfAct(PfController& k,const PfSelf& s,const PfConfig& cfg,float reach,PfOut& out){
 PfExec& e=k.exec;const PfStep& st=k.path[0];const float fromC[3]={st.from.x+.5f,(float)st.from.y,st.from.z+.5f};
 auto fail=[&]{e.yaw=0;e.mv[0]=e.mv[1]=0;e.sprint=e.jump=e.sneak=false;e.active=false;e.holdJump=false;out=PfOut();out.status=4;out.acted=true;return true;};
 for(int i=0;i<st.nbrk;++i){if(PfCellClear(s,st.brk[i]))continue;if(PfBreak(e,s,st.brk[i],reach)==2)return fail();e.stall=0;PfHold(e,s,fromC,out);out.acted=true;return true;}
 if(st.hasDoor&&!PfDoorOpen(s,st.door)){if(PfDoor(e,s,st.door,reach)==2)return fail();e.stall=0;PfHold(e,s,fromC,out);out.acted=true;return true;}
 if(st.hasPlace&&PfReplaceableAt(s,st.place)){e.stall=0;
  if(st.type==3){if(e.placed.empty()||e.placed.back()!=st.place)e.placed.push_back(st.place);if(PfPlace(e,s,st.place,reach,cfg.throwaway)==2)return fail();PfHold(e,s,fromC,out);out.acted=true;return true;}
  PfOut steer;PfSteer(k,s,cfg,steer);
  if(st.type!=7||(float)st.from.y+1.f<=s.bb[1]){if(e.placed.empty()||e.placed.back()!=st.place)e.placed.push_back(st.place);if(PfPlace(e,s,st.place,reach,cfg.throwaway)==2)return fail();
   out=steer;float ly,lp;if(PfLookAngles(e,s,&ly,&lp)){float wx,wz;PfToWorld(e.mv[0],e.mv[1],e.yaw,&wx,&wz);PfToLocal(wx,wz,ly,&e.mv[0],&e.mv[1]);e.yaw=ly;e.sprint=false;out.yaw=ly;out.pitch=lp;out.hasRot=true;}}
  else{out=steer;out.yaw=e.yaw;out.pitch=90.f;out.hasRot=true;}
  out.acted=true;return true;}
 return false;}
inline PfOut PfExecute(PfController& k,const PfSelf& s,const PfConfig& cfg,float reach){
 PfExec& e=k.exec;PfOut out;
 if(e.waterPending){PfOut w;if(PfPickup(e,s,w))return w;}
 const size_t n=PfReachedCount(k.path,s);if(n){k.path.erase(k.path.begin(),k.path.begin()+(std::ptrdiff_t)n);e.stall=0;PfResetInteraction(e,&s);}
 if(k.pathComplete&&k.path.empty()){PfExecClear(e,&s);out.status=1;return out;}
 if(k.path.empty()){PfExecClear(e,&s);return out;}
 if(e.breaking){bool listed=false;for(int i=0;i<k.path[0].nbrk;++i)if(k.path[0].brk[i]==e.brkPos)listed=true;if(!listed)PfResetInteraction(e,&s);}
 if(PfAct(k,s,cfg,reach,out))return out;
 PfSteer(k,s,cfg,out);
 if(k.path[0].type==5&&k.path[0].bucket){float y,p;if(PfClutch(e,s,&y,&p)){out.yaw=y;out.pitch=p;out.hasRot=true;out.instant=true;e.stall=0;}}
 const int old=e.stall++;if(old<100){if(PfOffPath(k,s))out.status=3;}else{e.stall=0;out.status=2;}
 return out;}

// ---- avoid regions: hostile mobs every tick, spawners every 40 ticks
inline bool PfPaletteRaw(uint64_t fn,uint64_t storage,uint64_t* out){__try{reinterpret_cast<void*(*)(uint64_t,uint64_t*)>(fn)(storage,out);return true;}__except(EXCEPTION_EXECUTE_HANDLER){return false;}}
inline int PfBitsRaw(uint64_t fn,uint64_t storage){__try{return reinterpret_cast<int(*)(uint64_t)>(fn)(storage);}__except(EXCEPTION_EXECUTE_HANDLER){return -1;}}
inline uint64_t PfChunkPtr(uint64_t fn,uint64_t region,int cx,int cz){int cp[2]={cx,cz};__try{return reinterpret_cast<uint64_t(*)(uint64_t,int*)>(fn)(region,cp);}__except(EXCEPTION_EXECUTE_HANDLER){return 0;}}
inline void PfSubSpawners(PfController& k,const Context& c,uint64_t storage,int bx,int by,int bz,std::vector<PfP>& found){
 const uint64_t palette=Virtual(c,storage,0xa8),bitsOf=Virtual(c,storage,0xa0),wordsOf=Virtual(c,storage,0x90);if(!palette||!bitsOf||!wordsOf)return;
 uint64_t p[2]{};if(!PfPaletteRaw(palette,storage,p)||!p[0]||!p[1])return;const uint32_t entries=(uint32_t)std::min<uint64_t>(p[0],4096);std::vector<uint8_t> hit(entries,0);bool any=false;
 for(uint32_t i=0;i<entries;++i){uint64_t block{};if(!Read(p[1]+8ull*i,&block)||!block)continue;auto f=k.spawnerBlocks.find(block);uint8_t m;
  if(f==k.spawnerBlocks.end()){std::string n;m=XrayBlockName((uintptr_t)block,&n)&&(n=="minecraft:mob_spawner"||n=="minecraft:trial_spawner")?1:0;k.spawnerBlocks.emplace(block,m);}else m=f->second;hit[i]=m;any=any||m;}
 if(!any)return;const int bits=PfBitsRaw(bitsOf,storage);if(bits<=0){if(hit[0])for(int at=0;at<4096;++at)found.push_back(PfP{bx+(at>>8),by+(at&15),bz+((at>>4)&15)});return;}if(bits>32)return;
 uint64_t words[2]{};if(!PfPaletteRaw(wordsOf,storage,words)||!words[0]||!words[1])return;const uint32_t per=32/bits,mask=bits==32?0xffffffffu:((1u<<bits)-1);
 for(uint32_t at=0;at<4096;++at){const uint32_t word=at/per;uint32_t value{};if(word>=words[0]||!Read(words[1]+4ull*word,&value))continue;const uint32_t pick=(value>>((at%per)*bits))&mask;
  if(pick<entries&&hit[pick])found.push_back(PfP{bx+(int)(at>>8),by+(int)(at&15),bz+(int)((at>>4)&15)});}}
inline void PfSpawnerScan(PfController& k,const PfSelf& s,const PfConfig& cfg,const PfP& cell){
 k.spawners.clear();if(!s.region)return;const uint64_t chunkAt=Virtual(s.c,s.region,0x148);int16_t lowest{};if(!chunkAt||!Read(s.region+0x3a,&lowest))return;
 const int R=std::max(2,cfg.cacheRadius),pcx=cell.x>>4,pcz=cell.z>>4,lim=R*16;std::vector<PfP> found;
 for(int dx=-R;dx<=R;++dx)for(int dz=-R;dz<=R;++dz){const uint64_t chunk=PfChunkPtr(chunkAt,s.region,pcx+dx,pcz+dz);if(!chunk)continue;uint64_t begin{},end{};
  if(!Read(chunk+0x1c8,&begin)||!Read(chunk+0x1d0,&end)||end<begin)continue;const uint32_t count=(uint32_t)std::min<uint64_t>((end-begin)/0x68,64);
  for(uint32_t i=0;i<count;++i){uint64_t layer0{};if(!Read(begin+0x68ull*i+0x30,&layer0)||!layer0)continue;PfSubSpawners(k,s.c,layer0,(pcx+dx)*16,lowest+16*(int)i,(pcz+dz)*16,found);}}
 std::vector<std::pair<int64_t,PfP>> cand;for(auto& p:found){if(std::abs(p.x-cell.x)>lim||std::abs(p.z-cell.z)>lim)continue;const int64_t a=p.x-cell.x,b=p.y-cell.y,c2=p.z-cell.z;cand.push_back({a*a+b*b+c2*c2,p});}
 std::sort(cand.begin(),cand.end(),[](const std::pair<int64_t,PfP>& a,const std::pair<int64_t,PfP>& b){return a.first<b.first;});for(size_t i=0;i<cand.size()&&i<16;++i)k.spawners.push_back(cand[i].second);}
inline void PfAvoidUpdate(PfController& k,const PfSelf& s,const PfConfig& cfg){
 k.mobRegions.clear();if(!cfg.avoidMobs)return;const PfP cell=PfStartCell(k,s);const int R=std::max(2,cfg.cacheRadius)*16;
 ForEachActor(s.c,[&](uintptr_t a){if(a==s.actor)return true;uintptr_t reg{},el{};uint32_t en{};uint32_t type{};float p[3];
  if(!ActorEntity(a,&reg,&en)||!Element(reg,en,"ActorTypeComponent",4,&el)||!Read(el,&type)||!(type&0x100)||(type&0xb00)!=0xb00||!ActorPos(a,p))return true;
  float lift=0;uintptr_t off{};if(Element(reg,en,"OffsetsComponent",0x44,&off))Read(off,&lift);if(!(lift>=0.f&&lift<8.f))lift=0;
  const PfP m{(int)std::floor(p[0]),(int)std::floor(p[1]-lift),(int)std::floor(p[2])};if(std::abs(cell.x-m.x)<=R&&std::abs(cell.z-m.z)<=R)k.mobRegions.push_back({m,16,6.0});return true;});
 if(k.tick%40==0||k.spawners.empty())PfSpawnerScan(k,s,cfg,cell);for(auto& p:k.spawners)k.mobRegions.push_back({p,64,4.0});}
// Stuck spots: entries expire after 1200 ticks; a spot hit again after more than 19 ticks counts up, and the third time
// resets it and tells the process that no path was found.
inline void PfStuckAdd(PfController& k,const PfP& p){k.stuck.erase(std::remove_if(k.stuck.begin(),k.stuck.end(),[&](const PfStuck& x){return k.tick-x.at>1200;}),k.stuck.end());
 for(auto& x:k.stuck)if(x.p==p){if(k.tick-x.at>19){x.at=k.tick;const int old=x.count++;if(old>1){x.count=0;k.noPath=true;}}return;}k.stuck.push_back({p,k.tick,1});}

// ---- progress watchdog
inline void PfProgressReset(PfController& k){k.progTicks=0;k.progFires=0;k.progBest=1e300;k.giveUp=false;k.unreachable=false;k.failReason.clear();}
inline const char* PfMoveName(uint8_t t){static const char* n[]={"walk","diagonal walk","bridge","step up","step down","fall","parkour jump","pillar up","ladder up","ladder down",
  "swim up","swim down","dig down"};return t<13?n[t]:"move";}
inline std::string PfCellText(const PfP& p){return std::to_string(p.x)+" "+std::to_string(p.y)+" "+std::to_string(p.z);}

// ---- search start and results
inline void PfReleaseInputs(PfShared& sh){sh.active=false;sh.jump=false;sh.sneak=false;sh.holdJump=false;sh.lastJump=false;}
inline void PfReset(PfController& k,const PfSelf* s){PfExecClear(k.exec,s);k.exec.placed.clear();k.proc=kPfNone;k.state=0;k.hasGoal=false;k.goalId=0;k.search.reset();k.replanSearch=false;k.path.clear();k.hasPath=false;k.pathComplete=false;
 k.arrived=false;k.noPath=false;k.status.clear();k.procBreak=k.procPlace=true;k.eating=false;PfProgressReset(k);k.visited.clear();auto& sh=PathFinder();PfReleaseInputs(sh);RotRelease(kRotPathFinder);std::lock_guard<std::mutex> l(sh.guard);sh.pathPoints.clear();sh.thinkPoints.clear();sh.hasGoal=false;}
inline void PfBuildOpt(PfController& k,const PfSelf& s,const PfConfig& cfg,PfOpt& o){o=PfOpt();o.sprint=cfg.allowSprint;o.parkour=cfg.allowParkour;o.brk=cfg.allowBreak&&k.procBreak;o.place=cfg.allowPlace&&k.procPlace;o.maxFall=std::max(1,cfg.maxFall);
 o.blocks=PfThrowawayCount(s,cfg.throwaway);int slot{},dim=0;ReadDimension(&dim);o.bucketFall=cfg.bucketClutch&&PfHotbarName(s,"water_bucket",&slot)&&dim!=1?cfg.maxClutchFall:0;}
// Least cost of entering a block goal: the break cost of its cell and of the head cell above when they hold blocks (a closed
// door costs its opening). Every path into the goal pays at least this, so adding it to the estimate keeps it a lower bound,
// and the search no longer floods a whole cave before accepting to dig the last blocks.
inline double PfGoalEntry(PfView& v,const PfGoal& g){if(!(g.kind==0||(g.kind==1&&g.radius==0)))return 0;double c=0;
 for(int dy=0;dy<2;++dy){const PfP q=PfAdd(g.p,0,dy,0);const PfRec& r=PfRc(v,q);if(!r.known||PfFree(v,q))continue;if(r.door&&v.o.doors)continue;const double b=PfBreakCost(v,q);if(b<kPfInf)c+=b;}
 return c;}
// Goal cell checks (block goals): lava, a block the path finder cannot break, no room for the head, no ground to stand on.
inline std::string PfGoalProblem(PfView& v,const PfP& p){const PfRec& r=PfRc(v,p);if(!r.known)return {};const std::string at=PfCellText(p);
 if(r.cls==2&&!v.o.lava)return at+" is in lava";
 auto blocked=[&](const PfP& q){const PfRec& b=PfRc(v,q);return b.known&&!PfFree(v,q)&&!(b.door&&v.o.doors)&&PfBreakCost(v,q)>=kPfInf;};
 if(blocked(p))return at+(v.o.brk?" is inside a block the path finder cannot break":" is inside a block and Allow Break is off (to stand on it: .goto "+PfCellText(PfAdd(p,0,1,0))+")");
 if(blocked(PfAdd(p,0,1,0)))return "there is no room to stand at "+at+(v.o.brk?" (the block above cannot be broken)":" (solid block above, Allow Break is off)");
 if(PfFree(v,p)&&!PfStand(v,PfAdd(p,0,-1,0))&&!PfSwim(v,p)&&!PfClimb(v,p)&&!(v.o.place&&v.o.blocks>0))return at+" is in the air with nothing to stand on";
 return {};}
// A block goal in the air (nothing to stand on, no water or ladder) can never be a stand cell: it moves to the ground up to
// 3 below. A goal inside a block is kept as typed: reaching it means digging into it (as Baritone's block goal).
// Returns the note to tell the player, empty when the goal is kept.
inline std::string PfSnapGoal(PfController& k,const PfSelf& s,const PfConfig& cfg,PfGoal& g){
 if(g.kind!=0)return {};PfWorldBegin(k.world,s.c,cfg,s.cell.x,s.cell.z);PfView v;v.w=&k.world;PfBuildOpt(k,s,cfg,v.o);
 const PfP p=g.p;const PfRec& r=PfRc(v,p);if(!r.known)return {};
 if(PfPass2(v,p)&&!PfStand(v,PfAdd(p,0,-1,0))&&!PfSwim(v,p)&&!PfClimb(v,p))
  for(int d=1;d<=3;++d){const PfP q=PfAdd(p,0,-d,0);if(!PfFree(v,q))break;if(PfStand(v,PfAdd(q,0,-1,0))||PfSwim(v,q)||PfClimb(v,q)){g.p=q;return PfCellText(p)+" is in the air, going to the ground below at "+PfCellText(q);}}
 return {};}
inline void PfStartSearch(PfController& k,const PfSelf& s,const PfConfig& cfg,bool replan,const PfP* from=nullptr){
 PfWorldBegin(k.world,s.c,cfg,s.cell.x,s.cell.z);auto S=std::make_unique<PfSearch>();S->view.w=&k.world;PfBuildOpt(k,s,cfg,S->view.o);k.lastOpt=S->view.o;
 S->goal=k.goal;S->goal.extra=PfGoalEntry(S->view,S->goal);S->start=from?*from:PfStartCell(k,s);k.contSearch=from!=nullptr;if(!from)k.contPending=false;S->grounded=!from&&(s.onGround||s.inWater||s.climb);S->greed=std::max(1.0,(double)cfg.searchGreed);S->maxMs=std::max(250,cfg.searchTime);S->cap=S->view.o.blocks;S->view.regions=k.mobRegions;
 for(auto& x:k.stuck)if(k.tick-x.at<=1200)S->view.regions.push_back({x.p,0,20.0});
 PfSearchBegin(*S);k.search=std::move(S);k.replanSearch=replan;if(!replan){k.path.clear();k.hasPath=false;k.pathComplete=false;}}
// New search from the current position, at least 500 ms after the previous one (the path is followed meanwhile).
inline void PfReplan(PfController& k,const PfSelf& s,const PfConfig& cfg){if(!k.hasGoal||(k.search&&!k.replanSearch))return;const int64_t now=PfNow();if(ActionMs(k.planAt)<=500.0)return;k.planAt=now;PfStartSearch(k,s,cfg,true);}
// Progress watchdog (every tick a path is followed): progress = entering a cell not visited in the last 600 ticks, or the goal
// estimate improving by a block; any progress clears the count. 200 ticks of steering without progress (breaking, placing, doors and searches do not count)
// mark the next step's cell as stuck and start a new search; the third time in a row the process is told the goal cannot be
// reached, with the place and the move that kept failing.
inline void PfWatch(PfController& k,const PfSelf& s,const PfConfig& cfg,const PfP& cell,const PfOut& out){
 bool progress=false;const uint64_t key=PfKey(cell);auto f=k.visited.find(key);if(f==k.visited.end()||k.tick-f->second>600)progress=true;k.visited[key]=k.tick;
 if(k.visited.size()>4096)for(auto it=k.visited.begin();it!=k.visited.end();)if(k.tick-it->second>600)it=k.visited.erase(it);else ++it;
 const double h=k.goal.h(cell);if(h<k.progBest-kPfSprint){k.progBest=h;progress=true;k.progFires=0;k.gotoS.fails=0;}
 if(progress){k.progTicks=0;k.progFires=0;return;}
 if(out.acted||out.status==1||k.path.empty()||k.search)return;
 if(++k.progTicks<200)return;
 k.progTicks=0;const PfStep& st=k.path[0];
 if(++k.progFires>=3){k.progFires=0;k.giveUp=true;k.noPath=true;
  k.failReason="no progress for 30 s around "+PfCellText(cell)+", the next move ("+PfMoveName(st.type)+" to "+PfCellText(st.to)+") keeps failing";return;}
 PfStuckAdd(k,st.to);k.planAt=0;PfReplan(k,s,cfg);}
// Proof of unreachability: every cell reachable from the player's own standing cell (not a later segment's start, not a
// cell in the air) was expanded without reaching an unloaded chunk or the Cache Radius, and no placement budget pruned a
// route (nodes are keyed by position only, so a cheaper route with more placed blocks can hide one within the budget).
inline bool PfProven(const PfSearch& S,const PfWorld& w,bool continued){return S.status==2&&!w.edge&&!continued&&S.grounded&&S.nodes.size()>1&&(S.cap==0||!S.view.o.place);}
// Search result: a full or partial path replaces the current one; no path at all tells the process (no path notification).
inline void PfFinishSearch(PfController& k,const PfSelf& s,bool){
 PfSearch& S=*k.search;bool ok=false,complete=false;
 const bool proven=PfProven(S,k.world,k.contSearch);
 if(S.status==1){ok=PfBuildPath(S,S.endNode);complete=ok;}else if(!proven&&PfPartial(S))ok=true;
 {auto& sh=PathFinder();std::lock_guard<std::mutex> l(sh.guard);sh.thinkPoints=S.sample;}
 const bool cont=k.contSearch;k.contSearch=false;
 // A next segment is appended when it starts where the current path ends; when the player already walked the whole
 // partial path, the segment (which starts where that path ended) becomes the path. A segment that no longer joins the
 // path is dropped and searched again: marking the path complete without it would report an arrival short of the goal.
 // A partial result schedules the next segment.
 bool joined=true;
 if(ok){
  if(!cont||!k.hasPath)k.path=S.moves;
  else if(!k.path.empty()&&!S.moves.empty()&&S.moves.front().from==k.path.back().to)k.path.insert(k.path.end(),S.moves.begin(),S.moves.end());
  else if(k.path.empty())k.path=S.moves;
  else joined=false;}
 if(ok&&!joined){k.contPending=true;k.contNs=PfNow();}
 else if(ok){k.hasPath=true;k.pathComplete=complete;k.exec.stall=0;k.retry=false;k.contPending=!complete;if(!complete)k.contNs=PfNow();}
 else{k.noPath=true;if(!k.hasPath){k.retry=true;k.retryNs=PfNow();}
  // why: a problem with the goal cell itself, else the closest cell the search could reach
  uint32_t best=0;for(uint32_t i=1;i<S.nodes.size();++i)if(S.nodes[i].h<S.nodes[best].h)best=i;const PfP closest=S.nodes.empty()?S.start:S.nodes[best].p;
  std::string why=S.goal.kind==0||(S.goal.kind==1&&S.goal.radius==0)?PfGoalProblem(S.view,S.goal.p):std::string();
  if(why.empty())why=proven?"every cell reachable from here was explored ("+std::to_string(S.nodes.size())+"), the closest is "+PfCellText(closest)
   :"no route found within the search time, the closest reachable cell is "+PfCellText(closest);
  k.failReason=why;if(proven)k.unreachable=true;}
 k.search.reset();k.replanSearch=false;(void)s;}
inline void PfSearchTick(PfController& k,const PfSelf& s){
 PfSearch& S=*k.search;const int64_t t0=PfNow();while(S.status==0&&ActionMs(t0)<4.0&&ActionMs(S.began)<S.maxMs)PfSearchStep(S,32);
 if(S.status!=0)PfFinishSearch(k,s,false);else if(ActionMs(S.began)>=S.maxMs)PfFinishSearch(k,s,true);}
// Validity (every 10 ticks): from the second step on, each step's move must still be generated from its start cell with a finite cost.
inline bool PfPathValid(PfController& k){if(k.path.size()<2)return true;k.world.cells.clear();PfView v;v.w=&k.world;v.o=k.lastOpt;std::vector<PfMove> moves;
 for(size_t i=1;i<k.path.size();++i){const PfStep& st=k.path[i];moves.clear();PfMoves(v,st.from,moves);bool found=false;
  for(auto& m:moves)if(m.type==st.type&&m.pos==st.to&&m.cost<kPfInf){found=true;break;}if(!found)return false;}return true;}

// ---- processes
// Goto: finished when the path arrived or the goal holds (search cell, or the cell of the feet when standing on a slab /
// path block); fails at once when the goal is proven unreachable or the watchdog gave up, else on the third no-path
// notification; the failure says why.
inline void PfGotoTick(PfController& k,const PfSelf& s,const PfP& cell,PfProcOut& o){auto& g=k.gotoS;
 if(k.arrived||g.goal.at(cell)||g.goal.at(s.cell)){o.result=1;o.message="Arrived at "+g.label+".";return;}
 const std::string why=k.failReason.empty()?std::string():": "+k.failReason;
 if(k.unreachable||k.giveUp){o.result=2;o.message="Cannot reach "+g.label+why+".";return;}
 if(k.noPath&&g.fails++>1){o.result=2;o.message="Could not find a path to "+g.label+why+".";return;}
 o.hasGoal=true;o.goal=g.goal;o.goalId=g.goalId;o.status="Going to "+g.label;}
// Wander: a new column goal (random angle, distance radius x (0.5..1)) whenever there is none, it was reached or had no path.
inline void PfWanderTick(PfController& k,const PfP& cell,PfProcOut& o){auto& w=k.wander;
 if(!w.hasGoal||k.arrived||k.noPath||PfGoal{2,w.target}.at(cell)){
  const float a=(float)(w.rng()>>8)*5.960464477539063e-08f*6.2831854820251465f;const float d=((float)(w.rng()>>8)*5.960464477539063e-08f*0.5f+0.5f)*(float)w.radius;
  w.target=PfP{(int)(cosf(a)*d)+cell.x,cell.y,(int)(sinf(a)*d)+cell.z};w.hasGoal=true;w.goalId=k.nextId++;}
 o.hasGoal=true;o.goal.kind=2;o.goal.p=w.target;o.goalId=w.goalId;o.status="Wandering to "+std::to_string(w.target.x)+" "+std::to_string(w.target.z);}
// Follow target: a player other than the local one whose name equals (ASCII case-insensitive); feet position, dead (health <= 0), airborne.
inline bool PfFindTarget(const PfSelf& s,const std::string& name,float* feet,bool* dead,bool* air){bool found=false;const std::string want=PfLower(name);
 ForEachActor(s.c,[&](uintptr_t a){if(a==s.actor)return true;uint64_t id{};char nm[64]{};if(!RuntimeId(a,&id)||!PlayerName(id,nm,sizeof(nm)))return true;if(PfLower(nm)!=want)return true;
  float p[3];uintptr_t reg{},off{};uint32_t en{};if(!ActorPos(a,p)||!ActorEntity(a,&reg,&en))return true;float lift=0;if(Element(reg,en,"OffsetsComponent",0x44,&off))Read(off,&lift);if(!(lift>=0.f&&lift<8.f))lift=0;
  feet[0]=p[0];feet[1]=p[1]-lift;feet[2]=p[2];float cur{},mx{};*dead=Attribute(reg,en,6,&cur,&mx)&&cur<=0.f;*air=!ActionOnGround(a);found=true;return false;});return found;}
inline void PfFollowTick(PfController& k,const PfSelf& s,const PfP& cell,PfProcOut& o){auto& f=k.follow;const int64_t now=PfNow();float p[3];bool dead=false,air=false;
 const bool found=PfFindTarget(s,f.name,p,&dead,&air);
 if(!found||dead){
  if(!f.lost){f.lost=true;f.lostNs=now;f.lostTimer=true;}
  if(f.lostTimer&&ActionMs(f.lostNs)>30000.0){o.result=1;o.message="Lost track of "+f.name+".";return;}
  f.close=false;
  if(dead){f.hasGoal=false;o.status="Waiting for "+f.name+" to respawn";return;}
  if(!f.hasLast){f.hasGoal=false;o.status="Waiting for "+f.name+" to come into view";return;}
  if(f.lastSeenGoal&&(k.arrived||k.noPath||PfGoal{1,f.last,1}.at(cell)))f.chaseLast=false;
  if(!f.chaseLast){f.hasGoal=false;o.status="Waiting for "+f.name+" to come into view";return;}
  if(!f.lastSeenGoal){f.lastSeenGoal=true;f.lastSeenId=k.nextId++;}
  o.hasGoal=true;o.goal.kind=1;o.goal.p=f.last;o.goal.radius=1;o.goalId=f.lastSeenId;o.status="Lost "+f.name+", heading to their last seen position";return;}
 if(f.lost){f.lost=false;f.lastSeenGoal=false;f.hasGoal=false;f.chaseLast=true;}
 if(!(air&&f.hasLast)){f.last=PfP{(int)std::floor(p[0]),(int)std::floor(p[1]),(int)std::floor(p[2])};f.hasLast=true;}
 const PfP t=f.last;const float dx=(float)(t.x-cell.x),dy=(float)(t.y-cell.y),dz=(float)(t.z-cell.z);const float dist=std::sqrt(dz*dz+dy*dy+dx*dx);
 if(dist<=f.distance||(dist<=1.5f+f.distance&&f.close)){f.close=true;f.hasGoal=false;}
 else{f.close=false;bool make=!f.hasGoal;
  if(f.hasGoal){const float thr=std::max(f.distance*0.5f,2.0f);const float mx=(float)(t.x-f.goalAt.x),my=(float)(t.y-f.goalAt.y),mz=(float)(t.z-f.goalAt.z);
   if(!(std::sqrt(mz*mz+my*my+mx*mx)<=thr)&&f.goalTimer&&ActionMs(f.goalNs)>500.0)make=true;}
  if(make){f.goalAt=t;f.hasGoal=true;f.goalId=k.nextId++;f.goalNs=now;f.goalTimer=true;}}
 o.retarget=true;o.status="Following "+f.name;if(f.hasGoal){o.hasGoal=true;o.goal.kind=1;o.goal.p=f.goalAt;o.goal.radius=f.radius;o.goalId=f.goalId;}}
// Mine / Chop block pattern against a block id: "*core", "core*" or "*core*" -> core contained in the id; an id with a namespace
// -> equal; otherwise the id without its namespace equals the pattern, or the ore names: pattern (+ "_ore" unless it already
// ends so), "deepslate_" + that, "nether_" + that.
inline bool PfMinePattern(const std::string& full,const std::string& pat){
 if(pat.empty())return full.empty();
 if(pat[0]=='*'||pat.back()=='*'){const size_t lead=pat[0]=='*'?1:0;size_t core=pat.size()-lead;if(core&&pat.back()=='*')--core;
  if(core==0||core>full.size())return false;return full.find(pat.substr(lead,core))!=std::string::npos;}
 if(pat.find(':')!=std::string::npos)return full==pat;
 if(full.empty())return false;std::string name=full;const size_t colon=name.find(':');if(colon!=std::string::npos)name.erase(0,colon+1);
 if(name==pat)return true;const std::string base=pat.size()>=4&&pat.compare(pat.size()-4,4,"_ore")==0?pat:pat+"_ore";
 return name==base||name=="deepslate_"+base||name=="nether_"+base;}
inline bool PfMineMatch(const std::vector<std::string>& patterns,const std::string& name){for(auto& t:patterns)if(PfMinePattern(name,t))return true;return false;}
inline void PfMineScan(PfController& k,const PfSelf& s,const PfP& cell){auto& m=k.mine;std::vector<PfP> found;
 if(s.region){const uint64_t chunkAt=Virtual(s.c,s.region,0x148);int16_t lowest{};if(chunkAt&&Read(s.region+0x3a,&lowest)){const int R=(m.radius+15)/16,pcx=cell.x>>4,pcz=cell.z>>4;
   for(int dx=-R;dx<=R;++dx)for(int dz=-R;dz<=R;++dz){const uint64_t chunk=PfChunkPtr(chunkAt,s.region,pcx+dx,pcz+dz);if(!chunk)continue;uint64_t begin{},end{};
    if(!Read(chunk+0x1c8,&begin)||!Read(chunk+0x1d0,&end)||end<begin)continue;const uint32_t count=(uint32_t)std::min<uint64_t>((end-begin)/0x68,64);
    for(uint32_t i=0;i<count;++i){uint64_t layer0{};if(!Read(begin+0x68ull*i+0x30,&layer0)||!layer0)continue;
     const uint64_t palette=Virtual(s.c,layer0,0xa8),bitsOf=Virtual(s.c,layer0,0xa0),wordsOf=Virtual(s.c,layer0,0x90);if(!palette||!bitsOf||!wordsOf)continue;
     uint64_t pl[2]{};if(!PfPaletteRaw(palette,layer0,pl)||!pl[0]||!pl[1])continue;const uint32_t entries=(uint32_t)std::min<uint64_t>(pl[0],4096);std::vector<uint8_t> hit(entries,0);bool any=false;
     for(uint32_t e=0;e<entries;++e){uint64_t block{};if(!Read(pl[1]+8ull*e,&block)||!block)continue;auto f=m.match.find(block);uint8_t v;
      if(f==m.match.end()){std::string n;v=XrayBlockName((uintptr_t)block,&n)&&PfMineMatch(m.patterns,n)?1:0;m.match.emplace(block,v);}else v=f->second;hit[e]=v;any=any||v;}
     if(!any)continue;const int bx=(pcx+dx)*16,by=lowest+16*(int)i,bz=(pcz+dz)*16;const int bits=PfBitsRaw(bitsOf,layer0);
     if(bits<=0){if(hit[0])for(int at=0;at<4096;++at)found.push_back(PfP{bx+(at>>8),by+(at&15),bz+((at>>4)&15)});continue;}if(bits>32)continue;
     uint64_t words[2]{};if(!PfPaletteRaw(wordsOf,layer0,words)||!words[0]||!words[1])continue;const uint32_t per=32/bits,mask=bits==32?0xffffffffu:((1u<<bits)-1);
     for(uint32_t at=0;at<4096;++at){const uint32_t word=at/per;uint32_t value{};if(word>=words[0]||!Read(words[1]+4ull*word,&value))continue;const uint32_t pick=(value>>((at%per)*bits))&mask;
      if(pick<entries&&hit[pick])found.push_back(PfP{bx+(int)(at>>8),by+(int)(at&15),bz+(int)((at>>4)&15)});}}}}}
 std::vector<std::pair<int64_t,PfP>> near96;for(auto& p:found){if(std::abs(p.x-cell.x)>m.radius||std::abs(p.z-cell.z)>m.radius)continue;const int64_t a=p.x-cell.x,b=p.y-cell.y,c2=p.z-cell.z;near96.push_back({a*a+b*b+c2*c2,p});}
 std::sort(near96.begin(),near96.end(),[](const std::pair<int64_t,PfP>& a,const std::pair<int64_t,PfP>& b){return a.first<b.first;});if(near96.size()>96)near96.resize(96);
 std::vector<PfP> list;for(auto& e:near96){auto f=m.skip.find(PfKey(e.second));if(f!=m.skip.end()&&m.tick<f->second)continue;list.push_back(e.second);if(list.size()>=24)break;}
 bool same=list.size()==m.targets.size();for(size_t i=0;same&&i<list.size();++i)same=list[i]==m.targets[i];if(!same)m.anyGoal=false;m.targets=list;}
inline void PfMineRemove(PfMineState& m,const PfP& p){m.targets.erase(std::remove_if(m.targets.begin(),m.targets.end(),[&](const PfP& q){return q==p;}),m.targets.end());}
inline std::string PfMineStatus(const PfMineState& m){return m.count<1?"Mining ("+std::to_string(m.mined)+" mined)":"Mining ("+std::to_string(m.mined)+"/"+std::to_string(m.count)+")";}
// Target that can be mined from here: within reach of the eye with a passable neighbour; Fast Mine takes the one closest to the view.
inline bool PfMinePick(const PfSelf& s,const PfMineState& m,float reach,bool fast,PfP* out){static const int nb[6][3]={{0,-1,0},{0,1,0},{0,0,-1},{0,0,1},{-1,0,0},{1,0,0}};bool any=false;float best=3.4028234663852886e+38f;
 for(auto& t:m.targets){const float dy=t.y+.5f-s.eye[1],dx=t.x+.5f-s.eye[0],dz=t.z+.5f-s.eye[2];if(!(dz*dz+dy*dy+dx*dx<=reach*reach))continue;if(fast&&PfReplaceableAt(s,t))continue;
  bool open=false;for(auto& n:nb)if(PfCellClear(s,PfAdd(t,n[0],n[1],n[2]))){open=true;break;}if(!open)continue;
  if(!fast){*out=t;return true;}
  const float pitch=std::atan2(dy,std::sqrt(dx*dx+dz*dz))*-57.29578f-s.pitch,yaw=fmodf(std::atan2(-dx,dz)*57.29578f-s.yaw,360.f);const float v=pitch*pitch+yaw*yaw;if(v<best){best=v;*out=t;any=true;}}
 return any;}
// ---- Junk Items: entries containing the core of a mined pattern (namespace and surrounding '*' removed) are kept; a junk
// stack is first looked for in the main inventory (slots 9 and up), then in the hotbar, where the first junk stack that is a
// throwaway block is kept. Dropping uses the game's Inventory::dropSlot (RVA 0x2af140, whole stack), at most every 1000 ms,
// not while the cursor holds an item.
inline bool PfJunkProtected(const std::vector<std::string>& patterns,const std::string& bare){
 for(auto t:patterns){const size_t colon=t.find(':');if(colon!=std::string::npos){t.erase(0,colon+1);if(t.empty())continue;}
  size_t a=0;while(a<t.size()&&t[a]=='*')++a;size_t b=t.size();while(b>a&&t[b-1]=='*')--b;if(a==b)continue;const std::string core=t.substr(a,b-a);
  if(core.size()<=bare.size()&&bare.find(core)!=std::string::npos)return true;}return false;}
inline bool PfJunkStack(uint64_t stack,const std::vector<std::string>& junk){uint8_t cnt{};if(!ActionItem(stack)||!Read(stack+0x22,&cnt)||!cnt)return false;std::string n;if(!ActionName(stack,&n))return false;
 const std::string id=PfFullId(n);for(auto& j:junk)if(PfFullId(j)==id)return true;return false;}
inline bool PfJunkSlot(const PfSelf& s,const std::vector<std::string>& junk,const std::vector<std::string>& throwaway,int* slot){const uint64_t box=ActionContainer(s.actor);if(!box)return false;
 const int size=ActionSize(s.c,box);for(int i=9;i<size;++i)if(PfJunkStack(ActionStackAt(s.c,box,i),junk)){*slot=i;return true;}
 bool kept=false;const int n=std::min(size,9);for(int i=0;i<n;++i){const uint64_t st=ActionStackAt(s.c,box,i);if(!PfJunkStack(st,junk))continue;
  if(kept){*slot=i;return true;}kept=true;if(!PfThrowawayMatch(st,throwaway)){*slot=i;return true;}}return false;}
inline void PfJunkDrop(PfController& k,const PfSelf& s,const std::vector<std::string>& junk,const std::vector<std::string>& throwaway){
 if(ActionCursorBusy(s.c,s.actor)||!k.junkTimer||ActionMs(k.junkNs)<=1000.0)return;k.junkNs=PfNow();k.junkTimer=true;
 int slot=-1;if(!PfJunkSlot(s,junk,throwaway,&slot))return;const uint64_t inv=ActionContainer(s.actor);if(!inv)return;
 reinterpret_cast<void(*)(uint64_t,int,bool,bool,bool)>(s.c.base+Rva(0x2af140))(inv,slot,false,true,false);}
inline bool PfInventoryFull(const PfSelf& s){const uint64_t box=ActionContainer(s.actor);if(!box)return false;const int n=std::min(ActionSize(s.c,box),36);
 for(int i=0;i<n;++i){const uint64_t st=ActionStackAt(s.c,box,i);uint8_t cnt{};if(!ActionItem(st)||!Read(st+0x22,&cnt)||!cnt)return false;}return true;}
inline void PfMineTick(PfController& k,const PfSelf& s,const PfConfig& cfg,const PfP& cell,float reach,PfProcOut& o){auto& m=k.mine;++m.tick;const bool fast=cfg.fastMine;
 // inventory: junk (Junk Mode Off / When Full / Always), then Stop When Full
 {const bool full=PfInventoryFull(s);const int mode=cfg.junkMode==2?0:(cfg.junkMode==1?2:1);bool junkFound=false;
  if(mode!=0){std::vector<std::string> junk;for(auto& j:cfg.junkItems){std::string bare=j;const size_t colon=bare.find(':');if(colon!=std::string::npos)bare.erase(0,colon+1);if(!PfJunkProtected(m.patterns,bare))junk.push_back(j);}
   int slot=-1;if(!junk.empty()&&PfJunkSlot(s,junk,cfg.throwaway,&slot)){junkFound=true;if(full||mode==2)PfJunkDrop(k,s,junk,cfg.throwaway);
    if(full){o.status="Dropping junk to make room ("+std::to_string(m.mined)+" mined)";return;}}}
  if(!junkFound&&full&&cfg.stopWhenFull){o.result=1;o.message="Inventory full; stopped after mining "+std::to_string(m.mined)+" block(s).";return;}}
 // the fixed target is being broken
 if(m.fixed){const int r=PfBreak(k.exec,s,m.target,reach);bool counted=false;
  if(r==2){m.skip[PfKey(m.target)]=m.tick+600;}
  else if(r==0&&!(fast&&PfReplaceableAt(s,m.target))){o.status=PfMineStatus(m);return;}
  else{if(r==0){k.exec.hasLook=false;k.exec.fixedOn=false;}counted=true;}
  if(counted){auto f=m.tries.find(PfKey(m.target));if(f==m.tries.end()||f->second.count==1)++m.mined;
   if(!fast){m.pickup=PfP{m.target.x,m.target.y<=cell.y+1?m.target.y:cell.y,m.target.z};m.collecting=true;m.pickupTicks=60;m.pickupId=k.nextId++;}}
  PfMineRemove(m,m.target);m.fixed=false;m.anyGoal=false;m.rescan=0;}
 // walking over the drops for up to 60 ticks
 if(m.collecting){const int old=m.pickupTicks--;PfGoal g;g.kind=5;g.p=m.pickup;
  if(1<old&&!k.noPath&&!g.at(cell)){o.hasGoal=true;o.goal=g;o.goalId=m.pickupId;o.status=PfMineStatus(m);return;}m.collecting=false;}
 if(m.count>0&&m.count<=m.mined){o.result=1;o.message="Finished mining "+std::to_string(m.mined)+" block(s).";return;}
 if(k.noPath&&m.anyGoal){for(auto& t:m.targets)m.skip[PfKey(t)]=m.tick+600;m.anyGoal=false;m.rescan=0;}
 if(--m.rescan<=0){m.rescan=20;PfMineScan(k,s,cell);}
 if(m.targets.empty()){   // searching: a random column goal within 48 blocks, renewed when reached or without a path
  PfGoal g;g.kind=2;g.p=m.searchAt;
  if(!m.searching||k.arrived||k.noPath||g.at(cell)){auto pick=[&]{uint64_t v;do{v=(uint64_t)m.rng()*0x61ull;}while((uint32_t)v<0x23u);return (int)(v>>32)-0x30;};
   const int x=cell.x+pick();const int z=cell.z+pick();m.searchAt=PfP{x,cell.y,z};m.searching=true;m.searchId=k.nextId++;g.p=m.searchAt;}
  o.hasGoal=true;o.goal=g;o.goalId=m.searchId;o.status="Searching for blocks to mine ("+std::to_string(m.mined)+" mined)";return;}
 PfP cand;if(PfMinePick(s,m,reach,fast,&cand)){
  for(auto it=m.tries.begin();it!=m.tries.end();)it=(m.tick-it->second.at)<0x259?std::next(it):m.tries.erase(it);
  PfMineTry& t=m.tries[PfKey(cand)];t.at=m.tick;const int old=t.count++;
  if(old<3){m.target=cand;m.fixed=true;if(fast)PfAimFace(k.exec,cand,PfFace(s,cand));o.status=PfMineStatus(m);return;}
  m.tries.erase(PfKey(cand));m.skip[PfKey(cand)]=m.tick+600;PfMineRemove(m,cand);m.anyGoal=false;m.rescan=0;o.status=PfMineStatus(m);return;}
 if(!m.anyGoal){m.anyGoal=true;m.anyId=k.nextId++;}
 o.hasGoal=true;o.goal.kind=4;o.goal.list=m.targets;o.goalId=m.anyId;o.retarget=true;o.status=PfMineStatus(m);}
// Tunnel: 1x2 along the facing axis from the start cell; progress = forward distance while on the start level; liquid next to
// the next cells stops it, no path stops it; the goal moves up to 8 blocks ahead, or to the first cell without a standable floor
// (placing is then allowed).
inline bool PfLiquidNear(PfController& k,const PfP& p){static const int o[6][3]={{0,-1,0},{0,1,0},{0,0,-1},{0,0,1},{-1,0,0},{1,0,0}};for(auto& d:o)if(PfRecAt(k.world,PfAdd(p,d[0],d[1],d[2])).cls)return true;return false;}
inline void PfTunnelTick(PfController& k,const PfP& cell,PfProcOut& o){auto& t=k.tunnel;
 if(!t.started){t.started=true;t.origin=cell;}
 int prog=0;if(cell.y==t.origin.y)prog=(cell.z-t.origin.z)*t.dz+(cell.x-t.origin.x)*t.dx;if(prog<t.progress)prog=t.progress;t.progress=prog;
 if(t.length>0&&t.length<=prog){o.result=1;o.message="Tunnel finished ("+std::to_string(prog)+" blocks).";return;}
 const PfP next{t.dx*(prog+1)+t.origin.x,t.origin.y,(prog+1)*t.dz+t.origin.z};
 if(PfLiquidNear(k,next)||PfLiquidNear(k,PfAdd(next,0,1,0))){o.result=2;o.message="Stopped tunnelling before breaking into liquid ("+std::to_string(prog)+" blocks).";return;}
 if(k.noPath){o.result=2;o.message="Could not dig further ("+std::to_string(prog)+" blocks).";return;}
 if(!t.hasGoal||t.goalIdx-prog<4){int lim=prog+8;if(t.length>=1&&t.length<lim)lim=t.length;int i=prog;
  while(i<lim){const int j=i+1;const PfRec& r=PfRecAt(k.world,PfP{t.dx*j+t.origin.x,t.origin.y-1,t.dz*j+t.origin.z});if(!r.solid)break;i=j;}
  t.gap=i<lim;t.goalIdx=lim;
  t.goalAt=PfP{t.dx*t.goalIdx+t.origin.x,t.origin.y,t.goalIdx*t.dz+t.origin.z};t.hasGoal=true;t.goalId=k.nextId++;}
 o.hasGoal=true;o.goal.kind=0;o.goal.p=t.goalAt;o.goalId=t.goalId;o.brk=true;o.place=t.gap;o.retarget=true;
 o.status=t.length<1?"Tunnelling ("+std::to_string(prog)+" blocks)":"Tunnelling ("+std::to_string(prog)+"/"+std::to_string(t.length)+" blocks)";}

// ---- Build
// Schematic queries (schematic manager of game_schematic.hpp, same game thread; its lock is taken). A schematic counts when it
// is shown and its palette is resolved; cells are read in layer 0 through the schematic's own transform (ScToLocal).
inline bool PfSchemReady(const ScSchem& sc){return !sc.data.palette.empty()&&sc.resolved.size()==sc.data.palette.size();}
inline bool PfSchemShown(){auto& sm=Schematics();std::lock_guard<std::recursive_mutex> l(sm.guard);for(auto& sc:sm.list)if(sc->show&&PfSchemReady(*sc))return true;return false;}
inline int32_t PfSchemIndex(const ScSchem& sc,const PfP& p){const int w[3]={p.x,p.y,p.z};int l[3];if(!ScToLocal(sc,w,l))return -1;const size_t i=nbt::Index(sc.data,l[0],l[1],l[2]);return i<sc.data.l0.size()?sc.data.l0[i]:-1;}
inline bool PfSchemSolid(const ScSchem& sc,int32_t idx){return idx>=0&&(size_t)idx<sc.data.palette.size()&&sc.data.palette[(size_t)idx].name!="minecraft:air";}   // every entry but minecraft:air
inline uint64_t PfSchemBlock(const ScSchem& sc,int32_t idx){return idx>=0&&(size_t)idx<sc.resolved.size()?sc.resolved[(size_t)idx]:0;}
// Half of a slab / stair block: vertical_half "top" -> 2 (other value 1), else upside_down_bit set -> 2 (clear 1), else 0.
inline uint8_t PfBuildHalf(const Context& c,uint64_t block){const uint64_t states=block?SusTag(c,block+0xf0,"states",10):0;if(!states)return 0;
 if(const uint64_t t=SusTag(c,states,"minecraft:vertical_half",8)){std::string v;SusString(t+8,&v);return v=="top"?2:1;}
 if(const uint64_t t=SusTag(c,states,"upside_down_bit",1)){uint8_t v{};Read(t+8,&v);return v?2:1;}return 0;}
// Cells of the shown schematics that differ from the world, sorted by Y, then horizontal squared distance to the player cell;
// the first `cap` are kept.
inline void PfSchemMissing(const PfSelf& s,const PfP& cell,size_t cap,std::vector<PfBuildTarget>& out){out.clear();if(!s.region)return;
 {auto& sm=Schematics();std::lock_guard<std::recursive_mutex> l(sm.guard);std::unordered_map<uint64_t,uint8_t> halves;
  for(auto& sc:sm.list){if(!sc->show||!PfSchemReady(*sc))continue;int W=0,H=0,D=0;ScDims(*sc,&W,&H,&D);
   for(int y=0;y<H;++y)for(int z=0;z<D;++z)for(int x=0;x<W;++x){const PfP p{sc->px+x,sc->py+y,sc->pz+z};const int32_t idx=PfSchemIndex(*sc,p);if(idx<0)continue;
    const uint64_t have=PfLiveBlock(s,p);
    if(!PfSchemSolid(*sc,idx)){if(BlockReplaceable(have))continue;PfBuildTarget t;t.p=p;t.occupied=true;out.push_back(t);continue;}
    const uint64_t want=PfSchemBlock(*sc,idx);if(!want||have==want)continue;
    auto h=halves.find(want);if(h==halves.end())h=halves.emplace(want,PfBuildHalf(s.c,want)).first;
    PfBuildTarget t;t.p=p;t.block=want;t.half=h->second;t.occupied=!BlockReplaceable(have);out.push_back(t);}}}
 std::sort(out.begin(),out.end(),[&](const PfBuildTarget& a,const PfBuildTarget& b){if(a.p.y!=b.p.y)return a.p.y<b.p.y;
  const int ax=a.p.x-cell.x,az=a.p.z-cell.z,bx=b.p.x-cell.x,bz=b.p.z-cell.z;return (uint32_t)(ax*ax+az*az)<(uint32_t)(bx*bx+bz*bz);});
 if(out.size()>cap)out.resize(cap);}
// The world block at p is the block a shown schematic wants there.
inline bool PfSchemMatch(const PfP& p,uint64_t have){auto& sm=Schematics();std::lock_guard<std::recursive_mutex> l(sm.guard);
 for(auto& sc:sm.list){if(!sc->show||!PfSchemReady(*sc))continue;const int32_t idx=PfSchemIndex(*sc,p);if(idx<0||!PfSchemSolid(*sc,idx))continue;if(PfSchemBlock(*sc,idx)==have)return true;}return false;}
// Stack holding a block: count byte (+0x22) set and the stack's block (+0x18) of the same BlockLegacy.
inline bool PfBuildHolds(uint64_t stack,uint64_t want){uint64_t blk{},a{},b{};uint8_t cnt{};return stack&&want&&Read(stack+0x18,&blk)&&blk&&Read(stack+0x22,&cnt)&&cnt&&Read(blk+0x68,&a)&&a&&Read(want+0x68,&b)&&a==b;}
inline void PfBuildNeed(PfBuildState& b,uint64_t block){uint64_t legacy{};char nm[96]{};if(block&&Read(block+0x68,&legacy)&&legacy)Text(legacy+0xe8,nm,sizeof(nm));++b.missing[nm];}
inline std::string PfBuildNeedText(const PfBuildState& b){if(b.missing.empty())return "-";std::string t;for(auto& [n,c]:b.missing){if(!t.empty())t+=", ";t+=n+" x"+std::to_string(c);}return t;}
inline void PfBuildStatus(const PfBuildState& b,PfProcOut& o){o.brk=false;o.place=true;o.status="Building ("+std::to_string(b.placed)+" placed, "+std::to_string(b.targets.size())+" left nearby)";}
// Done with a cell (placed or cleared): counted, its entries removed, placing stops.
inline void PfBuildRemove(PfBuildState& b,const PfP& p){++b.placed;b.targets.erase(std::remove_if(b.targets.begin(),b.targets.end(),[&](const PfBuildTarget& t){return t.p==p;}),b.targets.end());b.placing=false;}
inline void PfBuildScaffoldDrop(PfBuildState& b,const PfP& p){b.scaffold.erase(std::remove(b.scaffold.begin(),b.scaffold.end(),p),b.scaffold.end());b.scaffoldSet.erase(PfKey(p));}
// Game clip (BlockSource vtable +0x198, liquids ignored, steps given): block hit -> cell, face, hit point.
inline bool PfClip(const PfSelf& s,const float* from,const float* to,int steps,int32_t* cell,uint8_t* face,float* hit){
 const uint64_t clipOf=s.region?Virtual0(s.c,s.region,0x198):0;if(!clipOf)return false;
 using Clip=void*(*)(uint64_t,void*,const float*,const float*,bool,bool,int,bool,bool,uint64_t,const std::function<bool()>*,bool);
 static const std::function<bool()> accept=[]{return true;};alignas(16) uint8_t res[0x200]{};
 reinterpret_cast<Clip>(clipOf)(s.region,res,from,to,false,true,steps,false,false,s.actor,&accept,false);
 uint64_t weak{};memcpy(&weak,res+0x40,8);if(weak&&InterlockedDecrement((volatile long*)(weak+0xc))==0){uint64_t t{},rel{};if(Read(weak,&t)&&Read(t+8,&rel)&&rel)reinterpret_cast<void(*)(uint64_t)>(rel)(weak);}
 int type=-1;memcpy(&type,res+0x18,4);if(type!=0)return false;*face=res[0x1c];memcpy(cell,res+0x20,12);memcpy(hit,res+0x2c,12);return true;}
// Placement options: allowed faces (bit = face), half (click height 0.25 / 0.5 / 0.75 on side faces; bottom skips the block above,
// top the block below), line-of-sight clip, and the fixed yaw held while aiming.
struct PfPlaceOpt{uint8_t half=0,mask=0x3f;bool ray=false,fixedOn=false;float fixedYaw=0;};
struct PfPlaceHit{PfP nb;uint8_t face=0;float hit[3]{};};
// Placement search from an eye point: the cell must be air or liquid; neighbours in the order -y,+y,-z,+z,-x,+x must be solid,
// the click point within reach; with the clip, the ray to the click point pushed 0.01 into the neighbour must hit that neighbour
// on that face (the hit point is then the clip's).
inline bool PfFindPlace(const PfSelf& s,const PfP& p,float reach,const PfPlaceOpt& o,const float* eye,PfPlaceHit* out){
 if(!BlockReplaceable(PfLiveBlock(s,p)))return false;static const int d[6][3]={{0,-1,0},{0,1,0},{0,0,-1},{0,0,1},{-1,0,0},{1,0,0}};
 const float side=o.half==1?0.25f:(o.half==2?0.75f:0.5f);
 for(int i=0;i<6;++i){const int face=i^1;if(!((o.mask>>face)&1))continue;if(o.half==1?i==1:(o.half==2&&i==0))continue;
  const PfP nb=PfAdd(p,d[i][0],d[i][1],d[i][2]);if(!BlockSolid(PfLiveBlock(s,nb)))continue;
  const float hx=d[i][0]*.5f+(p.x+.5f),hz=d[i][2]*.5f+(p.z+.5f),hy=i>=2?side+(float)p.y:d[i][1]*.5f+(p.y+.5f);
  const float dx=hx-eye[0],dy=hy-eye[1],dz=hz-eye[2];if(!(std::sqrt(dz*dz+dy*dy+dx*dx)<=reach))continue;
  out->nb=nb;out->face=(uint8_t)face;out->hit[0]=hx;out->hit[1]=hy;out->hit[2]=hz;if(!o.ray)return true;
  const float to[3]={hx+d[i][0]*.01f,hy+d[i][1]*.01f,hz+d[i][2]*.01f};int32_t got[3]{};uint8_t f=0xff;float h[3]{};
  if(PfClip(s,eye,to,(int)reach+1,got,&f,h)&&f==face&&got[0]==nb.x&&got[1]==nb.y&&got[2]==nb.z){memcpy(out->hit,h,12);return true;}}
 return false;}
// Player box overlapping the cell (a block cannot be placed there).
inline bool PfPlaceOverlaps(const PfSelf& s,const PfP& p){const float fx=(float)p.x,fy=(float)p.y,fz=(float)p.z;const float* bb=s.bb;
 return fx<bb[3]&&fy<bb[4]&&fz<bb[5]&&!(fx+1.f<=bb[0])&&!(fy+1.f<=bb[1])&&bb[2]<fz+1.f;}
// A block to clear is visible: a face whose neighbour is not solid, its point 0.4 out of the centre within reach, and the clip
// from the eye to it hitting this cell.
inline bool PfBreakVisible(const PfSelf& s,const float* eye,const PfP& p,float reach){static const int d[6][3]={{0,-1,0},{0,1,0},{0,0,-1},{0,0,1},{-1,0,0},{1,0,0}};
 for(auto& q:d){if(BlockSolid(PfLiveBlock(s,PfAdd(p,q[0],q[1],q[2]))))continue;const float fp[3]={q[0]*.4f+(p.x+.5f),q[1]*.4f+(p.y+.5f),q[2]*.4f+(p.z+.5f)};
  const float dx=fp[0]-eye[0],dy=fp[1]-eye[1],dz=fp[2]-eye[2];if(!(std::sqrt(dz*dz+dy*dy+dx*dx)<=reach))continue;
  int32_t got[3]{};uint8_t f=0;float h[3]{};if(PfClip(s,eye,fp,(int)reach+1,got,&f,h)&&got[0]==p.x&&got[1]==p.y&&got[2]==p.z)return true;}
 return false;}
// Orientation of a block to place: pillar_axis x / z / y -> faces 0x30 / 0x0c / 0x03; weirdo_direction east / west / south /
// north -> yaw -90 / 90 / 0 / 180; minecraft:cardinal_direction east / west / north / south -> yaw 90 / -90 / 0 / 180 (another
// value: no fixed yaw). A cell whose constrained placement failed is placed without constraint.
inline PfPlaceOpt PfBuildOrient(const Context& c,const PfBuildState& b,const PfBuildTarget& t){PfPlaceOpt o;if(b.orientFailed.count(PfKey(t.p)))return o;
 const uint64_t states=t.block?SusTag(c,t.block+0xf0,"states",10):0;if(!states)return o;std::string v;
 if(const uint64_t g=SusTag(c,states,"pillar_axis",8)){if(SusString(g+8,&v)&&v.size()==1){if(v[0]=='x')o.mask=0x30;else if(v[0]=='z')o.mask=0x0c;else if(v[0]=='y')o.mask=0x03;}}
 if(const uint64_t g=SusTag(c,states,"weirdo_direction",3)){uint32_t w{};if(Read(g+8,&w)&&w<4){static const float yaw[4]={-90.f,90.f,0.f,180.f};o.fixedYaw=yaw[w];o.fixedOn=true;}}
 if(const uint64_t g=SusTag(c,states,"minecraft:cardinal_direction",8)){o.fixedOn=false;if(SusString(g+8,&v)){
   if(v=="east"){o.fixedYaw=90.f;o.fixedOn=true;}else if(v=="west"){o.fixedYaw=-90.f;o.fixedOn=true;}else if(v=="north"){o.fixedYaw=0.f;o.fixedOn=true;}else if(v=="south"){o.fixedYaw=180.f;o.fixedOn=true;}}}
 return o;}
// Within reach from an eye point: the centre within reach, then a visible face (to clear) or a placement found with the clip.
inline bool PfBuildReach(const PfSelf& s,const PfBuildState& b,const PfBuildTarget& t,const float* eye,float reach){
 const float dy=t.p.y+.5f-eye[1],dx=t.p.x+.5f-eye[0],dz=t.p.z+.5f-eye[2];if(!(dz*dz+dy*dy+dx*dx<=reach*reach))return false;
 if(t.occupied)return PfBreakVisible(s,eye,t.p,reach);
 PfPlaceOpt o=PfBuildOrient(s.c,b,t);o.half=t.half;o.ray=true;PfPlaceHit h;return PfFindPlace(s,t.p,reach,o,eye,&h);}
// Can act now: (to fill) the player box is outside the cell, and the cell is within reach from the eye.
inline bool PfBuildCanAct(const PfSelf& s,const PfBuildState& b,const PfBuildTarget& t,float reach){
 if(!t.occupied){const float* bb=s.bb;const float x=(float)t.p.x,y=(float)t.p.y,z=(float)t.p.z;if(bb[0]<x+1.f&&x<bb[3]&&bb[1]<y+1.f&&y<bb[4]&&bb[2]<z+1.f&&z<bb[5])return false;}
 return PfBuildReach(s,b,t,s.eye,reach);}
// Executor place routine with a given block and options: 1 the cell is no longer air / liquid, 0 working, 2 no stack / too many
// tries; the stack is selected in the hotbar first, the aim (look at the click point, fixed yaw when given) must be within 12 deg.
inline int PfPlaceBlock(PfExec& e,const PfSelf& s,const PfP& p,float reach,uint64_t want,const PfPlaceOpt& o){
 if(!e.placing||e.placePos!=p){e.placePos=p;e.placing=true;e.placeTries=0;e.placeCooldown=0;}
 if(!PfReplaceableAt(s,p)){e.placing=false;e.hasLook=false;return 1;}
 if(e.placeCooldown>=1){--e.placeCooldown;return 0;}
 int slot=-1;if(!PfHotbarFind(s,[&](uint64_t st){return PfBuildHolds(st,want);},&slot))return 2;
 int32_t* sel=ActionSelected(s.actor);if(!sel)return 2;if(*sel!=slot){*sel=slot;return 0;}
 int limit=7;PfPlaceHit h;
 if(!PfPlaceOverlaps(s,p)&&PfFindPlace(s,p,reach,o,s.eye,&h)){memcpy(e.look,h.hit,12);e.hasLook=true;e.fixedYaw=o.fixedYaw;e.fixedOn=o.fixedOn;float yaw,pitch;PfLookAngles(e,s,&yaw,&pitch);
  if(!PfWithin(s,yaw,pitch,e.tolerance))return 0;const float rel[3]={h.hit[0]-(float)h.nb.x,h.hit[1]-(float)h.nb.y,h.hit[2]-(float)h.nb.z};
  PfUseOn(s,h.nb,h.face,rel);ActionSwing(s.c,s.actor,1);e.placeCooldown=3;limit=11;}
 const int tries=e.placeTries++;return limit<tries?2:0;}
// Inventory -> hotbar: the first main-inventory stack of the block goes to hotbar slot 8 through the inventory scheduler.
inline constexpr int kInvPathFinder=25;   // inventory scheduler key
inline bool PfInvPending(){auto& q=Inventory();std::lock_guard<std::recursive_mutex> l(q.guard);return q.queue.count(kInvPathFinder)!=0;}
inline void PfBuildRequestMove(uint64_t block){InvSubmit(kInvPathFinder,[block](uintptr_t actor){Context c{};if(!Local(&c)||c.actor!=actor)return;const uint64_t box=ActionContainer(actor);if(!box||ActionCursorBusy(c,actor))return;
  const int n=ActionSize(c,box);for(int i=9;i<n;++i){const uint64_t st=ActionStackAt(c,box,i);if(!PfBuildHolds(st,block))continue;uint8_t cnt{};Read(st+0x22,&cnt);InvMoveStack(c,actor,0xc,i,0xc,8,cnt);return;}});}
// Material: 0 in the hotbar, 1 wait (a move is pending, the cursor is busy or less than 1000 ms since the last move), 2 none
// (not in the inventory, or three moves of the same block did not bring it to the hotbar).
inline int PfBuildSelect(PfBuildState& b,const PfSelf& s,uint64_t block){
 int slot=-1;if(PfHotbarFind(s,[&](uint64_t st){return PfBuildHolds(st,block);},&slot)){if(b.moveBlock==block)b.moveCount=0;return 0;}
 const uint64_t box=ActionContainer(s.actor);if(!box)return 2;
 if(!b.moveTimer||ActionMs(b.moveNs)<=1000.0)return 1;if(ActionCursorBusy(s.c,s.actor)||PfInvPending())return 1;
 if(b.moveBlock==block){if(b.moveCount>2)return 2;}else{b.moveBlock=block;b.moveCount=0;}
 const int n=ActionSize(s.c,box);for(int i=9;i<n;++i)if(PfBuildHolds(ActionStackAt(s.c,box,i),block)){++b.moveCount;b.moveNs=PfNow();b.moveTimer=true;PfBuildRequestMove(block);return 1;}
 return 2;}
// Target choice (index or -1): cells remembered as failed are skipped; a cell to fill without a stack anywhere in the inventory
// counts its block as missing; one without a solid neighbour is only a last resort; the first cell that can be acted on now
// wins, else the first other one, else the first without support.
inline int PfBuildChoose(PfBuildState& b,const PfSelf& s,float reach){static const int d[6][3]={{0,-1,0},{0,1,0},{0,0,-1},{0,0,1},{-1,0,0},{1,0,0}};
 int walk=-1,loose=-1;const uint64_t box=ActionContainer(s.actor);const int n=box?ActionSize(s.c,box):0;
 for(size_t i=0;i<b.targets.size();++i){const PfBuildTarget& t=b.targets[i];auto f=b.failed.find(PfKey(t.p));if(f!=b.failed.end()&&b.tick<f->second)continue;
  if(!t.occupied){bool have=false;for(int j=0;j<n&&!have;++j)have=PfBuildHolds(ActionStackAt(s.c,box,j),t.block);if(!have){PfBuildNeed(b,t.block);continue;}
   bool support=false;for(auto& q:d)if(BlockSolid(PfLiveBlock(s,PfAdd(t.p,q[0],q[1],q[2])))){support=true;break;}
   if(!support){if(loose<0)loose=(int)i;continue;}}
  if(PfBuildCanAct(s,b,t,reach))return (int)i;if(walk<0)walk=(int)i;}
 return walk>=0?walk:loose;}
// Stand-cell goal for a cell out of reach (kept while it is for the same cell; no path or arrived on it -> none): cells within
// ceil(reach) (one more below) with two passable non-liquid cells, not the cell's own column at its height, a solid or passable
// floor, eye (x + 0.5, y + 1.62, z + 0.5) within reach + 1 of the centre and accepted; solid floors first.
inline bool PfBuildGoal(PfController& k,const PfP& p,float reach,const std::function<bool(const float*)>& accept){auto& b=k.build;
 if(b.goalOn&&b.goalFor==p){if(k.noPath||k.arrived){b.goalOn=false;return false;}return true;}
 const int R=(int)std::ceil(reach);k.world.cells.clear();std::unordered_set<uint64_t> stand,other;
 for(int dy=-R-1;dy<=R;++dy)for(int dz=-R;dz<=R;++dz)for(int dx=-R;dx<=R;++dx){const PfP c=PfAdd(p,dx,dy,dz);
  const PfRec r0=PfRecAt(k.world,c),r1=PfRecAt(k.world,PfAdd(c,0,1,0));if(!(r0.passable==1&&r1.passable==1&&r0.cls==0&&r1.cls==0))continue;
  if(c.x==p.x&&c.z==p.z&&(c.y==p.y||c.y+1==p.y))continue;
  const PfRec rb=PfRecAt(k.world,PfAdd(c,0,-1,0));if(!(rb.solid||rb.passable==1))continue;
  const float eye[3]={c.x+.5f,c.y+1.62f,c.z+.5f};const float ex=p.x+.5f-eye[0],ey=p.y+.5f-eye[1],ez=p.z+.5f-eye[2];
  if(!(std::sqrt(ez*ez+ey*ey+ex*ex)<=reach+1.f)||!accept(eye))continue;(rb.solid?stand:other).insert(PfKey(c));}
 auto& pick=stand.empty()?other:stand;if(pick.empty()){b.goalOn=false;return false;}
 b.goalFor=p;b.goal=PfGoal();b.goal.kind=7;b.goal.p=p;b.goal.cells=std::shared_ptr<const std::unordered_set<uint64_t>>(std::make_shared<std::unordered_set<uint64_t>>(std::move(pick)));
 b.goalId=k.nextId++;b.goalOn=true;return true;}
// Acting on the current cell: true when it gives this tick's output. To fill: material, then the place routine; to clear: break
// (Allow Break), a wrong block is then filled. Out of reach -> placing stops; failures are remembered for 400 ticks.
inline bool PfBuildExec(PfController& k,const PfSelf& s,const PfConfig& cfg,float reach,PfProcOut& o){auto& b=k.build;const PfBuildTarget t=b.cur;const uint64_t key=PfKey(t.p);
 if(!t.occupied&&PfReplaceableAt(s,t.p)&&!PfBuildCanAct(s,b,t,reach)){b.placing=false;return false;}
 if(t.occupied){
  if(cfg.allowBreak){const int r=PfBreak(k.exec,s,t.p,reach);
   if(r!=2){if(r==0){PfBuildStatus(b,o);return true;}if(t.block){b.cur.occupied=false;PfBuildStatus(b,o);return true;}PfBuildRemove(b,t.p);return false;}}}
 else{const int r=PfBuildSelect(b,s,t.block);
  if(r!=2){if(r==1){PfBuildStatus(b,o);return true;}
   PfPlaceOpt opt=PfBuildOrient(s.c,b,t);opt.half=t.half;opt.ray=true;const int placed=PfPlaceBlock(k.exec,s,t.p,reach,t.block,opt);
   if(placed==1){PfBuildRemove(b,t.p);return false;}
   if(placed==2){if(opt.mask==0x3f&&!opt.fixedOn){b.failed[key]=b.tick+400;b.placing=false;return false;}b.orientFailed.insert(key);PfResetInteraction(k.exec,&s);}
   PfBuildStatus(b,o);return true;}
  PfBuildNeed(b,t.block);}
 b.failed[key]=b.tick+400;b.placing=false;return false;}
// Scaffolding removal: the block being broken first; otherwise the highest one (then the nearest to the player cell) is broken
// in place when its centre is within reach and a face is visible, else walked to (no stand cell -> forgotten).
inline void PfBuildScaffold(PfController& k,const PfSelf& s,const PfP& cell,float reach,PfProcOut& o){auto& b=k.build;
 o.brk=false;o.place=true;o.status="Removing scaffolding ("+std::to_string(b.scaffold.size())+" left)";
 if(b.scBreaking){const int r=PfBreak(k.exec,s,b.scAt,reach);if(r==0)return;PfBuildScaffoldDrop(b,b.scAt);b.scBreaking=false;return;}
 auto dist=[&](const PfP& q){const int a=q.x-cell.x,h=q.y-cell.y,c2=q.z-cell.z;return (uint32_t)(a*a+h*h+c2*c2);};size_t best=0;
 for(size_t i=1;i<b.scaffold.size();++i){const PfP& c=b.scaffold[i];const PfP& m=b.scaffold[best];if(c.y==m.y){if(dist(c)<dist(m))best=i;}else if(c.y>m.y)best=i;}
 const PfP t=b.scaffold[best];const float dx=t.x+.5f-s.eye[0],dy=t.y+.5f-s.eye[1],dz=t.z+.5f-s.eye[2];
 if(reach*reach<dz*dz+dy*dy+dx*dx||!PfBreakVisible(s,s.eye,t,reach)){
  auto ok=[&](const float* eye){const float ex=t.x+.5f-eye[0],ey=t.y+.5f-eye[1],ez=t.z+.5f-eye[2];return ez*ez+ey*ey+ex*ex<=reach*reach&&PfBreakVisible(s,eye,t,reach);};
  if(!PfBuildGoal(k,t,reach,ok)){PfBuildScaffoldDrop(b,t);return;}
  o.hasGoal=true;o.goal=b.goal;o.goalId=b.goalId;return;}
 b.scAt=t;b.scBreaking=true;b.goalOn=false;}
inline void PfBuildTick(PfController& k,const PfSelf& s,const PfConfig& cfg,const PfP& cell,float reach,PfProcOut& o){auto& b=k.build;
 if(!PfSchemShown()){o.result=2;o.message="Load and show a schematic before building.";return;}
 ++b.tick;
 {std::vector<PfP> placed;placed.swap(k.exec.placed);for(auto& p:placed)if(b.scaffoldSet.insert(PfKey(p)).second)b.scaffold.push_back(p);}   // support blocks placed by the executor
 if(--b.refresh<=0){b.refresh=20;PfSchemMissing(s,cell,256,b.targets);
  for(auto it=b.scaffold.begin();it!=b.scaffold.end();){const uint64_t have=PfLiveBlock(s,*it);if(BlockReplaceable(have)||PfSchemMatch(*it,have)){b.scaffoldSet.erase(PfKey(*it));it=b.scaffold.erase(it);}else ++it;}
  b.targets.erase(std::remove_if(b.targets.begin(),b.targets.end(),[&](const PfBuildTarget& t){return b.scaffoldSet.count(PfKey(t.p))!=0;}),b.targets.end());
  b.missing.clear();}
 if(b.placing&&PfBuildExec(k,s,cfg,reach,o))return;
 if(b.targets.empty()){if(!b.scaffold.empty()){PfBuildScaffold(k,s,cell,reach,o);return;}o.result=1;o.message="Schematic complete ("+std::to_string(b.placed)+" block(s) placed).";return;}
 const int pick=PfBuildChoose(b,s,reach);
 if(pick<0){o.result=2;o.message="Missing materials: "+PfBuildNeedText(b);return;}
 const PfBuildTarget t=b.targets[(size_t)pick];
 if(!PfBuildCanAct(s,b,t,reach)){
  if(!PfBuildGoal(k,t.p,reach,[&](const float* eye){return PfBuildReach(s,b,t,eye,reach);})){b.failed[PfKey(t.p)]=b.tick+400;b.placing=false;PfBuildStatus(b,o);return;}
  o.hasGoal=true;o.goal=b.goal;o.goalId=b.goalId;PfBuildStatus(b,o);return;}
 b.cur=t;b.placing=true;if(!PfBuildExec(k,s,cfg,reach,o))PfBuildStatus(b,o);}
inline void PfProcessTick(PfController& k,const PfSelf& s,const PfConfig& cfg,const PfP& cell,float reach,PfProcOut& o){
 switch(k.proc){case kPfGoto:PfGotoTick(k,s,cell,o);break;case kPfWander:PfWanderTick(k,cell,o);break;case kPfFollow:PfFollowTick(k,s,cell,o);break;
  case kPfMine:case kPfChop:PfMineTick(k,s,cfg,cell,reach,o);break;case kPfTunnel:PfTunnelTick(k,cell,o);break;case kPfBuild:PfBuildTick(k,s,cfg,cell,reach,o);break;default:o.result=1;break;}}
// Auto Eat: on the ground, not breaking, hunger <= 14, after a 1000 ms cooldown: select a food item from the hotbar (not golden
// apples, rotten flesh, spider eye, poisonous potato, pufferfish, chorus fruit, suspicious stew) and start using it; eating
// lasts 1750 ms.
inline bool PfEdible(const Context& c,uint64_t stack){const uint64_t item=ActionItem(stack);if(!item)return false;const uint64_t f=Virtual0(c,item,0x98);if(!f||!reinterpret_cast<bool(*)(uint64_t)>(f)(item))return false;
 std::string n;if(!ActionName(stack,&n))return false;if(n.compare(0,10,"minecraft:")==0)n.erase(0,10);
 static const char* no[]={"golden_apple","enchanted_golden_apple","rotten_flesh","spider_eye","poisonous_potato","pufferfish","chorus_fruit","suspicious_stew"};for(auto t:no)if(n==t)return false;return true;}
// ItemReleaseInventoryTransaction (0xe0 bytes, game vtable RVA 0xeffad60, type 4): action 1 (release), selected slot, item
// descriptor of the held stack, player position; sent through actor->vtable[0x718] like the use transaction.
inline void PfReleaseItem(const Context& c,uintptr_t actor,uint64_t stack){int32_t* sel=ActionSelected(actor);float pos[3]{};if(!sel||!ActionPosition(actor,pos))return;
 uint8_t* t=(uint8_t*)::operator new(0xe0);memset(t,0,0xe0);uint8_t* inv=t+0x10;*(float*)inv=1.0f;
 uint64_t* head=(uint64_t*)::operator new(0x38);head[0]=head[1]=(uint64_t)head;*(uint64_t*)(inv+8)=(uint64_t)head;*(uint64_t*)(inv+0x10)=0;
 uint64_t* buckets=(uint64_t*)::operator new(0x80);for(int i=0;i<16;++i)buckets[i]=(uint64_t)head;
 *(uint64_t*)(inv+0x18)=(uint64_t)buckets;*(uint64_t*)(inv+0x20)=(uint64_t)(buckets+16);*(uint64_t*)(inv+0x28)=(uint64_t)(buckets+16);*(uint64_t*)(inv+0x30)=7;*(uint64_t*)(inv+0x38)=8;
 DescriptorBuild(c,(uint64_t)(t+0x70),stack);*(uint64_t*)t=c.base+Rva(0xeffad60);*(int32_t*)(t+8)=4;*(int32_t*)(t+0x68)=1;*(int32_t*)(t+0x6c)=*sel;memcpy(t+0xd0,pos,12);
 SendUseTransaction(c,actor,t);}
inline bool PfAutoEat(PfController& k,const PfSelf& s,const PfConfig& cfg){
 if(k.eating){if(!k.eatTimer)return true;if(ActionMs(k.eatNs)<=1750.0)return true;
  {const uint64_t held=ActionHeld(s.c,s.actor);if(PfEdible(s.c,held))PfReleaseItem(s.c,s.actor,held);}   // meal over: release a food item still in use
  k.eating=false;k.eatCool=true;k.eatCoolNs=PfNow();return false;}
 if(k.eatCool&&ActionMs(k.eatCoolNs)<=1000.0)return false;if(!cfg.autoEat||k.exec.breaking)return false;
 if(!s.onGround||k.exec.waterPending)return false;float hunger{},mx{};if(!Attribute(s.c.registry,s.c.entity,1,&hunger,&mx)||14.f<hunger)return false;
 int slot{};if(!PfHotbarFind(s,[&](uint64_t st){return PfEdible(s.c,st);},&slot))return false;int32_t* sel=ActionSelected(s.actor);if(!sel)return false;
 if(*sel==slot){UseItem(s.c,s.actor,ActionHeld(s.c,s.actor),false);k.eatNs=PfNow();k.eatTimer=true;k.eating=true;}else *sel=slot;return true;}
inline void PfStartProcess(PfController& k,const PfSelf& s,int proc){PfReset(k,&s);k.proc=proc;(void)s;}
inline void PfApply(PfController& k,const PfSelf& s,const PfConfig& cfg,const PfCommandRequest& c){
 if(c.kind==7){PfReset(k,&s);PfMessage("Stopped pathing.");return;}if(c.kind==8){k.userPaused=true;PfMessage("Paused pathing.");return;}if(c.kind==9){k.userPaused=false;PfMessage("Resumed pathing.");return;}
 if(c.kind==10){if(k.hasGoal){k.planAt=0;PfReplan(k,s,cfg);}return;}
 if(c.kind==11&&!k.deathKnown){PfMessage("No death point recorded yet.");return;}
 k.userPaused=false;
 if(c.kind==1||c.kind==11||c.kind==12){PfStartProcess(k,s,kPfGoto);auto& g=k.gotoS;g=PfGotoState();g.goalId=k.nextId++;
  if(c.kind==11){g.goal.kind=1;g.goal.p=k.death;g.goal.radius=1;g.label="your last death";}
  else if(c.kind==12){g.goal.kind=6;g.goal.hasY=false;g.label="the nearest axis";}
  else{g.goal.kind=c.count;g.goal.p=PfP{c.pos[0],c.pos[1],c.pos[2]};g.goal.radius=std::max(0,c.radius);if(g.goal.kind==0&&c.radius>0)g.goal.kind=1;g.label=c.name;
   if(g.label.empty())g.label=c.count==2?std::to_string(c.pos[0])+" "+std::to_string(c.pos[2]):(c.count==3?std::to_string(c.pos[1]):std::to_string(c.pos[0])+" "+std::to_string(c.pos[1])+" "+std::to_string(c.pos[2]));
   const std::string note=PfSnapGoal(k,s,cfg,g.goal);
   if(g.goal.kind==0){PfView v;v.w=&k.world;PfBuildOpt(k,s,cfg,v.o);   // world cache prepared by PfSnapGoal
    const std::string why=PfGoalProblem(v,g.goal.p);if(!why.empty()){PfReset(k,&s);PfMessage("Cannot reach "+g.label+": "+why+".");return;}}
   if(!note.empty()){PfMessage(note+".");if(c.name.empty())g.label=PfCellText(g.goal.p);}}
  PfMessage("Pathing to "+g.label+".");}
 else if(c.kind==2){PfStartProcess(k,s,kPfFollow);auto& f=k.follow;f=PfFollowState();f.name=c.name;f.distance=c.radius>0?(float)c.radius:std::max(1.f,cfg.followDistance);f.radius=(int)std::ceil(f.distance);f.goalNs=PfNow();}
 else if(c.kind==3||c.kind==6){PfStartProcess(k,s,c.kind==3?kPfMine:kPfChop);auto& m=k.mine;m=PfMineState();k.junkNs=PfNow();k.junkTimer=true;m.count=c.count;m.radius=std::max(2,cfg.cacheRadius)*16;
  if(c.kind==6){m.patterns={"*_log","crimson_stem","warped_stem","stripped_crimson_stem","stripped_warped_stem"};m.label="Chop";PfMessage(c.count>0?"Chopping "+std::to_string(c.count)+" logs.":"Chopping trees.");}
  else{m.patterns=c.blocks.empty()?cfg.mineBlocks:c.blocks;m.label="Mine";if(m.patterns.empty()){PfReset(k,&s);PfMessage("Name at least one block to mine.");return;}
   std::string what;if(c.blocks.empty())what="the configured blocks";else for(size_t i=0;i<c.blocks.size();++i){if(i)what+=", ";what+=c.blocks[i];}
   PfMessage(c.count>0?"Mining "+std::to_string(c.count)+" of "+what+".":"Mining "+what+".");}}
 else if(c.kind==4){PfStartProcess(k,s,kPfWander);k.wander=PfWanderState();k.wander.radius=c.radius>0?c.radius:cfg.wanderRadius;}
 else if(c.kind==5){PfStartProcess(k,s,kPfTunnel);auto& t=k.tunnel;t=PfTunnelState();static const int dx[4]={0,-1,0,1},dz[4]={1,0,-1,0};const int q=((int)std::lround(s.yaw/90.f))&3;t.dx=dx[q];t.dz=dz[q];t.length=c.count;
  PfMessage(c.count>0?"Tunnelling "+std::to_string(c.count)+" blocks.":"Tunnelling.");}
 else if(c.kind==13){PfStartProcess(k,s,kPfBuild);k.build=PfBuildState();k.build.moveNs=PfNow();k.build.moveTimer=true;PfMessage("Building the shown schematic.");}}   // the move timer starts with the process
// Waypoint entry "name x y z [dimension] [#color]" -> position.
inline bool PfWaypoint(const std::vector<std::string>& entries,const std::string& want,int* pos){const std::string w=PfLower(want);
 for(auto& e:entries){std::vector<std::string> t;{std::string cur;for(char ch:e){if(ch==' '||ch==(char)9){if(!cur.empty())t.push_back(cur);cur.clear();}else cur+=ch;}if(!cur.empty())t.push_back(cur);}
  while(!t.empty()&&t.back()[0]=='#')t.pop_back();if(!t.empty()){const std::string d=PfLower(t.back());if(d=="overworld"||d=="nether"||d=="end"||d=="the_end")t.pop_back();}
  if(t.size()<4)continue;std::string name;for(size_t i=0;i+3<t.size();++i){if(i)name+=' ';name+=t[i];}if(PfLower(name)!=w)continue;
  for(int i=0;i<3;++i)pos[i]=atoi(t[t.size()-3+i].c_str());return true;}return false;}
// Last death: the health attribute reaching 0 (once per death) records the start cell, whatever the process.
inline void PfDeathWatch(PfController& k,const PfSelf& s){float cur{},mx{};if(!Attribute(s.c.registry,s.c.entity,6,&cur,&mx))return;const bool dead=cur<=0.f;if(dead&&!k.wasDead){k.death=PfStartCell(k,s);k.deathKnown=true;}k.wasDead=dead;}

// ---- publication, rotation, tick
inline void PfRotate(const PfConfig& cfg,const PfOut& out){if(!out.hasRot){RotRelease(kRotPathFinder);return;}
 RotRequest r;r.module=kRotPathFinder;r.yaw=out.yaw;r.pitch=out.pitch;r.priority=3;r.mode=cfg.silentRotations?2:1;const float sm=cfg.smoothing/100.f;
 if(cfg.silentRotations||sm<=0.f||out.instant){RotSubmit(std::move(r));return;}
 float speed=3.f;if(std::isfinite(sm)){if(sm!=1.f){const float v=fmaf(sm,-27.f,30.f);speed=1.f<sm?(v<=3.f?v:3.f):(3.f<=v?v:3.f);}}else speed=sm*-27.f;
 r.speed=speed;RotSubmitPersistent(std::move(r),0);}
inline void PfPublish(PfController& k,const PfSelf& s,const PfConfig& cfg){auto& sh=PathFinder();const PfExec& e=k.exec;
 sh.yaw=e.yaw;sh.mvX=e.mv[0];sh.mvZ=e.mv[1];sh.sprint=e.sprint;sh.jump=e.jump;sh.sneak=e.sneak;sh.holdJump=e.holdJump;if(!e.active)sh.lastJump=false;sh.active=e.active;
 std::lock_guard<std::mutex> l(sh.guard);sh.state=k.state;sh.process=k.proc;sh.drawCfg=cfg;
 sh.remaining=(int)k.path.size();sh.detail=k.status;
 sh.pathPoints.clear();if(cfg.renderPath&&!k.path.empty()){sh.pathPoints.push_back({s.pos[0],s.pos[1]+.1f,s.pos[2]});for(auto& st:k.path)sh.pathPoints.push_back({st.to.x+.5f,st.to.y+.1f,st.to.z+.5f});}
 if(k.search&&cfg.showThinking)sh.thinkPoints=k.search->sample;else if(!cfg.showThinking)sh.thinkPoints.clear();
 sh.hasGoal=k.hasGoal;if(k.hasGoal){const PfP& p=k.goal.p;sh.goalBox[0]=(float)p.x;sh.goalBox[1]=k.goal.kind==2?(float)s.cell.y:(float)p.y;sh.goalBox[2]=(float)p.z;sh.goalBox[3]=p.x+1.f;sh.goalBox[4]=sh.goalBox[1]+(k.goal.kind==2?3.f:1.f);sh.goalBox[5]=p.z+1.f;}}
inline float PfReach(const PfSelf& s){float r=AbrReach(s.c,s.actor);if(!(r>0.f)||r>4.5f)r=4.5f;return r;}
inline void PfTickWork(){
 auto& sh=PathFinder();auto& k=PfCtl();const bool on=sh.enabled.load();static bool was=false;Rotations().enabled[kRotPathFinder]=on;
 if(!on){if(was){PfSelf s;const bool have=PfReadSelf(s);PfReset(k,have?&s:nullptr);was=false;}return;}
 if(!was){k.planAt=PfNow();k.stuck.clear();k.spawners.clear();k.tick=0;k.eatCool=true;k.eatCoolNs=PfNow();}was=true;   // a fresh controller: replan and eat timers start now
 PfConfig cfg;std::deque<PfCommandRequest> cmds;{std::lock_guard<std::mutex> l(sh.guard);cfg=sh.cfg;cmds.swap(sh.queue);}sh.pauseOnInput=cfg.pauseOnInput;
 PfSelf s;if(!PfReadSelf(s)){PfReset(k,nullptr);return;}
 // A new local player or client level (leaving a world, joining or switching servers) ends the running process: the
 // cached world, its game mode and its block pointers belong to the old level, and the game's destroy-rate call on a stale
 // game mode jumped through a null pointer (an uncatchable Control Flow Guard fail-fast, crash of 14/11/2026).
 uint64_t level{},gm{};Read(s.actor+0x200,&level);Read(s.actor+0xac8,&gm);
 const bool moved=k.world.region&&(k.world.actor!=s.actor||k.world.level!=level);
 if(moved&&k.proc!=kPfNone){PfReset(k,nullptr);PfMessage("Stopped pathing: the world changed.");}
 // A new dimension (portal, teleport) also ends it: goals and paths are coordinates of the dimension they were set in,
 // and the same numbers in another one lead somewhere else (the Nether is 8 times smaller), often into lava.
 int dimension=-1;ReadDimension(&dimension);
 if(!moved&&dimension>=0&&k.dimension>=0&&dimension!=k.dimension&&k.proc!=kPfNone){PfReset(k,&s);PfMessage("Stopped pathing: the dimension changed.");}
 if(dimension>=0)k.dimension=dimension;
 if(moved||k.world.region!=s.region||!k.world.region||k.world.gm!=gm)PfWorldBegin(k.world,s.c,cfg,s.cell.x,s.cell.z);
 ++k.tick;PfDeathWatch(k,s);
 for(auto& c:cmds)PfApply(k,s,cfg,c);
 PfOut out;
 if(k.proc==kPfNone){PfExecClear(k.exec,&s);k.state=0;PfPublish(k,s,cfg);PfRotate(cfg,out);return;}
 PfAvoidUpdate(k,s,cfg);
 bool paused=k.userPaused;if(sh.keyPause.load()){if(ActionMs(sh.keyNs.load())>1500.0)sh.keyPause=false;else paused=true;}
 if(paused){PfExecClear(k.exec,&s);k.state=4;PfPublish(k,s,cfg);PfRotate(cfg,out);return;}
 if(k.search)PfSearchTick(k,s);
 k.exec.minDurability=cfg.saveTools?10:0;
 if(k.exec.toolWorn){k.exec.toolWorn=false;PfReset(k,&s);PfMessage("Stopped so your tool does not break.");PfPublish(k,s,cfg);PfRotate(cfg,out);return;}
 if(PfAutoEat(k,s,cfg)){PfIdle(k.exec,s,out);k.state=3;PfPublish(k,s,cfg);PfRotate(cfg,out);return;}
 const float reach=PfReach(s);const PfP cell=PfStartCell(k,s);PfProcOut po;PfProcessTick(k,s,cfg,cell,reach,po);k.noPath=false;k.unreachable=false;k.giveUp=false;
 if(po.result!=0){   // finished or failed: an open water pick-up completes first
  if(k.exec.waterPending){PfOut w;if(PfPickup(k.exec,s,w)){k.state=3;PfPublish(k,s,cfg);PfRotate(cfg,w);return;}}
  PfReset(k,&s);if(!po.message.empty())PfMessage(po.message);PfPublish(k,s,cfg);PfRotate(cfg,out);return;}
 k.status=po.status;k.procBreak=po.brk;k.procPlace=po.place;
 if(!po.hasGoal){k.hasGoal=false;PfIdle(k.exec,s,out);k.state=3;}
 else{
  if(!k.hasGoal||po.goalId!=k.goalId){k.goal=po.goal;k.goalId=po.goalId;k.hasGoal=true;k.arrived=false;k.search.reset();PfProgressReset(k);PfStartSearch(k,s,cfg,po.retarget&&k.hasPath);}
  else k.goal=po.goal;
  if(k.hasPath){out=PfExecute(k,s,cfg,reach);
   switch(out.status){case 1:k.arrived=true;break;case 2:case 4:if(!k.path.empty())PfStuckAdd(k,k.path[0].to);PfReplan(k,s,cfg);break;case 3:PfReplan(k,s,cfg);break;default:break;}
   if(out.status!=1)PfWatch(k,s,cfg,cell,out);
   if(k.contPending&&!k.search&&k.hasPath&&!k.path.empty()&&ActionMs(k.contNs)>1500.0){k.contPending=false;const PfP end=k.path.back().to;PfStartSearch(k,s,cfg,true,&end);}
   if(k.tick%10==0&&!k.search&&k.hasPath&&!PfPathValid(k))PfReplan(k,s,cfg);
   if(k.hasPath&&k.path.empty()&&!k.pathComplete&&!k.search)PfReplan(k,s,cfg);
   k.state=2;}
  else{PfExecClear(k.exec,&s);if(!k.search&&k.retry&&ActionMs(k.retryNs)>1000.0){k.retry=false;PfStartSearch(k,s,cfg,false);}k.state=k.search?1:2;}}
 PfPublish(k,s,cfg);PfRotate(cfg,out);}
inline void PfSayStopped(){if(auto f=pfSay)f("Path Finder stopped after an error; it works again in the next world you join.");}   // outside the __try function (no unwinding there)
inline void PfTick(){auto& sh=PathFinder();if(game::FeatureStopped(&sh.fault))return;__try{PfTickWork();}__except(game::FeatureFault("Path Finder",&sh.fault,GetExceptionInformation())){game::FeatureFlush();PfReleaseInputs(sh);PfSayStopped();}}
// SprintIntent: Pause On Input sampling, then the movement keys: the move vector goes from the steering yaw to the world and
// back into the head yaw frame; bits 13..16 at +-0.3, sprint bit 8 / flag 2 when 0.9 x length <= forward, jump bits 7 / 26
// (and 25 on the first frame) with flag 0x10 unless the jump goes through the jump hook, sneak bits 0 / 21 and flag 1.
inline void PfSprintWork(uint64_t input,uint64_t state,uint64_t movement){auto& sh=PathFinder();if(!sh.enabled.load()||!input)return;Context c{};uint64_t mine{};if(!Local(&c)||!Read(c.actor+0x240,&mine)||mine!=state)return;
 if(PfCtl().proc==kPfNone)return;
 uint32_t& bits=*(uint32_t*)input;
 // Pause On Input reads the keys the player holds (the word at +0x10), never the effective word at +0: that one already
 // carries the jump this path finder requested through the jump hook (JumpInput runs first) and the bits other modules
 // write, and reading it made every ascend / parkour jump pause the path finder for 1.5 s in a loop.
 uint32_t held{};if(sh.pauseOnInput.load()&&Read(input+0x10,&held)&&(held&((1u<<13)|(1u<<14)|(1u<<15)|(1u<<16)|0x80u|1u))){sh.keyNs=ActionNs();sh.keyPause=true;}
 if(sh.keyPause.load()||!sh.active.load())return;
 float head{},pitch{},yaw{};if(!RotRead(c.actor,&pitch,&yaw))return;head=yaw;uintptr_t hr{};float hv{};if(Element(c.registry,c.entity,"ActorHeadRotationComponent",8,&hr)&&Read(hr,&hv)&&std::isfinite(hv))head=hv;
 float wx,wz;PfToWorld(sh.mvX.load(),sh.mvZ.load(),sh.yaw.load(),&wx,&wz);float v[2];PfToLocal(wx,wz,head,&v[0],&v[1]);
 const float len2=v[1]*v[1]+v[0]*v[0];const bool sprint=0.f<len2&&sh.sprint.load()&&std::sqrt(len2)*0.8999999761581421f<=v[1];
 const bool hold=sh.holdJump.load(),jump=!hold&&sh.jump.load(),sneak=sh.sneak.load();
 const uint32_t dir=(v[0]<-0.30000001192092896f?1u<<16:0u)|(0.30000001192092896f<v[0]?1u<<15:0u)|(0.30000001192092896f<v[1]?1u<<13:0u)|(v[1]<-0.30000001192092896f?1u<<14:0u);
 uint32_t out=dir|(bits&0xfffe1fffu);uint8_t& flags=*(uint8_t*)(input+0x60);uint8_t f=flags;
 if(!hold){out=(jump&&!sh.lastJump.load()?1u<<25:0u)|(jump?0x4000080u:0u)|dir|(bits&0xf9fe1f7fu);f=(uint8_t)((f&0xef)|(jump?0x10:0));}
 out=(sprint?1u<<8:0u)|(out&0xfffffeffu);out=(out&0xffdffefeu)|(sneak?1u:0u)|(sneak?1u<<21:0u);bits=out;
 memcpy((void*)(input+4),v,8);memcpy((void*)(input+0x24),v,8);flags=(uint8_t)((f&0xfc)|(sprint?2:0)|(sneak?1:0));if(movement)memcpy((void*)movement,v,8);sh.lastJump=jump;}
inline void PfSprint(uint64_t input,uint64_t state,uint64_t movement){auto& sh=PathFinder();if(game::FeatureStopped(&sh.fault))return;__try{PfSprintWork(input,state,movement);}__except(game::FeatureFault("Path Finder (input)",&sh.fault,GetExceptionInformation())){game::FeatureFlush();}}

// ---- drawing (ScreenViewRender thread): appends world lines and the goal box
// Path: a flat ribbon 0.16 wide just above the walking surface with a thin centre line, both fading towards the end of
// the path; goal: a frame on its floor, a faint fill and a vertical beam fading upwards (taller for a column goal);
// thinking: short faint ticks. Colours come from the settings (Nickel orange by default).
inline void PfDraw(std::vector<WorldLine>& lines,std::vector<WorldBox>& boxes,std::vector<WorldQuad>& quads){auto& sh=PathFinder();std::lock_guard<std::mutex> l(sh.guard);if(!sh.enabled.load())return;
 const PfConfig& c=sh.drawCfg;const bool depth=!c.throughWalls;(void)boxes;
 auto tint=[](const float* base,float a,float* out){out[0]=base[0];out[1]=base[1];out[2]=base[2];out[3]=base[3]*a;};
 auto line=[&](const float* a,const float* b,const float* colA,const float* colB,float th){WorldLine w{};memcpy(w.a,a,12);memcpy(w.b,b,12);memcpy(w.colorA,colA,16);memcpy(w.colorB,colB,16);w.thickness=th;w.depth=depth;lines.push_back(w);};
 auto quad=[&](const float p[4][3],const float* col){WorldQuad q{};memcpy(q.p,p,sizeof(q.p));memcpy(q.color,col,16);q.depth=depth;quads.push_back(q);};
 const size_t n=sh.pathPoints.size();
 for(size_t i=1;i<n;++i){const auto& a=sh.pathPoints[i-1];const auto& b=sh.pathPoints[i];const float fa=1.f-.6f*(float)(i-1)/(float)(n-1),fb=1.f-.6f*(float)i/(float)(n-1);
  const float A[3]={a[0],a[1]-.07f,a[2]},B[3]={b[0],b[1]-.07f,b[2]};float dx=B[0]-A[0],dz=B[2]-A[2];const float len=std::sqrt(dx*dx+dz*dz);float px=.08f,pz=0;if(len>.001f){px=-dz/len*.08f;pz=dx/len*.08f;}
  const float P[4][3]={{A[0]+px,A[1],A[2]+pz},{B[0]+px,B[1],B[2]+pz},{B[0]-px,B[1],B[2]-pz},{A[0]-px,A[1],A[2]-pz}};float fill[4];tint(c.pathColor,.35f*(fa+fb)*.5f,fill);quad(P,fill);
  float ca[4],cb[4];tint(c.pathColor,fa,ca);tint(c.pathColor,fb,cb);const float lA[3]={A[0],A[1]+.01f,A[2]},lB[3]={B[0],B[1]+.01f,B[2]};line(lA,lB,ca,cb,2.f);}
 if(c.showThinking){float tc[4];tint(c.thinkingColor,.6f,tc);for(auto& p:sh.thinkPoints){const float a[3]={p[0],p[1]-.05f,p[2]},b[3]={p[0],p[1]+.15f,p[2]};line(a,b,tc,tc,1.f);}}
 if(sh.hasGoal){const float* g=sh.goalBox;const float x0=g[0]+.06f,x1=g[3]-.06f,z0=g[2]+.06f,z1=g[5]-.06f,y=g[1]+.02f,cx=(g[0]+g[3])*.5f,cz=(g[2]+g[5])*.5f,tall=g[4]-g[1]>1.5f?6.f:2.5f;
  float edge[4],fill[4],beamA[4],beamB[4];tint(c.goalColor,1.f,edge);tint(c.goalColor,.18f,fill);tint(c.goalColor,.85f,beamA);tint(c.goalColor,0.f,beamB);
  const float F[4][3]={{x0,y,z0},{x1,y,z0},{x1,y,z1},{x0,y,z1}};quad(F,fill);for(int k=0;k<4;++k)line(F[k],F[(k+1)%4],edge,edge,2.5f);
  const float b0[3]={cx,y,cz},b1[3]={cx,y+tall,cz};line(b0,b1,beamA,beamB,3.f);}}

// ---- commands and script API (callable from any thread)
inline void PfQueue(PfCommandRequest r){auto& sh=PathFinder();std::lock_guard<std::mutex> l(sh.guard);if(sh.queue.size()<32)sh.queue.push_back(std::move(r));}
inline void PfGoto(int x,int y,int z,int radius=0){PfCommandRequest r;r.kind=1;r.pos[0]=x;r.pos[1]=y;r.pos[2]=z;r.radius=radius<=4096?radius:4096;r.count=0;PfQueue(r);}
inline void PfFollow(const std::string& name,int dist=0){PfCommandRequest r;r.kind=2;r.name=name;r.radius=dist;PfQueue(r);}
inline void PfMine(const std::vector<std::string>& blocks,int count=0){PfCommandRequest r;r.kind=3;r.blocks=blocks;r.count=count;PfQueue(r);}
inline void PfWander(int radius=0){PfCommandRequest r;r.kind=4;r.radius=radius;PfQueue(r);}
inline void PfBuild(){PfCommandRequest r;r.kind=13;PfQueue(r);}
inline void PfStop(){PfCommandRequest r;r.kind=7;PfQueue(r);}
inline void PfPause(){PfCommandRequest r;r.kind=8;PfQueue(r);}
inline void PfResume(){PfCommandRequest r;r.kind=9;PfQueue(r);}
inline bool PfIsActive(){auto& sh=PathFinder();std::lock_guard<std::mutex> l(sh.guard);return sh.process!=0;}
inline void PfStatus(std::string* state,std::string* process,std::string* detail,int* remaining){static const char* st[]={"Idle","Planning","Moving","Acting","Paused"};static const char* pr[]={"None","Goto","Wander","Follow","Mine","Tunnel","Chop","Build"};
 auto& sh=PathFinder();std::lock_guard<std::mutex> l(sh.guard);*state=st[std::clamp(sh.state,0,4)];*process=pr[std::clamp(sh.process,0,7)];*detail=sh.detail;*remaining=sh.remaining;}
// Chat command (line starts with '.'): returns true when it was a path finder command.
inline bool PfChatCommand(const std::string& line){
 std::vector<std::string> w;{std::string cur;for(char ch:line){if(ch==' '||ch==(char)9){if(!cur.empty())w.push_back(cur);cur.clear();}else cur+=ch;}if(!cur.empty())w.push_back(cur);}
 if(w.empty())return false;std::string c=PfLower(w[0]);if(c==".go")c=".goto";const bool num1=w.size()>1&&(std::isdigit((unsigned char)w[1][0])||w[1][0]=='-');
 if(c!=".goto"&&c!=".mine"&&c!=".wander"&&c!=".follow"&&c!=".tunnel"&&c!=".chop"&&c!=".path"&&c!=".stop"&&c!=".build")return false;
 if(!PathFinder().enabled.load()){PfMessage("Turn Path Finder on first (Movement > Path Finder).");return true;}
 if(game::FeatureStopped(&PathFinder().fault)){PfMessage("Path Finder stopped after an error; it works again in the next world you join.");return true;}
 if(c==".stop"){PfStop();return true;}
 if(c==".build"){if(w.size()>1){PfMessage("Unexpected argument");return true;}PfBuild();return true;}   // no argument; the schematic is checked by the process
 if(c==".path"){const std::string sub=w.size()>1?PfLower(w[1]):std::string("status");if(sub=="stop")PfStop();else if(sub=="pause")PfPause();else if(sub=="resume")PfResume();
  else if(sub=="recalc"||sub=="recalculate"){PfCommandRequest r;r.kind=10;PfQueue(r);}
  else{std::string a,b,d;int n=0;PfStatus(&a,&b,&d,&n);PfMessage(a+" ("+b+(d.empty()?std::string():", "+d)+"), "+std::to_string(n)+" moves left.");}return true;}
 if(c==".goto"){PfCommandRequest r;r.kind=1;const size_t n=w.size()-1;
  if(n>=3&&num1){r.pos[0]=atoi(w[1].c_str());r.pos[1]=atoi(w[2].c_str());r.pos[2]=atoi(w[3].c_str());r.count=0;if(n>=4)r.radius=atoi(w[4].c_str());}
  else if(n==2&&num1){r.pos[0]=atoi(w[1].c_str());r.pos[2]=atoi(w[2].c_str());r.count=2;}
  else if(n==1&&num1){r.pos[1]=atoi(w[1].c_str());r.count=3;}
  else if(n==1&&PfLower(w[1])=="death"){r.kind=11;}
  else if(n==1&&PfLower(w[1])=="axis"){r.kind=12;}
  else if(n>=1){std::string name;for(size_t i=1;i<w.size();++i){if(i>1)name+=' ';name+=w[i];}std::vector<std::string> entries;{auto& sh=PathFinder();std::lock_guard<std::mutex> l(sh.guard);entries=sh.cfg.waypoints;}
   if(!PfWaypoint(entries,name,r.pos)){PfMessage("Usage: .goto <x> <y> <z> [radius], .goto <x> <z>, .goto <y>, .goto <waypoint> .goto axis or .goto death.");return true;}r.count=0;r.name=name;}
  else{PfMessage("Usage: .goto <x> <y> <z> [radius], .goto <x> <z>, .goto <y>, .goto <waypoint> .goto axis or .goto death.");return true;}PfQueue(r);return true;}
 if(c==".follow"){if(w.size()<2){PfMessage("Usage: .follow <player> [distance].");return true;}PfFollow(w[1],w.size()>2?std::max(1,atoi(w[2].c_str())):0);return true;}
 if(c==".wander"){PfWander(w.size()>1?std::clamp(atoi(w[1].c_str()),1,4096):0);return true;}
 if(c==".tunnel"){PfCommandRequest r;r.kind=5;r.count=w.size()>1?std::max(0,atoi(w[1].c_str())):0;PfQueue(r);return true;}
 if(c==".chop"){PfCommandRequest r;r.kind=6;r.count=w.size()>1?std::max(0,atoi(w[1].c_str())):0;PfQueue(r);return true;}
 if(c==".mine"){PfCommandRequest r;r.kind=3;for(size_t i=1;i<w.size();++i){if(i+1==w.size()&&std::isdigit((unsigned char)w[i][0]))r.count=atoi(w[i].c_str());else r.blocks.push_back(w[i]);}PfQueue(r);return true;}
 return false;}

// ============================================================================================ Pearl Chaser
struct PcShared{std::mutex guard;std::atomic<bool> enabled{false},fixed{false},pitchKnown{false},seqSet{false};std::atomic<int64_t> seqAt{0};float maxDistance=16,tolerance=1.5f;int reactionDelay=0;bool silentAim=true,silentSwitch=false,teammates=false;
 struct Seen{uint64_t id=0;float pos[3]{},vel[3]{};};std::vector<Seen> seen;float landing[3]{};float lastPitch=0;bool wasOn=false;volatile long fault=0;};
inline PcShared& PearlChaser(){static auto* s=new PcShared;return *s;}
// AddActorPacket (0x0d): type string at +0x40, runtime id +0x38, position +0x60, velocity +0x6c.
inline void PcReceiveWork(uintptr_t packet){auto& s=PearlChaser();if(!s.enabled.load()||PacketIdOf(packet)!=0x0d)return;char type[96]{};if(!Text(packet+0x40,type,sizeof(type)))return;int kind{};if(!TrajectoryKind(type,&kind)||kind!=3)return;
 PcShared::Seen e;if(!Read(packet+0x38,&e.id)||!Floats(packet+0x60,e.pos,3)||!Floats(packet+0x6c,e.vel,3))return;std::lock_guard<std::mutex> l(s.guard);if(s.seen.size()<64)s.seen.push_back(e);}
inline void PearlChaserReceive(uintptr_t packet){static volatile long stopped_=0;if(!game::FeatureStopped(&stopped_))__try{PcReceiveWork(packet);}__except(game::FeatureFault("PearlChaserReceive",&stopped_,GetExceptionInformation())){game::FeatureFlush();}}
inline float PcDist(const float* a,const float* b){const float x=a[0]-b[0],y=a[1]-b[1],z=a[2]-b[2];return std::sqrt(x*x+y*y+z*z);}
inline bool PcStartRaw(const Context& c,float* start){__try{reinterpret_cast<float*(*)(float*,uint64_t,float)>(c.base+Rva(0x2ceda40))(start,c.actor,1.0f);return Floats((uintptr_t)start,start,3);}__except(EXCEPTION_EXECUTE_HANDLER){return false;}}
inline void PcFinish(int result){auto& s=PearlChaser();s.fixed=false;s.pitchKnown=false;if(result==0){s.seqAt=ActionNs();s.seqSet=true;}RotRelease(kRotPearlChaser);}
// Thrower: the player nearest to the pearl's spawn point (within 3 blocks), within Max Distance of the local player; local player, bots and (without Teammates) teammates are skipped.
inline uintptr_t PcThrower(const Context& c,const PcShared::Seen& e,float maxDistance,bool teammates){uintptr_t best=0;float bestD=9.f;CombatTargetFilter filter;float me[3]{};if(!ActorPos(c.actor,me))return 0;
 ForEachActor(c,[&](uintptr_t a){if(a==c.actor||!CombatTargetValid(c,a,true,true,filter,true,!teammates))return true;float p[3];if(!ActorPos(a,p))return true;
  if(PcDist(me,p)>maxDistance)return true;const float dx=p[0]-e.pos[0],dy=p[1]-e.pos[1],dz=p[2]-e.pos[2];const float d2=dx*dx+dy*dy+dz*dz;if(d2<bestD){bestD=d2;best=a;}return true;});return best;}
inline bool PcLanding(const Context& c,uintptr_t thrower,const PcShared::Seen& e,float* out){const uint64_t region=ActorRegion(c);if(!region)return false;const TrajectoryParams p=TrajectoryBds(3);
 std::vector<TrajectoryBody> bodies;TrajectoryPath path;TrajectorySimulate(c,thrower,region,p,e.pos,e.vel,bodies,path);if(!path.hit||path.points.empty())return false;memcpy(out,path.points.back().data(),12);return true;}
// Closed-form launch velocity: per tick the position is v*A + gravity terms B; 200 steps then 20 bisection steps on the last fractional tick, speed fixed.
inline bool PcSolve(const TrajectoryParams& p,const float* d,float* out){
 const float speed=p.speed;if(!(speed>0.f)||!std::isfinite(speed)||std::sqrt(d[0]*d[0]+d[1]*d[1]+d[2]*d[2])<=9.999999747378752e-06f)return false;
 float G=p.order?p.gravity:0.f,D=p.order?p.air:1.f,A=0.f,B=0.f;
 for(int it=0;it<200;++it){const float a=B+G,b=A+D;const float vx=d[0]/b,vy=(d[1]+a)/b,vz=d[2]/b;
  if(std::sqrt(vx*vx+vy*vy+vz*vz)<=speed){float lo=0.f,hi=1.f;
   for(int j=0;j<20;++j){const float mid=(lo+hi)*0.5f,den=mid*D+A;const float x2=d[0]/den,y2=(d[1]+mid*G+B)/den,z2=d[2]/den;if(std::sqrt(x2*x2+y2*y2+z2*z2)<=speed)hi=mid;else lo=mid;}
   A+=D*hi;const float fx=d[0]/A,fy=(d[1]+B+G*hi)/A,fz=d[2]/A;const float inv=1.f/std::sqrt(fx*fx+fy*fy+fz*fz);out[0]=speed*inv*fx;out[1]=speed*inv*fy;out[2]=speed*inv*fz;return true;}
  D=p.air*D;G=G*p.air+p.gravity;A=b;B=a;}
 return false;}
inline bool PcFlight(const Context& c,uint64_t region,const TrajectoryParams& p,const float* start,const float* vel,const float* target,float* dist){
 std::vector<TrajectoryBody> bodies;TrajectoryPath path;TrajectorySimulate(c,c.actor,region,p,start,vel,bodies,path);if(!path.hit||path.points.empty())return false;*dist=PcDist(path.points.back().data(),target);return true;}
// Pitch scan around `centre` +- `range` (clamped to +-89) with the yaw fixed; returns the pitch with the smallest miss distance.
inline void PcScan(const Context& c,uint64_t region,const TrajectoryParams& p,const float* start,const float* target,float yawDeg,float centre,float range,float step,float* bestPitch,float* bestDist){
 const float hi=centre+range<89.f?centre+range:89.f,lo=-89.f<centre-range?centre-range:-89.f;const int n=(int)std::floor((hi-lo)/step);*bestDist=3.4028234663852886e+38f;*bestPitch=0.f;
 for(int i=0;i<=n;++i){const float pitch=lo+(float)i*step;const float yr=yawDeg*0.01745329238474369f,pr=pitch*0.01745329238474369f;
  const float dir[3]={-std::sin(yr)*std::cos(pr),-std::sin(pr),std::cos(yr)*std::cos(pr)};const float vel[3]={p.speed*dir[0],p.speed*dir[1],p.speed*dir[2]};float dist{};
  if(PcFlight(c,region,p,start,vel,target,&dist)&&dist<*bestDist){*bestDist=dist;*bestPitch=pitch;}}}
inline void PcAim(const Context& c,PcShared& s,bool silentAim,float tolerance){
 float start[3]{};if(!PcStartRaw(c,start))return;const uint64_t region=ActorRegion(c);if(!region)return;const TrajectoryParams p=TrajectoryBds(3);
 const float d[3]={s.landing[0]-start[0],s.landing[1]-start[1],s.landing[2]-start[2]};float yaw=0,pitch=0;bool ok=false;float v[3]{};
 if(PcSolve(p,d,v)){const float n=std::sqrt(v[0]*v[0]+v[1]*v[1]+v[2]*v[2]);if(n>9.999999974752427e-07f){const float dir[3]={v[0]/n,v[1]/n,v[2]/n};const float vel[3]={p.speed*dir[0],p.speed*dir[1],p.speed*dir[2]};float dist{};
   if(PcFlight(c,region,p,start,vel,s.landing,&dist)&&!(tolerance<dist)){yaw=std::atan2(-dir[0],dir[2])*57.29578f;pitch=std::atan2(dir[1],std::sqrt(dir[0]*dir[0]+dir[2]*dir[2]))*-57.29578f;ok=true;}}}
 if(!ok){yaw=std::atan2(-d[0],d[2])*57.29578f;float best{},dist{};
  if(!s.pitchKnown.load()){PcScan(c,region,p,start,s.landing,yaw,0.f,89.f,2.f,&best,&dist);s.lastPitch=best;}
  PcScan(c,region,p,start,s.landing,yaw,s.lastPitch,2.f,0.25f,&best,&dist);
  if(dist<=tolerance){pitch=best;s.lastPitch=best;s.pitchKnown=true;ok=true;}}
 if(!ok){ActionCancel(kActPearlChaser);PcFinish(1);return;}
 RotRequest r;r.module=kRotPearlChaser;r.yaw=yaw;r.pitch=pitch;r.mode=silentAim?2:0;r.priority=3;RotSubmit(std::move(r));}
inline void PearlChaserWork(){
 auto& s=PearlChaser();const bool on=s.enabled.load();Rotations().enabled[kRotPearlChaser]=on;
 if(!on){if(s.wasOn){s.wasOn=false;ActionCancel(kActPearlChaser);RotRelease(kRotPearlChaser);s.fixed=false;s.pitchKnown=false;s.seqSet=false;std::lock_guard<std::mutex> l(s.guard);s.seen.clear();}return;}
 if(!s.wasOn){s.wasOn=true;s.fixed=false;s.pitchKnown=false;s.seqAt=ActionNs();s.seqSet=true;}   // activation: state cleared, timestamp = now
 float maxDistance,tolerance;int delay;bool silentAim,silentSwitch,teammates;std::vector<PcShared::Seen> take;
 {std::lock_guard<std::mutex> l(s.guard);maxDistance=s.maxDistance;tolerance=s.tolerance;delay=s.reactionDelay;silentAim=s.silentAim;silentSwitch=s.silentSwitch;teammates=s.teammates;take.swap(s.seen);}
 Context c{};if(!Local(&c))return;
 const bool recent=s.seqSet.load()&&ActionMs(s.seqAt.load())<=1000.0;
 if(!s.fixed.load()&&!recent&&!take.empty()&&ActionHasItem(c,c.actor,"ender_pearl")){
  for(auto& e:take){const uintptr_t thrower=PcThrower(c,e,maxDistance,teammates);if(!thrower)continue;float land[3]{},start[3]{};
   if(!PcLanding(c,thrower,e,land)||!PcStartRaw(c,start)||PcDist(start,land)<3.0f)continue;
   ActionSequence q;q.module=kActPearlChaser;q.silent=silentSwitch?1:0;if(delay>0)q.steps.push_back(StepWait((double)delay*50.0));
   q.steps.push_back(StepRotationReady(kRotPearlChaser,0.01f));q.steps.push_back(StepFindName("ender_pearl"));q.steps.push_back(StepUse());q.steps.push_back(StepSwitchBack());
   q.done=[](int result){PcFinish(result);};ActionSubmit(std::move(q));
   memcpy(s.landing,land,12);s.pitchKnown=false;s.fixed=true;break;}}
 if(s.fixed.load())PcAim(c,s,silentAim,tolerance);}
inline void PearlChaserTick(){auto& s=PearlChaser();if(s.fault)return;__try{PearlChaserWork();}__except(EXCEPTION_EXECUTE_HANDLER){InterlockedExchange(&s.fault,1);if(actionLog)actionLog("Pearl Chaser: fault caught, module stopped until restart");}}
}
