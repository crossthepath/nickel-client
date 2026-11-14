// Backtrack: delays the incoming movement of the entity the player attacks, so that its displayed hitbox stays closer
// than its real position.
// - Target: an outgoing InventoryTransaction (#0x1e, transaction at packet+0x170: type +8 == 3 (use on entity), action
//   +0x70 == 1 (attack)) selects the runtime id at transaction+0x68 and stamps the attack time. A target not attacked
//   for 30 s, or no longer present, is dropped (held packets released). Changing target releases and clears the
//   records (always in Freeze, only with Target Required in Smooth); the remembered real position is forgotten.
//   While packets are held, outgoing Interact (#0x21) packets with action 4 (mouse over) are cancelled. In Freeze, an
//   attack on a target whose MobHurtTimeComponent is zero (or missing) releases everything and starts the cooldown.
// - Held packets: incoming MoveActorAbsolute (#0x12, runtime id +0x30, position +0x3c) and MovePlayer (#0x13, runtime id
//   +0x30, position +0x38) are copied (shared_ptr reference) into a FIFO with the hold time and dropped from the receive
//   hook; later they are replayed through the local receive path (ReceivePacket) from the GameUpdate flush, like the
//   outgoing queue of Blink / Fake Lag. Connection packets (LagConnectionPacket) clear everything.
// - Smooth: every packet of an entity that passes the filter (Target Required: only the attacked entity; Players Only:
//   players) is held when the entity's displayed position is closer to the local player than the new position,
//   otherwise everything held is released and the packet passes. Other entity packets (see BacktrackClassA/B) are held
//   in order while the entity is held (not with Positions Only). Held packets older than Delay ms are released on every
//   GameUpdate (release up to now - Delay).
// - Freeze: only the target. A new position closer to the local player than the displayed one releases everything and
//   starts the Cooldown; otherwise holding starts when no cooldown runs, a previous position is known, the step is at
//   least 0.05 blocks and the old position is closer than the new one; once holding, every target move packet is held,
//   and so is every other incoming packet (in order).
//   GameUpdate releases everything when the real position is not farther than the displayed one, or when the oldest
//   held packet is older than Delay ms (then the cooldown starts).
// - Indicator: a wireframe box (actor box size) at the last received real position of each record that is held or
//   younger than Delay + 250 ms, in Indicator Color.
// - Game offsets: actor+0x240 position state (x, y, z), actor+0x248 box.
#pragma once
#include "game_combat.hpp"
#include "game_lag.hpp"
#include "game_packets.hpp"
#include "game_world.hpp"
#include <atomic>
#include <cmath>
#include <cstring>
#include <deque>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace game {
struct BacktrackRecord{float pos[3]{};int64_t at=0;};
struct BacktrackHeld{GamePacket packet;int id=0;int64_t at=0;bool released=false;};
struct BacktrackShared{
 std::atomic<bool> enabled{false},resetPending{false},playersOnly{true},targetRequired{true},positionsOnly{false},indicator{true},fault{false};
 std::atomic<int> mode{0},delay{50};std::atomic<float> cooldown{0.5f};float color[4]{1.f,0.35f,0.35f,0.25f};   // guarded by 'guard'
 
 std::recursive_mutex guard;
 bool hasTarget=false,hasPos=false,hasAttack=false,hasCool=false;uint64_t target=0;float pos[3]{};int64_t attackAt=0,coolAt=0;
 std::unordered_map<uint64_t,BacktrackRecord> records;std::unordered_set<uint64_t> held;std::deque<BacktrackHeld> queue;bool replaying=false;};
inline BacktrackShared& Backtrack(){static auto* s=new BacktrackShared;return *s;}

// ---- packet classes
inline bool BacktrackClassA(int id){   // packets held in order while an entity is held
 if(id==0x10||id==0xa1||id==0x13e||id==0x142)return true;
 const unsigned a=(unsigned)id-0x97u,b=(unsigned)id-0x3au;
 return (a<=0x25&&((0x3000a00001ull>>a)&1))||(b<=0x34&&((0x10000000000011ull>>b)&1))||id==0x15;}
inline bool BacktrackClassB(int id){   // entity packets starting with a runtime id
 return (id<=0x28&&((0x180300c0000ull>>id)&1))||id==0x6f||id==0x9d;}
inline int BacktrackId(uintptr_t packet){uint64_t t{},g{};return Read(packet,&t)&&t&&Read(t+8,&g)&&g?reinterpret_cast<int(*)(uintptr_t)>(g)(packet):-1;}

// ---- queue
inline bool BacktrackAnyHeld(BacktrackShared& s){for(auto& h:s.queue)if(!h.released)return true;return false;}
inline void BacktrackReleaseUpTo(BacktrackShared& s,int64_t limit){for(auto& h:s.queue)if(h.at<=limit)h.released=true;if(!BacktrackAnyHeld(s))s.held.clear();}
inline void BacktrackReleaseAll(BacktrackShared& s){BacktrackReleaseUpTo(s,INT64_MAX);}
inline void BacktrackClear(BacktrackShared& s){s.records.clear();s.held.clear();}   // lists only (held packets stay queued)
inline void BacktrackReset(BacktrackShared& s){BacktrackReleaseAll(s);BacktrackClear(s);s.hasTarget=s.hasPos=s.hasAttack=s.hasCool=false;}
inline void BacktrackReleaseCooldown(BacktrackShared& s){BacktrackReleaseAll(s);s.coolAt=LagNs();s.hasCool=true;}
inline bool BacktrackOldest(BacktrackShared& s,int64_t* at){bool any=false;for(auto& h:s.queue)if(!h.released&&(!any||h.at<*at)){*at=h.at;any=true;}return any;}
// Replays released packets from the head (local receive path), like the outgoing resend of the lag queue.
inline void BacktrackFlushWork(){auto& s=Backtrack();std::lock_guard<std::recursive_mutex> lock(s.guard);Context c{};
 if(!Client(&c)){for(auto& h:s.queue)ReleasePacket(h.packet);s.queue.clear();return;}
 while(!s.queue.empty()&&s.queue.front().released){BacktrackHeld h=std::move(s.queue.front());s.queue.pop_front();
  s.replaying=true;const bool ok=ReceivePacket(c,&h.packet);s.replaying=false;ReleasePacket(h.packet);
  if(!ok){for(auto& r:s.queue)ReleasePacket(r.packet);s.queue.clear();s.held.clear();break;}}}
// Copies the receive hook's shared_ptr<Packet>& into the queue; the caller drops the packet.
inline bool BacktrackHold(BacktrackShared& s,void* shared,int id){uint64_t object{},control{};if(!Read((uintptr_t)shared,&object)||!Read((uintptr_t)shared+8,&control)||!object||!control)return false;
 InterlockedIncrement(reinterpret_cast<volatile long*>(control)+2);BacktrackHeld h;h.packet.object=object;h.packet.control=control;h.id=id;h.at=LagNs();s.queue.push_back(std::move(h));return true;}

// ---- helpers
inline float BacktrackDist2(const float* a,const float* b){const float x=a[0]-b[0],y=a[1]-b[1],z=a[2]-b[2];return x*x+y*y+z*z;}
inline bool BacktrackActor(const Context& c,uint64_t rid,uintptr_t* actor){return ActorByRuntimeId(c.registry,rid,actor)&&*actor;}
inline bool BacktrackIsPlayer(uintptr_t actor){uintptr_t registry{};uint32_t entity{};return ActorEntity(actor,&registry,&entity)&&Has(registry,entity,"PlayerComponent");}
// Does this entity take part (Target Required: the attacked one; Players Only).
inline bool BacktrackTracked(BacktrackShared& s,uint64_t rid,uintptr_t actor){
 if(s.targetRequired&&(!s.hasTarget||s.target!=rid))return false;
 if(s.playersOnly)return actor&&BacktrackIsPlayer(actor);return true;}
inline bool BacktrackCooldown(BacktrackShared& s){return s.hasCool&&(double)(LagNs()-s.coolAt)/1e6<=(double)s.cooldown.load()*1000.0;}
// New position closer than min step and the old one closer than the new one (to the local player).
inline bool BacktrackStep(const float* old,const float* now,const float* local){return 0.002500000176951289f<=BacktrackDist2(now,old)&&BacktrackDist2(old,local)<BacktrackDist2(now,local);}
// Move packet fields: runtime id and position.
inline bool BacktrackMove(uintptr_t packet,int id,uint64_t* rid,float* pos){if(id!=0x12&&id!=0x13)return false;return Read(packet+0x30,rid)&&Floats(packet+(id==0x13?0x38:0x3c),pos,3);}

// ---- incoming packets (receive hook, shared_ptr<Packet>&); returns true when the packet must not be handled now
inline bool BacktrackReceiveWork(void* shared){auto& s=Backtrack();if(!s.enabled)return false;uint64_t object{};if(!Read((uintptr_t)shared,&object)||!object)return false;
 std::lock_guard<std::recursive_mutex> lock(s.guard);if(s.replaying)return false;const int id=BacktrackId(object);if(id<0)return false;
 if(LagConnectionPacket(id)){BacktrackReset(s);return false;}
 Context c{};float local[3]{};if(!Local(&c)||!ActorPos(c.actor,local)){BacktrackReset(s);return false;}
 const bool held=BacktrackAnyHeld(s);
 if(s.mode.load()==1){   // Freeze
  do{   // 'break' = the packet is not a movement of the target: it is only held in order when packets are already held
   if(!s.hasTarget||(id!=0x12&&id!=0x13))break;   // MoveActorDelta (0x6f): layout not ported, handled like any other packet
   uint64_t rid{};float np[3]{};if(!BacktrackMove(object,id,&rid,np)||rid!=s.target)break;
   uintptr_t actor{};if(!BacktrackActor(c,rid,&actor)||(s.playersOnly&&!BacktrackIsPlayer(actor)))break;
   const bool hadPos=s.hasPos;float old[3]{};if(hadPos)memcpy(old,s.pos,12);memcpy(s.pos,np,12);s.hasPos=true;
   auto& rec=s.records[rid];memcpy(rec.pos,np,12);rec.at=LagNs();
   float shown[3]{};if(!ActorPos(actor,shown))break;
   if(BacktrackDist2(np,local)<=BacktrackDist2(shown,local)){if(held)BacktrackReleaseCooldown(s);return false;}
   if(!held){if(!hadPos||BacktrackCooldown(s)||!BacktrackStep(old,np,local))return false;}
   if(!BacktrackHold(s,shared,id))return false;s.held.insert(rid);return true;
  }while(false);
  return held&&BacktrackHold(s,shared,id);}
 // Smooth
 if(id!=0x73){
  bool anyHeld=held;
  if(!BacktrackClassA(id)){
   if(!BacktrackClassB(id))return false;uint64_t rid{};float np[3]{};if(!Read(object+0x30,&rid))return false;uint64_t own{};
   if(!(RuntimeId(c.actor,&own)&&own==rid)){
    uintptr_t actor{};const bool found=BacktrackActor(c,rid,&actor);const bool inHeld=s.held.count(rid)!=0;const bool tracked=BacktrackTracked(s,rid,found?actor:0);
    if(!tracked){s.records.erase(rid);if(!inHeld)return false;}
    if(id==0x12||id==0x13){
     const bool valid=BacktrackMove(object,id,&rid,np);
     if(!tracked||!found||!valid){if(!inHeld)return false;BacktrackReleaseAll(s);return false;}
     auto& rec=s.records[rid];memcpy(rec.pos,np,12);rec.at=LagNs();
     float shown[3]{};if(!ActorPos(actor,shown)||!(BacktrackDist2(shown,local)<BacktrackDist2(np,local))){if(!inHeld)return false;BacktrackReleaseAll(s);return false;}
     s.held.insert(rid);return BacktrackHold(s,shared,id);}
    anyHeld=inHeld;}}
  if(s.positionsOnly||!anyHeld)return false;return BacktrackHold(s,shared,id);}
 if(!held)return false;return BacktrackHold(s,shared,id);}
inline bool BacktrackReceive(void* shared){auto& s=Backtrack();if(s.fault)return false;__try{return BacktrackReceiveWork(shared);}__except(EXCEPTION_EXECUTE_HANDLER){s.fault=true;return false;}}

// ---- outgoing packets: target selection; returns true to cancel the send
inline bool BacktrackSendWork(uintptr_t packet){auto& s=Backtrack();if(!s.enabled)return false;std::lock_guard<std::recursive_mutex> lock(s.guard);const int id=BacktrackId(packet);
 uint64_t tx{};uint64_t rid{};bool attack=false;
 if(id==0x1e&&Read(packet+0x170,&tx)&&tx){int32_t type{},action{};attack=Read(tx+8,&type)&&type==3&&Read(tx+0x70,&action)&&action==1&&Read(tx+0x68,&rid);}
 if(!attack){if(!BacktrackAnyHeld(s)||id!=0x21)return false;uint8_t action{};return Read(packet+0x30,&action)&&action==4;}
 Context c{};const int mode=s.mode.load();
 if(mode==1&&Local(&c)&&BacktrackAnyHeld(s)){uintptr_t actor{},registry{},element{};uint32_t entity{};int32_t hurt=0;
  if(BacktrackActor(c,rid,&actor)){const bool has=ActorEntity(actor,&registry,&entity)&&Element(registry,entity,"MobHurtTimeComponent",4,&element)&&Read(element,&hurt);
   if(!has||hurt==0)BacktrackReleaseCooldown(s);}}
 const bool changed=!s.hasTarget||s.target!=rid;
 if(changed){if(mode==1||s.targetRequired){BacktrackReleaseAll(s);BacktrackClear(s);}s.hasPos=false;}
 s.target=rid;s.hasTarget=true;s.attackAt=LagNs();s.hasAttack=true;return false;}
inline bool BacktrackSend(uintptr_t packet){auto& s=Backtrack();if(s.fault)return false;__try{return BacktrackSendWork(packet);}__except(EXCEPTION_EXECUTE_HANDLER){s.fault=true;return false;}}

// ---- GameUpdate
inline void BacktrackTickWork(){auto& s=Backtrack();if(s.resetPending.exchange(false)){std::lock_guard<std::recursive_mutex> lock(s.guard);BacktrackReset(s);}
 if(!s.enabled&&s.queue.empty())return;std::lock_guard<std::recursive_mutex> lock(s.guard);
 Context c{};float local[3]{};if(!s.enabled||!Local(&c)||!ActorPos(c.actor,local)){BacktrackReset(s);BacktrackFlushWork();return;}
 uintptr_t actor{};bool found=false;
 if(s.hasTarget){const bool fresh=s.hasAttack&&(double)(LagNs()-s.attackAt)/1e6<=30000.0;found=BacktrackActor(c,s.target,&actor);
  if(!fresh||!found){BacktrackReleaseAll(s);s.hasTarget=s.hasPos=s.hasAttack=false;BacktrackFlushWork();return;}}
 for(auto it=s.records.begin();it!=s.records.end();){uintptr_t a{};if(BacktrackActor(c,it->first,&a))++it;else it=s.records.erase(it);}
 for(uint64_t rid:s.held){uintptr_t a{};if(!BacktrackActor(c,rid,&a)){BacktrackReleaseAll(s);BacktrackFlushWork();return;}}
 int64_t oldest{};if(!BacktrackOldest(s,&oldest)){s.held.clear();BacktrackFlushWork();return;}
 const int mode=s.mode.load();float shown[3]{};
 if(found&&mode==1&&s.hasPos&&ActorPos(actor,shown)&&BacktrackDist2(s.pos,local)<=BacktrackDist2(shown,local)){BacktrackReleaseCooldown(s);BacktrackFlushWork();return;}
 const int64_t limit=LagNs()-(int64_t)s.delay.load()*1000000;
 if(mode==0)BacktrackReleaseUpTo(s,limit);else if(oldest<=limit)BacktrackReleaseCooldown(s);
 BacktrackFlushWork();}
inline void BacktrackTick(){auto& s=Backtrack();if(s.fault)return;__try{BacktrackTickWork();}__except(EXCEPTION_EXECUTE_HANDLER){s.fault=true;}}

// ---- indicator: wireframe box (actor box size) at the last real position of held / recent records
inline void BacktrackLinesWork(const float*,std::vector<WorldLine>* out){auto& s=Backtrack();if(!s.enabled||!s.indicator)return;Context c{};if(!Local(&c))return;
 std::lock_guard<std::recursive_mutex> lock(s.guard);const int64_t now=LagNs();const double window=(double)s.delay.load()+250.0;float color[4];memcpy(color,s.color,16);
 for(auto& kv:s.records){if(!s.held.count(kv.first)&&(double)(now-kv.second.at)/1e6>window)continue;uintptr_t actor{};if(!BacktrackActor(c,kv.first,&actor))continue;
  float feet[3],cur[3],size[6];uint64_t bb{};if(!ActorFeet(actor,feet)||!ActorPos(actor,cur)||!Read(actor+0x248,&bb)||!bb||!Floats(bb,size,6))continue;
  const float hx=(size[3]-size[0])*0.5f,hz=(size[5]-size[2])*0.5f,h=size[4]-size[1],base=kv.second.pos[1]-(cur[1]-feet[1]);
  const float lo[3]={kv.second.pos[0]-hx,base,kv.second.pos[2]-hz},hi[3]={kv.second.pos[0]+hx,base+h,kv.second.pos[2]+hz};
  static const int pick[8][3]={{0,0,0},{1,0,0},{1,0,1},{0,0,1},{0,1,0},{1,1,0},{1,1,1},{0,1,1}};static const int edges[12][2]={{0,1},{1,2},{2,3},{3,0},{4,5},{5,6},{6,7},{7,4},{0,4},{1,5},{2,6},{3,7}};
  for(auto& e:edges){WorldLine line{};for(int j=0;j<3;++j){line.a[j]=pick[e[0]][j]?hi[j]:lo[j];line.b[j]=pick[e[1]][j]?hi[j]:lo[j];}memcpy(line.colorA,color,16);memcpy(line.colorB,color,16);line.thickness=1.5f;line.depth=true;out->push_back(line);}}}
inline bool BacktrackLines(const float* camera,std::vector<WorldLine>& out){__try{BacktrackLinesWork(camera,&out);return true;}__except(EXCEPTION_EXECUTE_HANDLER){return false;}}
}
