// Player modules: Pearl Aimbot, Faster Inventory, Auto Fish and Regen.
// - Pearl Aimbot. Send: an InventoryTransactionPacket (0x1e) whose transaction (+0x170) is an item use (+8 == 2) of
//   the click-air kind (+0x68 == 1) with a valid slot (+0x84) holding "ender_pearl" remembers the throw time.
//   Receive: MovePlayerPacket (0x13) in teleport mode (+0x50 == 2) for the local runtime id (+0x30) within 5000 ms of
//   the throw arms a snap for the next level tick; without Silent the packet rotation (+0x44 pitch, +0x48 yaw, +0x4c
//   head yaw) is rewritten towards the target closest to the landing position (+0x38).
//   Target: actors (ActorOwnerComponent, walked from the end) passing Anti Bot, Teams and the common filter (marked
//   actors skipped), squared distance from the origin to their position <= Max Distance^2, nearest wins.
//   Aim point: box interpolated by the frame fraction; Closest Point off -> centre with y = (minY + maxY) / 2; on ->
//   centre (y at 90% of the height) blended with the box point closest to the origin by Multipoint Horizontal /
//   Vertical. Yaw = atan2(-dx, dz), pitch = -atan2(dy, hypot(dx, dz)).
//   Level tick (Snap): origin = player position; Silent -> absolute mode 2 request; otherwise a mode 0 (game turn)
//   request only when the yaw (fmod 360) or pitch differs by more than 0.1 deg; priority 3. Track mode then follows
//   that target's runtime id for Tracking Duration ms.
//   GameUpdate (Track): eye interpolated by the frame fraction (RVA 0x2ceda40), current rotation from actor+0x250 or,
//   without Silent, the camera angles; relative request {dyaw * f, Vertical ? dpitch * f : 0}, f = clamp(1 -
//   exp(-k dt)), k = 84 - 72 x Smoothing kept >= 12 (<= 12 above 1); mode 2 with Silent, else 1; priority 3. Stops
//   when the target leaves the common filter or the duration elapses.
// - Faster Inventory (no settings). The open-inventory InteractPacket (0x21, action 6) sent while the inventory
//   scheduler is idle counts a pending server answer and asks for a local opening; the next GameUpdate builds a
//   ContainerOpenPacket (0x2e: window 0, type -1, block = the feet cell, unique id -1) and handles it locally, so the screen opens at
//   once. The server's ContainerOpenPacket of type -1 that answers is then cancelled (absorbed); if the local screen is
//   gone by then, the server window is closed with ContainerClosePacket (0x2f {window, 0xff, 0}) on the next
//   GameUpdate. Closing the inventory (0x2f window 0 or type -1) sent with answers pending arms the absorption.
//   The local screen uses window 0, so the first opening in a world is left to the server and local opening is used
//   afterwards only when the server answered with window 0. A server that uses another window (PowerNukkitX: 2) keeps
//   the plain inventory: answering its window with a local window 0 made it reject the closes, and the inventory never
//   opened again. Pending
//   state is dropped after 3000 ms. The GameUpdate part also runs while the module is off (cleanup).
//   The container model is *(actor+0x5c8), its type byte at +0x51 (-1 = player inventory).
// - Auto Fish. The player casts; the module follows the player's fishing hook (ActorTypeComponent == 0x4d, synched
//   data item 5 = owner unique id) and detects a bite from ActorEventPacket 0x1b event 13 for the hook's runtime id or,
//   with Motion Detection, a hook settled in water for 1000 ms (moves <= 0.05) that dips by 0.1. Water = material byte
//   5 (legacy +0x28) at the hook cell or the two cells below (region vtable 0x28). Recast On Entity: hook target
//   (synched item 6) set. Recast On Ground: hook out of water and still (<= 0.01, no target) for Ground Time s.
//   Reel sequence (action queue, 5000 ms limit): Reel Delay, holding fishing_rod, use, counter + 1; with Recast:
//   Recast Delay, wait until the old hook is gone and 1000 ms since the last recast, looking at water (clip ray of 24
//   blocks without liquids, then 0.1-block steps along it), rod held (Swap To Rod: find it), use, phase "recast".
//   The module list shows the number of catches.
// - Regen. Mines redstone ore (minecraft:redstone_ore, lit_redstone_ore, deepslate_redstone_ore,
//   lit_deepslate_redstone_ore; exact names) with the generic block breaker also used by Bed Breaker: sub-chunk palette
//   scan of the 3 x 3 chunks (1 ms budget, changed sub-chunks, round robin), pauses (no external input data, player
//   breaking, Creative / Spectator, health <= 0, Scaffold or Nuker on). Absorption (attribute 13): full when
//   min(Cap, max if > 0) - 0.01 <= current. Stop: nothing starts and a running break is cancelled while full; Queue:
//   the break holds (breaker state 3, "Queued") once progress + 2 x per-tick >= 0.99999 or progress >= Queue
//   Threshold while full; Always: ignored.
//   Target choice (breaker idle, cursor free): Steal target (< 5000 ms), the Locked target (Range + 0.75), the
//   remembered target, then every scanned ore within Range: first pass without uncovering, second pass with Uncover
//   Range when Uncover is on; lowest break ticks, then nearest. An ore needs a tool faster than the hand (speed > 1,
//   Item Profiles first) and either an exposed face facing the player (no wall ray) or, uncovering, a straight line of
//   up to Uncover Range solid blocks in front of a face that faces the player, the first block of it with an exposed
//   face being broken first.
//   Steal: LevelEventPacket 0x19 event 3600 (start break) at a block next to a hidden ore within Range remembers that
//   ore; event 3601 at the same block forgets it.
//   Breaker: action-queue slot switch (Ghost Switch = silent, else 50 ms), rotation to the face centre (mode 2 Silent
//   Rotations / 1, priority 3) when Rotate, start / continue destroy on the game mode with the slot and actor+0xb4a
//   forced, 1000 ms to start, 1500 ms stall, confirmation by SubChunkSetBlock or 1500 ms; a cancelled target is
//   remembered for the next choice.
#pragma once
#include "game_player.hpp"
#include "game_bed_breaker.hpp"
#include "game_sticky_aim.hpp"
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <functional>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace game {
enum{kRotPearlAimbot=25,kRotRegen=26,kAutoFishAction=25,kRegenAction=26};

// ---- shared reads
// Optional 64-bit id (value + present), compared as std::optional.
struct OptId{int64_t v=0;bool ok=false;bool operator==(const OptId& o)const{return ok==o.ok&&(!ok||v==o.v);}bool operator!=(const OptId& o)const{return !(*this==o);}};
inline OptId PcUniqueId(uintptr_t actor){OptId r{};uintptr_t registry{},element{};uint32_t entity{};if(ActorEntity(actor,&registry,&entity)&&Element(registry,entity,"ActorUniqueIDComponent",8,&element)&&Read(element,&r.v))r.ok=true;return r;}
inline OptId PcRuntimeId(uintptr_t actor){OptId r{};uint64_t id{};if(RuntimeId(actor,&id)){r.v=(int64_t)id;r.ok=true;}return r;}
// Synched actor data item `index` (pointer vector of SynchedActorDataComponent), entity id value at item+0x10;
// present when it is neither 0 nor -1.
inline OptId PcSynchedId(uintptr_t actor,int index){OptId r{};uintptr_t registry{},element{};uint32_t entity{};uint64_t begin{},end{},item{};int64_t v{};
 if(!ActorEntity(actor,&registry,&entity)||!Element(registry,entity,"SynchedActorDataComponent",0x48,&element)||!Read(element,&begin)||!Read(element+8,&end)||end<begin||end-begin<(uint64_t)(index*8+1))return r;
 if(!Read(begin+index*8,&item)||!item||!Read(item+0x10,&v)||(uint64_t)v-1u>0xfffffffffffffffdull)return r;r.v=v;r.ok=true;return r;}
// Attribute `id` of the local player: current value (+0x7c) and maximum (+0x78).
inline bool PcAttribute(const Context& c,uint32_t id,float* current,float* maximum){uintptr_t element{};uint64_t begin{},end{},table{};
 if(!Element(c.registry,c.entity,"AttributesComponent",0x50,&element)||!Read(element,&begin)||!Read(element+8,&end)||!Read(element+0x18,&table)||end<begin||!table)return false;
 const uint64_t count=(end-begin)/4;if(!count||count>256)return false;
 for(uint64_t i=0;i<count;++i){uint32_t a{};if(!Read(begin+i*4,&a))return false;if(a!=id)continue;return Read(table+i*0x88+0x7c,current)&&Read(table+i*0x88+0x78,maximum);}return false;}
inline int PcPacketId(uintptr_t packet){return PacketIdOf(packet);}
// Block of a region at a cell through region->vtable[0x28] (as the projectile simulation), material byte legacy+0x28.
inline bool PcWater(const Context& c,uint64_t region,const int32_t* cell){const uint64_t get=Virtual0(c,region,0x28);if(!get)return false;int32_t at[3]={cell[0],cell[1],cell[2]};
 const uint64_t block=reinterpret_cast<uint64_t(*)(uint64_t,int32_t*)>(get)(region,at);uint64_t legacy{};uint8_t material{};return block&&Read(block+0x68,&legacy)&&legacy&&Read(legacy+0x28,&material)&&material==5;}

// =====================================================================================================================
// Pearl Aimbot
struct PearlAimConfig{int mode=0;int duration=1000;float smoothing=0.1f;bool vertical=true,closest=true;int mpH=100,mpV=100;bool silent=false;float maxDistance=10.f;};
struct PearlAimShared{std::mutex guard;std::atomic<bool> enabled{false};PearlAimConfig cfg;
 int64_t thrownAt=0;bool thrown=false;bool snap=false;int64_t trackAt=0;bool trackTimed=false;uint64_t trackId=0;bool tracking=false;volatile long fault=0;};
inline PearlAimShared& PearlAim(){static auto* s=new PearlAimShared;return *s;}
// Activation and deactivation both clear the throw, snap and tracking flags.
inline void PearlAimReset(){auto& s=PearlAim();std::lock_guard<std::mutex> l(s.guard);s.thrown=false;s.snap=false;s.trackTimed=false;s.tracking=false;}
inline PearlAimConfig PearlAimCfg(){auto& s=PearlAim();std::lock_guard<std::mutex> l(s.guard);return s.cfg;}
// Aim point in the target box (interpolated with `alpha`) seen from `origin`.
inline bool PearlAimPoint(const PearlAimConfig& k,uintptr_t target,const float* origin,float alpha,float* out){float b[6];if(!AimBox(target,alpha,b))return false;
 if(!k.closest){out[0]=(b[0]+b[3])*0.5f;out[1]=(b[1]+b[4])*0.5f;out[2]=(b[2]+b[5])*0.5f;return true;}
 const float h=std::min(1.0f,std::max(0.0f,(float)k.mpH/100.0f)),v=std::min(1.0f,std::max(0.0f,(float)k.mpV/100.0f));
 const float cx=std::min(b[3],std::max(b[0],origin[0])),cy=std::min(b[4],std::max(b[1],origin[1])),cz=std::min(b[5],std::max(b[2],origin[2]));
 out[0]=(b[0]+b[3])*0.5f*(1.0f-h)+cx*h;out[1]=((b[4]-b[1])*0.9f+b[1])*(1.0f-v)+cy*v;out[2]=(b[2]+b[5])*0.5f*(1.0f-h)+cz*h;return true;}
inline void PearlAimAngles(const float* from,const float* to,float* yaw,float* pitch){const float dx=to[0]-from[0],dy=to[1]-from[1],dz=to[2]-from[2];
 *pitch=atan2f(dy,hypotf(dx,dz))*-57.295780181884766f;*yaw=atan2f(-dx,dz)*57.295780181884766f;}
// Nearest accepted target to `origin` within Max Distance (walked from the end of the actor view, strict "<").
inline uintptr_t PearlAimTarget(const Context& c,const PearlAimConfig& k,const float* origin){uintptr_t owners{};uint64_t begin{},end{};
 if(!Storage(c.registry,Fnv("ActorOwnerComponent"),&owners)||!Read(owners+0x20,&begin)||!Read(owners+0x28,&end)||end<begin||(end-begin)%4||(end-begin)>4*65536)return 0;
 float own[3];if(!ActorPos(c.actor,own))return 0;const float limit=k.maxDistance*k.maxDistance;uintptr_t best=0;float bestD=3.4028234663852886e+38f;CombatTargetFilter filter;
 for(uint64_t at=end;at>begin;){at-=4;uint32_t entity{};uintptr_t slot{};uint64_t actor{};if(!Read(at,&entity))break;if(entity>=0xfffc0000u)continue;if(!InStorage(owners,entity,8,&slot)||!Read(slot,&actor)||!actor)continue;
  if(!CombatTargetValid(c,(uintptr_t)actor,false,true,filter,true,true))continue;float p[3];if(!ActorPos((uintptr_t)actor,p))continue;
  float d=(p[2]-origin[2])*(p[2]-origin[2])+(p[1]-origin[1])*(p[1]-origin[1])+(p[0]-origin[0])*(p[0]-origin[0]);if(limit<d)d=3.4028234663852886e+38f;
  if(d<bestD){bestD=d;best=(uintptr_t)actor;}}
 return best;}
// PacketSend (read only)
inline void PearlAimSendWork(uintptr_t packet){auto& s=PearlAim();if(!s.enabled)return;if(PcPacketId(packet)!=0x1e)return;Context c{};if(!Local(&c))return;
 uint64_t t{};int32_t type{},action{},slot{};if(!Read(packet+0x170,&t)||!t||!Read(t+8,&type)||type!=2||!Read(t+0x68,&action)||action!=1||!Read(t+0x84,&slot)||slot<0)return;
 const uint64_t box=ActionContainer(c.actor);if(slot>=ActionSize(c,box)||!ActionNameIs(ActionStackAt(c,box,slot),"ender_pearl"))return;
 std::lock_guard<std::mutex> l(s.guard);s.thrownAt=ActionNs();s.thrown=true;}
// PacketReceive: teleport after the throw; may rewrite the packet rotation.
inline void PearlAimReceiveWork(uintptr_t packet){auto& s=PearlAim();if(!s.enabled)return;if(PcPacketId(packet)!=0x13)return;uint8_t mode{};uint64_t who{},mine{};Context c{};
 if(!Read(packet+0x50,&mode)||mode!=2||!Local(&c)||!RuntimeId(c.actor,&mine)||!Read(packet+0x30,&who)||who!=mine)return;
 PearlAimConfig k;{std::lock_guard<std::mutex> l(s.guard);if(!s.thrown||ActionMs(s.thrownAt)>5000.0)return;s.thrown=false;s.snap=true;k=s.cfg;}
 if(k.silent)return;float origin[3];if(!Floats(packet+0x38,origin,3))return;const uintptr_t target=PearlAimTarget(c,k,origin);if(!target)return;
 float p[3],yaw{},pitch{};if(!PearlAimPoint(k,target,origin,1.0f,p))return;PearlAimAngles(origin,p,&yaw,&pitch);
 *(float*)(packet+0x44)=pitch;*(float*)(packet+0x48)=yaw;*(float*)(packet+0x4c)=yaw;}
// LevelTick: Snap
inline void PearlAimTickWork(){auto& s=PearlAim();if(!s.enabled)return;Context c{};
 if(!Local(&c)){std::lock_guard<std::mutex> l(s.guard);s.thrown=false;s.snap=false;s.trackTimed=false;s.tracking=false;return;}
 PearlAimConfig k;{std::lock_guard<std::mutex> l(s.guard);if(!s.snap)return;s.snap=false;k=s.cfg;}
 float origin[3];if(!ActorPos(c.actor,origin))return;const uintptr_t target=PearlAimTarget(c,k,origin);if(!target)return;
 float p[3],yaw{},pitch{};if(!PearlAimPoint(k,target,origin,1.0f,p))return;PearlAimAngles(origin,p,&yaw,&pitch);
 RotRequest r;r.module=kRotPearlAimbot;r.yaw=yaw;r.pitch=pitch;r.relative=false;r.priority=3;bool submit=true;
 if(k.silent)r.mode=2;
 else{float cp{},cy{};if(!RotRead(c.actor,&cp,&cy))return;submit=std::fabs(fmodf(yaw-cy,360.0f))>0.1f||std::fabs(pitch-cp)>0.1f;r.mode=0;}
 if(submit)RotSubmit(std::move(r));
 if(k.mode!=1)return;uint64_t id{};const bool has=RuntimeId(target,&id);
 std::lock_guard<std::mutex> l(s.guard);s.trackId=id;s.tracking=has;s.trackAt=ActionNs();s.trackTimed=true;}
// GameUpdate: Track
inline void PearlAimUpdateWork(){auto& s=PearlAim();if(!s.enabled)return;PearlAimConfig k;uint64_t id{};
 {std::lock_guard<std::mutex> l(s.guard);if(!s.tracking)return;k=s.cfg;if(s.trackTimed&&(double)k.duration<ActionMs(s.trackAt)){s.trackTimed=false;s.tracking=false;return;}id=s.trackId;}
 Context c{};uintptr_t target{};auto stop=[&]{std::lock_guard<std::mutex> l(s.guard);s.trackTimed=false;s.tracking=false;};
 if(!Local(&c)||!ActorByRuntimeId(c.registry,id,&target)||!target||!CombatTargetValid(c,target,false,true,true,true)){stop();return;}
 const float alpha=AimAlpha(c);float eye[3];reinterpret_cast<float*(*)(float*,uint64_t,float)>(c.base+0x2ceda40)(eye,c.actor,alpha);
 if(!std::isfinite(eye[0])||!std::isfinite(eye[1])||!std::isfinite(eye[2]))return;
 float cp{},cy{};if(!RotRead(c.actor,&cp,&cy))return;if(!k.silent){float fp{},fy{};if(FreeLookCameraAngles(c,&fp,&fy)){cp=fp;cy=fy;}}
 float p[3],yaw{},pitch{};if(!PearlAimPoint(k,target,eye,alpha,p))return;PearlAimAngles(eye,p,&yaw,&pitch);
 const float dy=fmodf(yaw-cy,360.0f),dp=pitch-cp;const float sm=k.smoothing;float kk=12.0f;
 if(std::isfinite(sm)){if(sm!=1.0f){const float x=fmaf(sm,-72.0f,84.0f);kk=1.0f<sm?(12.0f<x?12.0f:x):(x<12.0f?12.0f:x);}}else if(std::isinf(sm))kk=sm*-72.0f;else kk=sm+sm;
 float f=AimExp(kk,RotFrameDt());f=f<0.0f?0.0f:(f>1.0f?1.0f:f);
 RotRequest r;r.module=kRotPearlAimbot;r.relative=true;r.yaw=dy*f;r.pitch=k.vertical?dp*f:0.0f;r.mode=k.silent?2:1;r.priority=3;RotSubmit(std::move(r));}

// =====================================================================================================================
// Faster Inventory
struct FasterInvShared{std::recursive_mutex guard;std::atomic<bool> enabled{false};int pending=0;bool absorb=false,openWanted=false,openLocal=false,closing=false;
 bool windowKnown=false,otherWindow=false;   // first server inventory window of this world; not 0 (PowerNukkitX: 2) = no local opening
 std::vector<uint8_t> windows;int64_t at=0;bool timed=false;volatile long fault=0;};
inline FasterInvShared& FasterInv(){static auto* s=new FasterInvShared;return *s;}
inline thread_local bool fasterInvLocalReceive=false;   // set while our ContainerOpenPacket is handled locally (not an incoming packet)
inline bool FasterInvSchedulerIdle(){auto& s=Inventory();std::lock_guard<std::recursive_mutex> l(s.guard);return s.state==0;}
inline bool FasterInvOpen(uintptr_t actor){uint64_t model{};int8_t type{};return Read(actor+0x5c8,&model)&&model&&Read(model+0x51,&type)&&type==-1;}
// Activation: everything cleared, timer started.
inline void FasterInvActivate(){auto& s=FasterInv();std::lock_guard<std::recursive_mutex> l(s.guard);s.pending=0;s.absorb=s.openWanted=s.openLocal=s.closing=false;s.windows.clear();s.at=ActionNs();s.timed=true;}
inline void FasterInvSendWork(uintptr_t packet){auto& s=FasterInv();if(!s.enabled)return;const int id=PcPacketId(packet);if(id!=0x2f&&id!=0x21)return;const bool idle=FasterInvSchedulerIdle();   // read before our lock (the scheduler sends under its own lock)
 std::lock_guard<std::recursive_mutex> l(s.guard);if(s.closing)return;
 if(id==0x2f){uint8_t w{};int8_t t{};Read(packet+0x30,&w);Read(packet+0x31,&t);if(w!=0&&t!=-1)return;
  if(s.pending>0)s.absorb=true;s.openWanted=false;s.openLocal=false;s.at=ActionNs();s.timed=true;return;}
 if(id!=0x21||!s.windowKnown||s.otherWindow)return;   // the first opening of a world is left to the server
 uint8_t action{};Context c{};if(!Read(packet+0x30,&action)||action!=6||!Local(&c)||!StickyHasInput(c)||!idle)return;
 if(!FasterInvOpen(c.actor)){s.openLocal=false;s.absorb=false;s.pending=1;s.openWanted=true;}else{s.pending+=1;s.openWanted=!s.openLocal;}
 s.at=ActionNs();s.timed=true;}
// Returns true when the packet must not be handled.
inline bool FasterInvReceiveWork(uintptr_t packet){auto& s=FasterInv();if(!s.enabled||fasterInvLocalReceive)return false;const int id=PcPacketId(packet);if(id!=0x2f&&id!=0x2e)return false;const bool idle=FasterInvSchedulerIdle();std::lock_guard<std::recursive_mutex> l(s.guard);
 if(id==0x2f){uint8_t w{};int8_t t{};Read(packet+0x30,&w);Read(packet+0x31,&t);
  if(w==0||t==-1){s.windows.erase(std::remove(s.windows.begin(),s.windows.end(),w),s.windows.end());s.openLocal=false;s.absorb=false;}return false;}
 if(id!=0x2e||!idle)return false;int8_t t{};uint8_t w{};if(!Read(packet+0x31,&t)||t!=-1||!Read(packet+0x30,&w))return false;
 if(!s.windowKnown){s.windowKnown=true;s.otherWindow=w!=0;}   // the local window 0 can only match a server that uses it
 if(s.pending==0)return false;
 s.pending-=1;if(!s.absorb)return false;
 if(s.openLocal&&w!=0){if(s.pending==0)s.absorb=false;return false;}   // the client takes the server's window
 if(!s.openLocal)s.windows.push_back(w);if(s.pending==0)s.absorb=false;return true;}
// GameUpdate, also while the module is off.
inline void FasterInvUpdateWork(){auto& s=FasterInv();Context c{};const bool idle=FasterInvSchedulerIdle();std::unique_lock<std::recursive_mutex> l(s.guard);
 if(!Local(&c)){s.pending=0;s.absorb=s.openWanted=s.openLocal=s.closing=s.windowKnown=s.otherWindow=false;s.windows.clear();s.at=ActionNs();s.timed=true;return;}   // out of a world: the next server is judged again
 if(!s.windows.empty()){std::vector<uint8_t> list;list.swap(s.windows);s.closing=true;
  for(uint8_t w:list){GamePacket p=CreatePacket(c,0x2f);if(p.object){*(uint8_t*)(p.object+0x30)=w;*(uint8_t*)(p.object+0x31)=0xff;*(uint8_t*)(p.object+0x32)=0;SendPacket(c,p.object);}ReleasePacket(p);}
  s.closing=false;}
 if(s.pending==0)s.absorb=false;
 if(!s.enabled){s.absorb=s.openWanted=s.openLocal=false;s.pending=0;return;}
 if(s.timed&&ActionMs(s.at)>3000.0){s.pending=0;s.absorb=false;s.openWanted=false;}
 const bool wanted=s.openWanted;s.openWanted=false;
 if(!wanted||!StickyHasInput(c)||!idle||FasterInvOpen(c.actor))return;
 l.unlock();GamePacket p=CreatePacket(c,0x2e);
 // Block position = the player's feet cell: the game closes a container screen whose position is out of reach, so the
 // reference (0, 0, 0) closed at once anywhere but near the world origin; servers send the player's position too.
 if(p.object){*(uint16_t*)(p.object+0x30)=0xff00;memset((void*)(p.object+0x34),0,12);float feet[3]{};
  if(ActorFeet(c.actor,feet)){const int32_t cell[3]={(int32_t)std::floor(feet[0]),(int32_t)std::floor(feet[1]),(int32_t)std::floor(feet[2])};memcpy((void*)(p.object+0x34),cell,12);}
  *(int64_t*)(p.object+0x40)=-1;fasterInvLocalReceive=true;ReceivePacket(c,&p);fasterInvLocalReceive=false;}
 ReleasePacket(p);l.lock();if(FasterInvOpen(c.actor)){s.openLocal=true;s.absorb=true;}}

// =====================================================================================================================
// Auto Fish
struct AutoFishConfig{float reelLow=80,reelHigh=160;bool recast=true;float recastLow=300,recastHigh=600;bool motion=true,onGround=false;int groundTime=3;bool onEntity=false,swap=false;};
struct AutoFishShared{std::mutex guard;std::atomic<bool> enabled{false};AutoFishConfig cfg;int phase=0;OptId tracked,trackedRuntime,old;float last[3]{};bool known=false;bool bite=false,settled=false;
 std::atomic<int> catches{0};int64_t calmAt=0,groundAt=0,phaseAt=0;bool calmOn=false,groundOn=false,phaseOn=false;volatile long fault=0;};
inline AutoFishShared& AutoFish(){static auto* s=new AutoFishShared;return *s;}
inline void AutoFishClear(AutoFishShared& s){s.phase=0;s.tracked.ok=false;s.trackedRuntime.ok=false;s.known=false;s.bite=false;s.settled=false;}
// Activation and deactivation: full reset (counter included), three timers started. Deactivation also cancels the
// module's sequence (ActionCancelRequest, handled on the action thread).
inline void AutoFishReset(bool deactivate){auto& s=AutoFish();if(deactivate)ActionCancelRequest(kAutoFishAction);std::lock_guard<std::mutex> l(s.guard);AutoFishClear(s);s.old.ok=false;s.catches=0;
 const int64_t now=ActionNs();s.calmAt=s.groundAt=s.phaseAt=now;s.calmOn=s.groundOn=s.phaseOn=true;}
// The player's fishing hook: actor type 0x4d whose owner (synched item 5) is the player's unique id.
inline uintptr_t AutoFishHook(const Context& c){const OptId mine=PcUniqueId(c.actor);if(!mine.ok)return 0;uintptr_t owners{};uint64_t begin{},end{};
 if(!Storage(c.registry,Fnv("ActorOwnerComponent"),&owners)||!Read(owners+0x20,&begin)||!Read(owners+0x28,&end)||end<begin||(end-begin)%4||(end-begin)>4*65536)return 0;
 for(uint64_t at=end;at>begin;){at-=4;uint32_t entity{};uintptr_t slot{},element{};uint64_t actor{};int32_t type{};if(!Read(at,&entity))break;if(entity>=0xfffc0000u)continue;
  if(!Element(c.registry,entity,"ActorTypeComponent",4,&element)||!Read(element,&type)||type!=0x4d)continue;if(!InStorage(owners,entity,8,&slot)||!Read(slot,&actor)||!actor)continue;
  if(PcSynchedId((uintptr_t)actor,5)==mine)return (uintptr_t)actor;}
 return 0;}
inline bool AutoFishRod(uintptr_t actor){Context c{};return Client(&c)&&ActionNameIs(ActionHeld(c,actor),"fishing_rod");}
// Looking at water: clip ray of 24 blocks (no liquids) from the player position along the view, then cells every 0.1
// block up to the hit (or 24); a water cell, or the hit block being water, answers true.
inline bool AutoFishLooksAtWater(uintptr_t actor){Context c{};if(!Local(&c)||c.actor!=actor)return false;const uint64_t region=ActorRegion(c);if(!region)return false;
 float pos[3],pitch{},yaw{};if(!ActorPos(actor,pos)||!RotRead(actor,&pitch,&yaw))return false;{float fp{},fy{};if(FreeLookCameraAngles(c,&fp,&fy)){pitch=fp;yaw=fy;}}
 const float pr=pitch*0.01745329238474369f,yr=yaw*0.01745329238474369f;const float cp=cosf(pr),sp=sinf(pr),sy=sinf(yr),cy=cosf(yr);const float dx=-sy*cp,dz=cy*cp;
 const float to[3]={pos[0]+dx*24.0f,pos[1]-sp*24.0f,pos[2]+dz*24.0f};const uint64_t clipOf=Virtual0(c,region,0x198);if(!clipOf)return false;
 using Clip=void*(*)(uint64_t,void*,const float*,const float*,bool,bool,int,bool,bool,uint64_t,const std::function<bool()>*,bool);static const std::function<bool()> accept=[]{return true;};
 alignas(16) uint8_t res[0x200]{};reinterpret_cast<Clip>(clipOf)(region,res,pos,to,false,true,25,false,false,actor,&accept,false);
 uint64_t weak{};memcpy(&weak,res+0x40,8);if(weak&&InterlockedDecrement((volatile long*)(weak+0xc))==0){uint64_t t{},rel{};if(Read(weak,&t)&&Read(t+8,&rel)&&rel)reinterpret_cast<void(*)(uint64_t)>(rel)(weak);}
 int type{};memcpy(&type,res+0x18,4);float hit[3];memcpy(hit,res+0x2c,12);float span=24.0f;
 if(type==0)span=sqrtf((hit[2]-pos[2])*(hit[2]-pos[2])+(hit[1]-pos[1])*(hit[1]-pos[1])+(hit[0]-pos[0])*(hit[0]-pos[0]));
 for(float t=0.0f;0.0f<=span&&t<=span;t+=0.1f){const int32_t cell[3]={(int32_t)floorf(dx*t+pos[0]),(int32_t)floorf(pos[1]-sp*t),(int32_t)floorf(dz*t+pos[2])};if(PcWater(c,region,cell))return true;}
 if(type!=0)return false;int32_t b[3];memcpy(b,res+0x20,12);return PcWater(c,region,b);}
// ActorEventPacket 0x1b, event 13, for the tracked hook.
inline void AutoFishReceiveWork(uintptr_t packet){auto& s=AutoFish();if(!s.enabled||PcPacketId(packet)!=0x1b)return;uint8_t ev{};int64_t id{};
 if(!Read(packet+0x38,&ev)||ev!=13||!Read(packet+0x30,&id))return;std::lock_guard<std::mutex> l(s.guard);if(s.phase==1&&s.trackedRuntime.ok&&s.trackedRuntime.v==id)s.bite=true;}
// LevelTick: motion and ground detection for the tracked hook.
inline void AutoFishTickWork(){auto& s=AutoFish();if(!s.enabled)return;Context c{};if(!Local(&c))return;{std::lock_guard<std::mutex> l(s.guard);if(s.phase!=1)return;}
 const uintptr_t hook=AutoFishHook(c);if(!hook)return;const OptId uid=PcUniqueId(hook);float p[3];if(!ActorPos(hook,p))return;
 Context hc=c;hc.actor=hook;const uint64_t region=ActorRegion(hc);bool water=false;
 if(region){const int32_t x=(int32_t)floorf(p[0]),y=(int32_t)floorf(p[1]),z=(int32_t)floorf(p[2]);for(int k=0;k<3&&!water;++k){const int32_t cell[3]={x,y-k,z};water=PcWater(c,region,cell);}}
 const bool hasTarget=PcSynchedId(hook,6).ok;
 std::lock_guard<std::mutex> l(s.guard);if(s.phase!=1||uid!=s.tracked)return;
 const float old[3]={s.last[0],s.last[1],s.last[2]};memcpy(s.last,p,12);if(!s.known){s.known=true;return;}
 const float dy=p[1]-old[1],d=std::fabs(p[2]-old[2])+std::fabs(dy)+std::fabs(p[0]-old[0]);const int64_t now=ActionNs();
 if(!water){s.settled=false;s.calmAt=now;s.calmOn=true;if(d<=0.01f&&!hasTarget)return;s.groundAt=now;s.groundOn=true;return;}
 s.groundAt=now;s.groundOn=true;if(!s.cfg.motion)return;
 if(!s.settled){if(0.05f<d){s.calmAt=now;s.calmOn=true;}s.settled=s.calmOn&&ActionMs(s.calmAt)>1000.0;}
 else if(dy<=-0.1f)s.bite=true;}
// Reel sequence (submitted on the GameUpdate thread, steps run by the action queue).
inline void AutoFishSubmit(const AutoFishConfig& k,OptId old){ActionSequence q;q.module=kAutoFishAction;q.silent=0;
 q.conditions.push_back([](uintptr_t,int64_t start){const double e=ActionMs(start);return e<5000.0||e==5000.0;});
 const float reel=ActionRandom(k.reelLow,k.reelHigh);if(0.0f<reel)q.steps.push_back(StepWait((double)reel));
 q.steps.push_back(StepPredicate([](uintptr_t a){return AutoFishRod(a);}));q.steps.push_back(StepUse());
 q.steps.push_back([](ActionCtx&){AutoFish().catches++;return 1;});
 if(k.recast){const float again=ActionRandom(k.recastLow,k.recastHigh);if(0.0f<again)q.steps.push_back(StepWait((double)again));
  q.steps.push_back([old](ActionCtx& x){const uintptr_t hook=AutoFishHook(*x.c);if(hook&&PcUniqueId(hook)==old)return 0;auto& s=AutoFish();std::lock_guard<std::mutex> l(s.guard);
   return s.phaseOn&&ActionMs(s.phaseAt)>1000.0?1:0;});
  q.steps.push_back(StepPredicate([](uintptr_t a){return AutoFishLooksAtWater(a);}));
  if(k.swap)q.steps.push_back(StepFindName("fishing_rod"));else q.steps.push_back(StepPredicate([](uintptr_t a){return AutoFishRod(a);}));
  q.steps.push_back(StepUse());
  q.steps.push_back([](ActionCtx&){auto& s=AutoFish();std::lock_guard<std::mutex> l(s.guard);s.phase=3;s.phaseAt=ActionNs();s.phaseOn=true;return 1;});}
 q.done=[](int){auto& s=AutoFish();std::lock_guard<std::mutex> l(s.guard);if(s.phase==2)AutoFishClear(s);};
 ActionSubmit(std::move(q));}
// GameUpdate: hook tracking and reel decision.
inline void AutoFishUpdateWork(){auto& s=AutoFish();if(!s.enabled)return;Context c{};float health{};
 if(!Local(&c)||!StickyHasInput(c)||!PlayerHealth(c,&health)||!(0.0f<health))return;{std::lock_guard<std::mutex> l(s.guard);if(s.phase==2)return;}
 const uintptr_t hook=AutoFishHook(c);bool reel=false;AutoFishConfig k;OptId old;
 {const OptId uid=hook?PcUniqueId(hook):OptId{};const OptId rid=hook?PcRuntimeId(hook):OptId{};const bool hasTarget=hook&&PcSynchedId(hook,6).ok;const bool rod=hook&&ActionNameIs(ActionHeld(c,c.actor),"fishing_rod");
  std::lock_guard<std::mutex> l(s.guard);if(s.phase==2)return;
  if(!hook){if((s.phase==3&&s.phaseOn&&ActionMs(s.phaseAt)>3000.0)||s.phase==1)AutoFishClear(s);return;}
  if(uid!=s.tracked&&uid!=s.old){s.phase=1;s.tracked=uid;s.trackedRuntime=rid;s.known=false;s.bite=false;s.settled=false;const int64_t now=ActionNs();s.calmAt=s.groundAt=now;s.calmOn=s.groundOn=true;}
  if(s.phase!=1||!rod)return;
  bool go=s.bite;if(!go){if(s.cfg.onEntity&&hasTarget)go=true;else go=s.cfg.onGround&&s.groundOn&&(double)s.cfg.groundTime*1000.0<ActionMs(s.groundAt);}
  if(!go)return;s.phase=2;s.old=s.tracked;old=s.old;k=s.cfg;reel=true;}
 if(reel)AutoFishSubmit(k,old);}

// =====================================================================================================================
// Regen
struct RgConfig{bool enabled=false;float range=4.5f;int targetMode=0;bool uncover=true;int uncoverRange=2;int absorptionMode=0;float queueThreshold=0.9f;int absorptionCap=10;
 bool rotate=true,silent=true,ghost=true,swing=true,outline=true,steal=false,othersOn=false;std::vector<PlayerProfile> profiles;};
struct RgSnapshot{int state=0;bool uncover=false;float progress=0;int32_t n[3]{},p[3]{};};
struct RgShared{std::mutex guard;RgConfig cfg;RgSnapshot snap;std::mutex pendGuard;std::vector<uint64_t> dirty;std::vector<BbShared::Hit> hits;volatile long fault=0;
 std::atomic<uint32_t> slotReady{0},seqDone{0},current{0};};
inline RgShared& Regen(){static auto* s=new RgShared;return *s;}
struct RgBreaker{int state=0;bool active=false;uint32_t counter=0,id=0;int32_t P[3]{},N[3]{};uint8_t face=0;int slot=0;float best=0;double clock=0;int64_t wait=0,stall=0,confirm=0;bool gone=false,second=false;};
struct RgState{std::recursive_mutex guard;uint64_t level=0,region=0;std::unordered_map<uint64_t,uint8_t> category;std::unordered_map<uint64_t,BbChunk> chunks;std::vector<uint64_t> active;size_t robin=0;uint64_t gen=0;RgBreaker br;
 bool memory=false;int32_t memoryPos[3]{};bool locked=false;int32_t lockedPos[3]{};
 bool steal=false;int32_t stealOther[3]{},stealPos[3]{};int64_t stealAt=0;bool stealTimed=false;};
inline RgState& RgData(){static auto* s=new RgState;return *s;}
inline bool RgOreName(const std::string& n){return n=="minecraft:redstone_ore"||n=="minecraft:lit_redstone_ore"||n=="minecraft:deepslate_redstone_ore"||n=="minecraft:lit_deepslate_redstone_ore";}
inline bool RgOre(RgState& st,uint64_t block){if(!block)return false;auto f=st.category.find(block);if(f!=st.category.end())return f->second!=0;std::string name;const bool ore=XrayBlockName(block,&name)&&RgOreName(name);
 if(st.category.size()<65536)st.category.emplace(block,ore?1:0);return ore;}
inline void RgClearCaches(RgState& st){st.level=0;st.region=0;st.category.clear();st.chunks.clear();st.active.clear();st.robin=0;}
// Scan of one sub-chunk (layers 0 then 1), as Bed Breaker with the ore test.
inline void RgScanSub(const Context& c,RgState& st,BbSub& sub,uint64_t address,int cx,int cz,int index,int minY){sub.pos.clear();sub.l0=sub.l1=sub.s0=sub.s1=0;
 if(!Read(address+0x30,&sub.l0)||!Read(address+0x38,&sub.l1))return;sub.s0=BbPalette(c,sub.l0);sub.s1=BbPalette(c,sub.l1);
 for(int layer=0;layer<2;++layer){const uint64_t storage=layer?sub.l1:sub.l0;if(!storage)continue;const uint64_t palette=Virtual(c,storage,0xa8),bitsOf=Virtual(c,storage,0xa0),wordsOf=Virtual(c,storage,0x90);if(!palette||!bitsOf||!wordsOf)continue;
  uint64_t out[2]{};reinterpret_cast<void*(*)(uint64_t,uint64_t*)>(palette)(storage,out);if(!out[0]||!out[1])continue;const uint32_t entries=(uint32_t)std::min<uint64_t>(out[0],4096);
  static uint8_t match[4096];bool any=false;for(uint32_t i=0;i<entries;++i){uint64_t b{};match[i]=Read(out[1]+8ull*i,&b)&&RgOre(st,b);any=any||match[i];}if(!any)continue;
  auto add=[&](uint32_t at){sub.pos.push_back({cx*16+(int32_t)(at>>8),minY+16*index+(int32_t)(at&15),cz*16+(int32_t)((at>>4)&15)});};
  const int bits=reinterpret_cast<int(*)(uint64_t)>(bitsOf)(storage);if(bits<=0){if(match[0])for(uint32_t at=0;at<4096;++at)add(at);continue;}if(bits>32)continue;
  uint64_t words[2]{};reinterpret_cast<void*(*)(uint64_t,uint64_t*)>(wordsOf)(storage,words);if(!words[0]||!words[1])continue;const uint32_t per=32/bits,mask=bits==32?0xffffffffu:((1u<<bits)-1);
  for(uint32_t at=0;at<4096;++at){const uint32_t word=at/per;if(word>=words[0])break;uint32_t v{};if(!Read(words[1]+4ull*word,&v))break;const uint32_t pick=(v>>((at%per)*bits))&mask;if(pick<entries&&match[pick])add(at);}}}
// Absorption (attribute 13) full: min(Cap, maximum when > 0) - 0.01 <= current.
inline bool RgAbsorptionFull(const Context& c,int cap){float cur{},top{};if(!PcAttribute(c,13,&cur,&top))return false;const float limit=0.0f<top?std::min((float)cap,top):(float)cap;return limit+-0.01f<=cur;}
// Per-tick destroy progress of a block with `slot` temporarily selected (MC 0x376b010).
inline float RgPerTick(const BbCtx& x,int slot,uint64_t block){int32_t* sel=ActionSelected(x.actor);if(!sel||!block||!x.gm)return 0;const int32_t saved=*sel;*sel=slot;
 const float p=reinterpret_cast<float(*)(uint64_t,uint64_t)>(x.c.base+0x376b010)(x.gm,block);*sel=saved;return p;}
struct RgCost{int slot=-1;int ticks=0;bool ok=false;};
// Break cost: best slot (Item Profiles first, strictly faster than the current slot), ticks = ceil(0.99999 / per tick);
// only valid results are kept for the tick.
inline RgCost RgCostOf(const BbCtx& x,const RgConfig& cfg,std::unordered_map<uint64_t,RgCost>& cache,uint64_t block){auto f=cache.find(block);if(f!=cache.end()&&f->second.ok)return f->second;RgCost r{};
 int32_t* sel=ActionSelected(x.actor);if(sel&&block){const int s=PlayerBestSlot(x.c,x.actor,block,*sel,cfg.profiles);if(s>=0){const float p=RgPerTick(x,s,block);if(std::isfinite(p)&&p>0){r.slot=s;r.ticks=(int)std::ceil(0.99998999f/p);r.ok=true;}}}
 cache[block]=r;return r;}
struct RgPlan{int32_t P[3]{},N[3]{};uint8_t face=0;int slot=0;int cost=0;float dist=0;bool ok=false;};
inline float RgCenterDist(const BbCtx& x,const int32_t* b){return BbDist2(x.eye,b);}
inline bool RgInRange(const BbCtx& x,const int32_t* b){return BbDist2(x.eye,b)<=x.range*x.range;}
// Face f of P faces the eye: (eye - (centre + 0.5 n)) . n > 0.
inline bool RgFaceToward(const BbCtx& x,const int32_t* b,int f){const int* n=BbNormal(f);const float fc[3]={b[0]+.5f+.5f*n[0],b[1]+.5f+.5f*n[1],b[2]+.5f+.5f*n[2]};
 return 0.0f<(x.eye[0]-fc[0])*n[0]+(x.eye[1]-fc[1])*n[1]+(x.eye[2]-fc[2])*n[2];}
// Evaluation of an ore at P with uncover depth L.
inline RgPlan RgEvaluate(RgState& st,const RgConfig& cfg,const BbCtx& x,const int32_t* P,int L,std::unordered_map<uint64_t,RgCost>& costs){RgPlan r{};
 const uint64_t block=BbBlock(x,P);if(!RgOre(st,block)||!BbReach(x,P))return r;const RgCost c0=RgCostOf(x,cfg,costs,block);if(!c0.ok)return r;
 if(!(1.0f<PlayerSpeed(x.c,ActionStackAt(x.c,ActionContainer(x.actor),c0.slot),block,cfg.profiles)))return r;
 uint8_t face{};if(BbFace(x,P,&face)){memcpy(r.P,P,12);memcpy(r.N,P,12);r.face=face;r.slot=c0.slot;r.cost=c0.ticks;r.dist=RgCenterDist(x,P);r.ok=true;return r;}
 if(L<=0)return r;bool have=false;
 for(int f=0;f<6;++f){if(!RgFaceToward(x,P,f))continue;const int* n=BbNormal(f);int cost=c0.ticks;
  for(int k=1;k<=L;++k){const int32_t q[3]={P[0]+n[0]*k,P[1]+n[1]*k,P[2]+n[2]*k};const uint64_t qb=BbBlock(x,q);if(!BbSolid(qb)||!BbReach(x,q))break;const RgCost cq=RgCostOf(x,cfg,costs,qb);if(!cq.ok)break;
   cost+=cq.ticks;uint8_t qf{};if(!BbFace(x,q,&qf))continue;const float d=RgCenterDist(x,q);
   if(!have||(cost!=r.cost?cost<r.cost:d<r.dist)){memcpy(r.P,P,12);memcpy(r.N,q,12);r.face=qf;r.slot=cq.slot;r.cost=cost;r.dist=d;r.ok=true;}have=true;break;}}
 return r;}

// ---- breaker (same machine as Bed Breaker, with the Queue pause)
inline bool RgSame(const int32_t* a,const int32_t* b){return a[0]==b[0]&&a[1]==b[1]&&a[2]==b[2];}
inline bool RgValid(RgState& st,const BbCtx& x){const RgBreaker& b=st.br;const float lim=x.range+.75f;if(BbDist2(x.eye,b.N)>lim*lim||BbSupport(x,b.N))return false;
 return RgSame(b.N,b.P)?RgOre(st,BbBlock(x,b.P)):BbSolid(BbBlock(x,b.N));}
inline bool RgGone(RgState& st,const BbCtx& x){const RgBreaker& b=st.br;return RgSame(b.N,b.P)?!RgOre(st,BbBlock(x,b.P)):!BbSolid(BbBlock(x,b.N));}
inline void RgCall(RgState& st,const BbCtx& x,const std::function<void()>& f){int32_t* sel=ActionSelected(x.actor);uint8_t* breaking=(uint8_t*)(x.actor+0xb4a);if(!sel)return;
 const int32_t saved=*sel;const uint8_t was=*breaking;*sel=st.br.slot;*breaking=1;f();*sel=saved;*breaking=was;}
inline void RgBroken(RgState& st,const BbCtx& x){auto& b=st.br;b.state=4;b.gone=RgGone(st,x);b.second=false;b.confirm=ActionNs();}
inline void RgStart(RgState& st,const RgConfig& cfg,const BbCtx& x){auto& b=st.br;bool done=false;const uint64_t f=Virtual0(x.c,x.gm,0x08);if(!f)return;
 RgCall(st,x,[&]{int32_t at[3]={b.N[0],b.N[1],b.N[2]};reinterpret_cast<bool(*)(uint64_t,int32_t*,uint8_t,bool*)>(f)(x.gm,at,b.face,&done);});
 if(cfg.swing)ActionSwing(x.c,x.actor,2);b.state=2;b.best=0;b.clock=BbGmClock(x.gm);b.stall=ActionNs();if(done)RgBroken(st,x);}
inline void RgContinue(RgState& st,const RgConfig& cfg,const BbCtx& x){auto& b=st.br;bool done=false;const uint64_t f=Virtual0(x.c,x.gm,0x18);if(!f)return;
 RgCall(st,x,[&]{int32_t at[3]={b.N[0],b.N[1],b.N[2]};float eye[3]={x.eye[0],x.eye[1],x.eye[2]};reinterpret_cast<bool(*)(uint64_t,int32_t*,uint8_t,float*,bool*)>(f)(x.gm,at,b.face,eye,&done);});
 if(done)RgBroken(st,x);else if(cfg.swing)ActionSwing(x.c,x.actor,2);}
inline volatile DWORD rgTickThread=0;inline BbCtx* volatile rgCurrentCtx=nullptr;inline BbCtx rgLastCtx{};
inline RgConfig RgCfg(){auto& s=Regen();std::lock_guard<std::mutex> l(s.guard);return s.cfg;}
inline void RgRotate(RgState& st,const RgConfig& cfg,const BbCtx& x,bool act){auto& b=st.br;const int* n=BbNormal(b.face);const float pt[3]={b.N[0]+.5f+.5f*n[0],b.N[1]+.5f+.5f*n[1],b.N[2]+.5f+.5f*n[2]};
 const float dx=pt[0]-x.eye[0],dy=pt[1]-x.eye[1],dz=pt[2]-x.eye[2];RotRequest r;r.module=kRotRegen;r.yaw=std::atan2(-dx,dz)*57.29578f;r.pitch=-std::atan2(dy,std::sqrt(dx*dx+dz*dz))*57.29578f;
 r.mode=cfg.silent?2:1;r.priority=3;const uint32_t id=b.id;
 if(act)r.reached=[id](uintptr_t){if(rgTickThread!=GetCurrentThreadId())return;auto& s=RgData();std::lock_guard<std::recursive_mutex> l(s.guard);if(!s.br.active||s.br.id!=id)return;BbCtx* x=rgCurrentCtx;if(!x)return;
  const RgConfig cfg=RgCfg();if(s.br.state==1)RgStart(s,cfg,*x);else if(s.br.state==2||s.br.state==3)RgContinue(s,cfg,*x);};
 RotSubmit(std::move(r));}
inline void RgAdvance(RgState& st,const RgConfig& cfg,const BbCtx& x){if(cfg.rotate)RgRotate(st,cfg,x,true);else if(st.br.state==1)RgStart(st,cfg,x);else RgContinue(st,cfg,x);}
struct RgStopResult{bool valid=false,destroyed=false;int32_t P[3]{};};
inline RgStopResult RgStop(RgState& st,const BbCtx* x,bool cancel){auto& b=st.br;RgStopResult r{};if(!b.active)return r;const bool wasBreaking=b.state==2||b.state==3;
 if(cancel&&wasBreaking&&x&&x->gm){const uint64_t f=Virtual0(x->c,x->gm,0x20);if(f){int32_t at[3]={b.N[0],b.N[1],b.N[2]};reinterpret_cast<void(*)(uint64_t,int32_t*)>(f)(x->gm,at);}}
 r.valid=true;r.destroyed=!cancel&&RgSame(b.N,b.P);memcpy(r.P,b.P,12);b.state=0;b.active=false;b.id=0;Regen().current=0;RotRelease(kRotRegen);return r;}
// After a stop: a target that was not destroyed is remembered; a destroyed one also clears the Locked target.
inline void RgRemember(RgState& st,const RgStopResult& r){if(!r.valid)return;if(!r.destroyed){memcpy(st.memoryPos,r.P,12);st.memory=true;}else{st.memory=false;st.locked=false;}}
// Queue pause: Absorption Mode Queue, absorption full, and progress + 2 x per tick >= 0.99999 or progress >= Queue Threshold.
inline bool RgPause(RgState& st,const RgConfig& cfg,const BbCtx& x){if(cfg.absorptionMode!=1||!RgAbsorptionFull(x.c,cfg.absorptionCap))return false;
 float progress{};Read(x.gm+0x24,&progress);const float p=RgPerTick(x,st.br.slot,BbBlock(x,st.br.P));return 0.99998999f<=p+p+progress||cfg.queueThreshold<=progress;}
inline void RgBegin(RgState& st,const RgConfig& cfg,const RgPlan& plan){auto& b=st.br;auto& s=Regen();
 if(cfg.targetMode==0){memcpy(st.lockedPos,plan.P,12);st.locked=true;}
 b.id=++b.counter;if(!b.id)b.id=++b.counter;s.current=b.id;memcpy(b.P,plan.P,12);memcpy(b.N,plan.N,12);b.face=plan.face;b.slot=plan.slot;b.state=1;b.active=true;b.wait=ActionNs();b.best=0;
 const uint32_t id=b.id;const int slot=plan.slot;ActionSequence q;q.module=kRegenAction;q.silent=cfg.ghost?1:0;
 q.steps.push_back([slot](ActionCtx& x){ActionSwitch(x,slot);return 1;});if(!cfg.ghost)q.steps.push_back(StepWait(50.0));
 q.steps.push_back([id](ActionCtx&){auto& s=Regen();s.slotReady=id;return s.current.load()==id?1:2;});
 q.steps.push_back([id](ActionCtx&){return Regen().current.load()==id?0:1;});q.steps.push_back(StepSwitchBack());
 q.done=[id](int result){if(result==1)Regen().seqDone=id;};ActionSubmit(std::move(q));}
inline RgStopResult RgBreakerTick(RgState& st,const RgConfig& cfg,const BbCtx& x){auto& b=st.br;auto& s=Regen();if(!b.active)return {};
 if(s.seqDone.load()==b.id)return RgStop(st,&x,true);
 if(b.state==4){const bool hit=b.gone&&RgGone(st,x);if(hit||b.second||ActionMs(b.confirm)>1500.0)return RgStop(st,&x,false);if(cfg.rotate)RgRotate(st,cfg,x,false);return {};}
 if(!RgValid(st,x))return RgStop(st,&x,true);
 if(b.state==2||b.state==3){const double clk=BbGmClock(x.gm);if(clk<=b.clock){const uint64_t f=Virtual0(x.c,x.gm,0x48);if(f)reinterpret_cast<void(*)(uint64_t)>(f)(x.gm);}b.clock=BbGmClock(x.gm);
  if(RgSame(b.N,b.P)&&RgPause(st,cfg,x)){b.state=3;return {};}
  if(b.state==3){b.state=2;b.stall=ActionNs();}
  float progress{};Read(x.gm+0x24,&progress);if(progress>b.best){b.best=progress;b.stall=ActionNs();}else if(ActionMs(b.stall)>1500.0)return RgStop(st,&x,true);
  RgAdvance(st,cfg,x);return {};}
 if(b.state==1){if(ActionMs(b.wait)>1000.0)return RgStop(st,&x,true);if(s.slotReady.load()==b.id)RgAdvance(st,cfg,x);}
 return {};}
inline void RgSnap(RgState& st,const BbCtx* x){auto& s=Regen();RgSnapshot n{};const auto& b=st.br;n.state=b.active?b.state:0;n.uncover=b.active&&!RgSame(b.N,b.P);memcpy(n.n,b.N,12);memcpy(n.p,b.P,12);
 if(n.state==4)n.progress=1.f;else if((n.state==2||n.state==3)&&x&&x->gm){float p{};Read(x->gm+0x24,&p);n.progress=std::clamp(p,0.f,1.f);}
 std::lock_guard<std::mutex> l(s.guard);s.snap=n;}
// Player context: position (+0x240), box (+0x248), region, game mode, Range; walls are not ray-tested by Regen.
inline bool RgContext(const Context& c,const RgConfig& cfg,BbCtx* x){*x=BbCtx{};x->c=c;x->actor=c.actor;Read(c.actor+0xac8,&x->gm);x->region=ActorRegion(c);uint64_t where{},bb{};
 if(!x->region||!x->gm||!Read(c.actor+0x240,&where)||!where||!Floats(where,x->eye,3)||!Read(c.actor+0x248,&bb)||!bb||!Floats(bb,x->box,6)||!Read(x->region+0x3a,&x->minY))return false;
 x->range=cfg.range;x->noWalls=false;return true;}

// ---- LevelTick (also while the module is off, for the cleanup)
inline void RegenWork(){auto& s=Regen();auto& st=RgData();std::lock_guard<std::recursive_mutex> lock(st.guard);const RgConfig cfg=RgCfg();rgCurrentCtx=nullptr;
 Context c{};const bool have=Local(&c);BbCtx x{};const bool ctx=have&&RgContext(c,cfg,&x);
 if(!cfg.enabled){if(!st.br.active&&!st.region){RgSnap(st,nullptr);return;}RgRemember(st,RgStop(st,ctx?&x:nullptr,true));RgClearCaches(st);st.memory=false;st.locked=false;st.steal=false;RgSnap(st,nullptr);return;}
 if(!ctx){RgRemember(st,RgStop(st,nullptr,true));RgClearCaches(st);RgSnap(st,nullptr);return;}
 rgLastCtx=x;rgCurrentCtx=&rgLastCtx;rgTickThread=GetCurrentThreadId();   // kept until the rotation manager pops its stack ("reached" callback)
 uint64_t level{};Read(c.actor+0x200,&level);if(level!=st.level||x.region!=st.region){RgClearCaches(st);st.level=level;st.region=x.region;}
 // Scan, 1 ms budget
 const int64_t t0=ActionNs();auto late=[&]{return ActionMs(t0)>1.0;};++st.gen;const uint64_t chunkAt=Virtual(c,x.region,0x148);
 if(chunkAt){const int fx=(int)std::floor(x.eye[0]),fz=(int)std::floor(x.eye[2]);static const int offs[9][2]={{0,0},{-1,0},{1,0},{0,-1},{0,1},{-1,-1},{-1,1},{1,-1},{1,1}};st.active.clear();
  for(auto& o:offs){int cp[2]={(fx>>4)+o[0],(fz>>4)+o[1]};const uint64_t chunk=reinterpret_cast<uint64_t(*)(uint64_t,int*)>(chunkAt)(x.region,cp);if(!chunk)continue;uint64_t begin{},end{};
   if(!Read(chunk+0x1c8,&begin)||!Read(chunk+0x1d0,&end)||end<begin)continue;const uint32_t count=(uint32_t)std::min<uint64_t>((end-begin)/0x68,64);const uint64_t key=((uint64_t)(uint32_t)cp[0]<<32)|(uint32_t)cp[1];BbChunk& r=st.chunks[key];
   if(r.chunk!=chunk||r.begin!=begin||(r.read&&r.count!=count)){r.chunk=chunk;r.begin=begin;r.count=count;r.read=false;r.x=cp[0];r.z=cp[1];r.subs.assign(count,BbSub{});}r.count=count;if(r.subs.size()!=count)r.subs.resize(count);r.gen=st.gen;st.active.push_back(key);}
  for(auto it=st.chunks.begin();it!=st.chunks.end();)it=it->second.gen==st.gen?std::next(it):st.chunks.erase(it);if(st.robin>=st.active.size())st.robin=0;
  std::vector<uint64_t> dirty;std::vector<BbShared::Hit> hits;{std::lock_guard<std::mutex> l(s.pendGuard);dirty.swap(s.dirty);hits.swap(s.hits);}
  auto scan=[&](BbChunk& r,uint32_t i){RgScanSub(c,st,r.subs[i],r.begin+0x68ull*i,r.x,r.z,(int)i,x.minY);};
  if(!dirty.empty()){std::sort(dirty.begin(),dirty.end());dirty.erase(std::unique(dirty.begin(),dirty.end()),dirty.end());
   for(uint64_t k:st.active){BbChunk& r=st.chunks[k];if(!r.read)continue;for(uint64_t a:dirty)if(a>=r.begin&&a<r.begin+0x68ull*r.count&&(a-r.begin)%0x68==0)scan(r,(uint32_t)((a-r.begin)/0x68));}}
  for(uint64_t k:st.active){if(late())break;BbChunk& r=st.chunks[k];if(r.read)continue;for(uint32_t i=0;i<r.count;++i)scan(r,i);r.read=true;}
  for(size_t turn=0;turn<st.active.size()&&!late();++turn){st.robin=(st.robin+1)%st.active.size();BbChunk& r=st.chunks[st.active[st.robin]];if(!r.read)continue;
   for(uint32_t i=0;i<r.count;++i){const uint64_t a=r.begin+0x68ull*i;uint64_t l0{},l1{};if(!Read(a+0x30,&l0)||!Read(a+0x38,&l1))continue;BbSub& sub=r.subs[i];if(l0!=sub.l0||l1!=sub.l1||BbPalette(c,l0)!=sub.s0||BbPalette(c,l1)!=sub.s1)scan(r,i);}}
  // Break confirmation (SubChunkSetBlock)
  if(st.br.active&&st.br.state==4)for(auto& h:hits){if(h.layer!=0)continue;int cp[2]={st.br.N[0]>>4,st.br.N[2]>>4};const uint64_t chunk=reinterpret_cast<uint64_t(*)(uint64_t,int*)>(chunkAt)(x.region,cp);uint64_t begin{};
   if(!chunk||!Read(chunk+0x1c8,&begin))continue;const int i=(st.br.N[1]-x.minY)>>4;if(i<0||h.sub!=begin+0x68ull*i)continue;
   if(h.index!=(uint16_t)((st.br.N[1]&15)|((st.br.N[2]&15)<<4)|((st.br.N[0]&15)<<8)))continue;if(!st.br.gone)st.br.gone=true;else st.br.second=true;}}
 // Pauses
 {float h{};const bool alive=PlayerHealth(c,&h)&&0.0f<h;uint8_t breaking{};Read(c.actor+0xb4a,&breaking);int32_t gameType=-1;uintptr_t type{};if(Element(c.registry,c.entity,"ActorGameTypeComponent",4,&type))Read(type,&gameType);
  if(!StickyHasInput(c)||breaking||gameType==1||gameType==6||!alive||cfg.othersOn){RgRemember(st,RgStop(st,&x,true));RgSnap(st,&x);return;}}
 // Target choice
 if(!st.br.active){
  if(cfg.absorptionMode==0&&RgAbsorptionFull(c,cfg.absorptionCap)){RgSnap(st,&x);return;}
  if(ActionCursorBusy(c,c.actor)){RgSnap(st,&x);return;}
  std::unordered_map<uint64_t,RgCost> costs;const int L=cfg.uncover?cfg.uncoverRange:0;RgPlan best{};
  if(st.steal){if(st.stealTimed&&ActionMs(st.stealAt)>5000.0)st.steal=false;else best=RgEvaluate(st,cfg,x,st.stealPos,L,costs);}
  if(!best.ok){
   if(cfg.targetMode==0){if(st.locked){BbCtx wide=x;wide.range=x.range+0.75f;best=RgEvaluate(st,cfg,wide,st.lockedPos,L,costs);if(!best.ok)st.locked=false;}}else st.locked=false;
   if(!best.ok&&st.memory)best=RgEvaluate(st,cfg,x,st.memoryPos,L,costs);
   if(!best.ok){std::vector<std::array<int32_t,3>> cands;for(auto& e:st.chunks)for(auto& sub:e.second.subs)for(auto& p:sub.pos)if(RgInRange(x,p.data()))cands.push_back(p);
    const int passes[2]={0,L};for(int k=0;k<2;++k){const int depth=passes[k];for(auto& p:cands){const RgPlan r=RgEvaluate(st,cfg,x,p.data(),depth,costs);if(!r.ok)continue;
      if(!best.ok||(r.cost!=best.cost?r.cost<best.cost:r.dist<best.dist))best=r;}
     if(best.ok||depth==L)break;}}}
  st.memory=false;if(!best.ok){RgSnap(st,&x);return;}RgBegin(st,cfg,best);}
 // Breaker tick (a running break is cancelled while full in Stop mode)
 RgStopResult result{};if(cfg.absorptionMode!=0||st.br.state==4||!RgAbsorptionFull(c,cfg.absorptionCap))result=RgBreakerTick(st,cfg,x);else result=RgStop(st,&x,true);
 RgRemember(st,result);RgSnap(st,&x);}
// LevelEventPacket 0x19 (Steal): 3600 next to a hidden ore within Range remembers it; 3601 at the same block forgets it.
inline void RegenReceiveWork(uintptr_t packet){if(PcPacketId(packet)!=0x19)return;const RgConfig cfg=RgCfg();if(!cfg.enabled||!cfg.steal)return;int32_t ev{};float f[3];
 if(!Read(packet+0x30,&ev)||!Read(packet+0x34,&f[0])||!Read(packet+0x38,&f[1])||!Read(packet+0x3c,&f[2]))return;const int32_t pos[3]={(int32_t)floorf(f[0]),(int32_t)floorf(f[1]),(int32_t)floorf(f[2])};
 auto& st=RgData();std::lock_guard<std::recursive_mutex> lock(st.guard);
 if(ev==3601){if(st.steal&&RgSame(st.stealOther,pos))st.steal=false;return;}if(ev!=3600)return;
 if(st.br.active&&RgSame(st.br.N,pos))return;Context c{};BbCtx x{};if(!Local(&c)||!RgContext(c,cfg,&x))return;
 for(int k=0;k<6;++k){const int* n=BbNormal(k);const int32_t q[3]={pos[0]+n[0],pos[1]+n[1],pos[2]+n[2]};if(!RgInRange(x,q)||!RgOre(st,BbBlock(x,q)))continue;uint8_t face{};if(BbFace(x,q,&face))continue;
  memcpy(st.stealOther,pos,12);memcpy(st.stealPos,q,12);st.stealAt=ActionNs();st.stealTimed=true;st.steal=true;return;}}
inline void RegenSetBlock(uint64_t sub,uint8_t layer,uint16_t index){auto& s=Regen();if(s.fault)return;{std::lock_guard<std::mutex> l(s.guard);if(!s.cfg.enabled)return;}
 std::lock_guard<std::mutex> l(s.pendGuard);if(s.dirty.size()<4096)s.dirty.push_back(sub);if(s.hits.size()<4096)s.hits.push_back({sub,layer,index});}
// Module list suffix: Mining / Uncovering (states 1, 2, 4), Queued (state 3).
inline std::string RegenLabel(const RgSnapshot& n){if(n.state==0)return {};if(n.state==3)return "Queued";return n.uncover?"Uncovering":"Mining";}

// =====================================================================================================================
// Entry points (guarded)
inline void(*volatile playerCLog)(const char*)=nullptr;
inline void PlayerCFault(volatile long* fault,const char* text){if(!InterlockedExchange(fault,1)&&playerCLog)playerCLog(text);}
// LevelTick (UtilityTick): Pearl Aimbot Snap, Auto Fish detection, Regen.
inline void PlayerCTick(){
 if(!PearlAim().fault){__try{PearlAimTickWork();}__except(EXCEPTION_EXECUTE_HANDLER){PlayerCFault(&PearlAim().fault,"Pearl Aimbot: fault caught, module stopped until restart");}}
 if(!AutoFish().fault){__try{AutoFishTickWork();}__except(EXCEPTION_EXECUTE_HANDLER){PlayerCFault(&AutoFish().fault,"Auto Fish: fault caught, module stopped until restart");}}
 if(!Regen().fault){__try{RegenWork();}__except(EXCEPTION_EXECUTE_HANDLER){PlayerCFault(&Regen().fault,"Regen: fault caught, module stopped until restart");}}}
// GameUpdate (CombatGameUpdates): Pearl Aimbot Track, Faster Inventory, Auto Fish decision.
inline void PlayerCGameUpdate(){
 if(!PearlAim().fault){__try{PearlAimUpdateWork();}__except(EXCEPTION_EXECUTE_HANDLER){PlayerCFault(&PearlAim().fault,"Pearl Aimbot: fault caught, module stopped until restart");}}
 if(!FasterInv().fault){uint64_t* lent=ItemRegistryLendBegin();   // the locally handled ContainerOpenPacket builds item descriptors
  __try{FasterInvUpdateWork();}__except(EXCEPTION_EXECUTE_HANDLER){fasterInvLocalReceive=false;PlayerCFault(&FasterInv().fault,"Faster Inventory: fault caught, module stopped until restart");}ItemRegistryLendEnd(lent);}
 if(!AutoFish().fault){__try{AutoFishUpdateWork();}__except(EXCEPTION_EXECUTE_HANDLER){PlayerCFault(&AutoFish().fault,"Auto Fish: fault caught, module stopped until restart");}}}
// PacketSend (UtilitySend, never cancels).
inline void PlayerCSend(uintptr_t packet){
 if(!PearlAim().fault){__try{PearlAimSendWork(packet);}__except(EXCEPTION_EXECUTE_HANDLER){PlayerCFault(&PearlAim().fault,"Pearl Aimbot: fault caught, module stopped until restart");}}
 if(!FasterInv().fault){__try{FasterInvSendWork(packet);}__except(EXCEPTION_EXECUTE_HANDLER){PlayerCFault(&FasterInv().fault,"Faster Inventory: fault caught, module stopped until restart");}}}
// PacketReceive (UtilityReceiveWork): true = packet not handled (Faster Inventory absorption only).
inline bool PlayerCReceive(uintptr_t packet){bool cancel=false;
 if(!PearlAim().fault){__try{PearlAimReceiveWork(packet);}__except(EXCEPTION_EXECUTE_HANDLER){PlayerCFault(&PearlAim().fault,"Pearl Aimbot: fault caught, module stopped until restart");}}
 if(!AutoFish().fault){__try{AutoFishReceiveWork(packet);}__except(EXCEPTION_EXECUTE_HANDLER){PlayerCFault(&AutoFish().fault,"Auto Fish: fault caught, module stopped until restart");}}
 if(!Regen().fault){__try{RegenReceiveWork(packet);}__except(EXCEPTION_EXECUTE_HANDLER){PlayerCFault(&Regen().fault,"Regen: fault caught, module stopped until restart");}}
 if(!FasterInv().fault){__try{cancel=FasterInvReceiveWork(packet);}__except(EXCEPTION_EXECUTE_HANDLER){PlayerCFault(&FasterInv().fault,"Faster Inventory: fault caught, module stopped until restart");}}
 return cancel;}
}
