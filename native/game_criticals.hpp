// Criticals (Combat, PacketSend listener): makes attacks land as critical hits by crafting or editing
// PlayerAuthInputPackets (0x90).
// - Conditions: game type not Spectator (ActorGameTypeComponent != 6), weapon in hand with Weapons Only, and the head
// not in water (ActorHeadInWaterFlagComponent absent).
// - Packet: every auth input sent is copied (same field copy as Insta Break, input bits cleared). On an attack
// transaction (InventoryTransaction 0x1e, data +0x170: type 3, action 1) not sent from inside another send, once per
// auth input, two auth inputs are rebuilt from the copy and sent first: y + 0.3 with delta (0, 0.3, 0), then y + 0.2
// with delta (0, -0.1 rounded down one ulp, 0). If the player was sprinting, both carry the "stop sprinting" input bits and the next real
// auth input restores sprinting.
// - Levitate / Sprint: on each auth input, if the conditions hold (and, with Target Required, an entity is under the
// crosshair): packet +8 = 0x100000000; Levitate adds 0.2002 to y (+0x3c), then 0.2001 on the following packets;
// while falling (FallDistanceComponent > 0) the input bits say "stop sprinting" and the next packet restores them.
// Input bits (packet +0x88): 4 sprint down, 0x14 sprinting, 0x19 start sprinting, 0x1a stop sprinting.
// The original packet is never cancelled.
// - Jump (Nickel): servers that check criticals against their own view of the player (a ground probe of up to 0.5
// block, or a server-side simulation) never see the offsets above. On the ground, with an entity under the crosshair
// or (without Target Required) a valid target within 3.5 blocks, a jump is requested from GameUpdate; attacks that land
// while the player falls are real critical hits on every server.
#pragma once
#include <atomic>
#include <cstring>
#include <mutex>
#include "game_combat.hpp"

namespace game {
struct CriticalsConfig{int mode=0;bool weaponsOnly=false,targetRequired=false;};   // mode: 0 Packet, 1 Levitate, 2 Sprint, 3 Jump
struct CriticalsShared{std::mutex guard;std::atomic<bool> enabled{false},resetRequest{false};CriticalsConfig cfg;
 // Send-path state (game thread only): last auth input copy and its presence, crafted packets being sent, crafted
 // packets already sent for this auth input, Levitate phase, sprint bits to restore.
 uint8_t snapshot[0x88]{};bool has=false,sent=false,phase=false,restore=false;std::atomic<bool> sending{false};};
inline CriticalsShared& Criticals(){static auto* s=new CriticalsShared;return *s;}
// Activation and deactivation: the state is cleared before the next packet is handled.
inline void CriticalsActivate(CriticalsShared& s){s.resetRequest=true;}
inline void CriticalsReset(CriticalsShared& s){s.resetRequest=true;}
inline void CriticalsClear(CriticalsShared& s){memset(s.snapshot,0,sizeof(s.snapshot));s.has=false;s.sent=false;s.phase=false;s.restore=false;s.sending=false;}

// Sets or clears one input bit of a PlayerAuthInputPacket (bits below 0x42, words at +0x88).
inline void AuthInputBit(uintptr_t packet,uint32_t bit,bool on){if(bit>=0x42)return;uint64_t* word=reinterpret_cast<uint64_t*>(packet+0x88+(bit>>6)*8);const uint64_t mask=1ull<<(bit&63);if(on)*word|=mask;else *word&=~mask;}
inline void AuthInputStopSprint(uintptr_t packet){AuthInputBit(packet,0x14,false);AuthInputBit(packet,4,false);AuthInputBit(packet,0x19,false);AuthInputBit(packet,0x1a,true);}
inline void AuthInputResumeSprint(uintptr_t packet){AuthInputBit(packet,0x14,true);AuthInputBit(packet,0x19,true);AuthInputBit(packet,0x1a,false);}
// Sprinting: actor flag 3 of ActorDataFlagComponent (0x18-byte bitset).
inline bool ActorSprinting(uintptr_t actor){uintptr_t registry{},element{};uint32_t entity{};uint64_t bits{};return ActorEntity(actor,&registry,&entity)&&Element(registry,entity,"ActorDataFlagComponent",0x18,&element)&&Read(element,&bits)&&((bits>>3)&1);}
// Fall distance: first float of FallDistanceComponent (8-byte element); false without the component.
inline bool ActorFallDistance(uintptr_t actor,float* fall){uintptr_t registry{},element{};uint32_t entity{};return ActorEntity(actor,&registry,&entity)&&Element(registry,entity,"FallDistanceComponent",8,&element)&&Read(element,fall);}
// Common conditions: not Spectator, weapon in hand with Weapons Only, head not in water.
inline bool CriticalsAllowed(const Context& c,const CriticalsConfig& k){uintptr_t registry{},element{};uint32_t entity{};if(!ActorEntity(c.actor,&registry,&entity))return false;
 int32_t gameType=-1;if(Element(registry,entity,"ActorGameTypeComponent",4,&element))Read(element,&gameType);if(gameType==6)return false;
 if(k.weaponsOnly&&!IsWeapon(c))return false;return !Has(registry,entity,"ActorHeadInWaterFlagComponent");}
// One crafted auth input: rebuilt from the copy, y raised by `lift`, delta written, sprint bits cleared if needed.
inline void CriticalsCraftOne(const Context& c,const uint8_t* copy,float lift,const float* delta,bool sprinting){GamePacket pk=CreatePacket(c,0x90);
 if(pk.object){const uintptr_t p=pk.object;InstaRebuild(p,copy);float x{},y{},z{};memcpy(&x,copy+8,4);memcpy(&y,copy+0xc,4);memcpy(&z,copy+0x10,4);
  *reinterpret_cast<float*>(p+0x38)=x+0.0f;*reinterpret_cast<float*>(p+0x3c)=lift+y;*reinterpret_cast<float*>(p+0x40)=z+0.0f;memcpy(reinterpret_cast<void*>(p+0x48),delta,12);
  if(sprinting)AuthInputStopSprint(p);SendPacket(c,p);}
 ReleasePacket(pk);}
// Packet mode: the two crafted auth inputs sent ahead of the attack.
inline void CriticalsCraft(CriticalsShared& s,const Context& c){const bool sprinting=ActorSprinting(c.actor);uint8_t copy[0x88];memcpy(copy,s.snapshot,sizeof(copy));s.sending=true;
 static const float first[3]={0.0f,0.3f,0.0f},second[3]={0.0f,-0.10000000894069672f,0.0f};   // 0xbdccccce, one ulp below -0.1f
 CriticalsCraftOne(c,copy,0.3f,first,sprinting);CriticalsCraftOne(c,copy,0.2f,second,sprinting);
 s.sending=false;s.sent=true;s.restore=s.restore||sprinting;}
// Restores sprinting on this auth input if a previous packet stopped it and the player still sprints.
inline void CriticalsRestore(CriticalsShared& s,const Context& c,uintptr_t packet){if(s.restore&&ActorSprinting(c.actor))AuthInputResumeSprint(packet);s.restore=false;}
// PacketSend listener; `nested` = this send happens inside another send. Returns whether to cancel (never).
// Jump mode (GameUpdate): one jump request while on the ground with a target close enough.
inline void CriticalsJumpWork(){auto& s=Criticals();if(!s.enabled)return;CriticalsConfig k;{std::lock_guard<std::mutex> lock(s.guard);k=s.cfg;}if(k.mode!=3)return;
 Context c{};if(!Local(&c)||!CombatNoInterface()||!ActionOnGround(c.actor)||!CriticalsAllowed(c,k))return;bool target=CrosshairActor(c)!=0;
 if(!target&&!k.targetRequired){float me[3]{};if(ActorFeet(c.actor,me)){CombatTargetFilter f;ForEachActor(c,[&](uintptr_t a){float p[3]{};if(!target&&CombatTargetValid(c,a,false,true,f)&&ActorFeet(a,p)){const float dx=p[0]-me[0],dy=p[1]-me[1],dz=p[2]-me[2];if(dx*dx+dy*dy+dz*dz<=3.5f*3.5f)target=true;}return !target;});}}
 if(target)JumpRequest(c.actor);}
inline bool CriticalsSend(uintptr_t packet,bool nested){auto& s=Criticals();if(!s.enabled)return false;if(s.resetRequest.exchange(false))CriticalsClear(s);
 if(s.sending)return false;Context c{};if(!Local(&c))return false;CriticalsConfig k;{std::lock_guard<std::mutex> lock(s.guard);k=s.cfg;}
 const int id=PacketIdOf(packet);
 if(id!=0x90){if(id!=0x1e||nested||k.mode!=0||s.sent||!s.has)return false;uint64_t data{};int32_t type{},action{};
  if(Read(packet+0x170,&data)&&data&&Read(data+8,&type)&&type==3&&Read(data+0x70,&action)&&action==1&&CriticalsAllowed(c,k))CriticalsCraft(s,c);return false;}
 if(k.mode==3)return false;
 if(k.mode==0){InstaSnapshot(s.snapshot,packet);s.has=true;s.sent=false;CriticalsRestore(s,c,packet);return false;}
 if(CriticalsAllowed(c,k)&&(!k.targetRequired||CrosshairActor(c))){*reinterpret_cast<uint64_t*>(packet+8)=0x100000000ull;
  if(k.mode==1){*reinterpret_cast<float*>(packet+0x3c)=(s.phase?0.2001f:0.2002f)+*reinterpret_cast<float*>(packet+0x3c);s.phase=true;}else s.phase=false;
  float fall{};if(ActorFallDistance(c.actor,&fall)&&0.0f<fall){AuthInputStopSprint(packet);s.restore=true;return false;}
  CriticalsRestore(s,c,packet);return false;}
 if(s.restore&&ActorSprinting(c.actor))AuthInputResumeSprint(packet);s.phase=false;s.restore=false;return false;}
}
