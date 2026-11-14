// Packet holding system used by Blink and Fake Lag.
// - SerializedPacketSendEvent #36 (MC 0x2383370, priority 5): on the game thread, an uncancelled send whose this =
// network handler + 0x20 (handler = *(sender+0x20) when byte +0x28 is zero; sender = client->vtable[0x910]) with a
// server connection present (first element of [network+0xe8, +0xf0), the connection vector the game's own send reads, byte +0x199 zero, peer *(+0x118)) raises
// LagPacketEvent { outgoing, id (packet->vtable[1]), serialized data, hold mask }; a zero mask raises
// LagPacketPassEvent, otherwise the send is cancelled and a copy is queued
// { data, id, packet+0xc, packet+0x28, timestamp, mask }.
// - Resend runs on every GameUpdate: while the queue head has a zero mask, its bytes are resent as-is through
// peer->vtable[1](peer, data, packet+0xc, packet+0x28), then LagPacketPassEvent is raised; a changed connection or
// a refused resend clears the queue.
// - Counting and release per module bit; some ids are never held; connection packet ids are tracked.
// - Incoming packets (Blink "Bi-directional", Fake Lag "Freeze"): the original holds them at the serialized level
// (ConnectionReceiveEvent #35); here they are taken from the per-type receive dispatchers (decoded packets, as
// Backtrack): LagPacketEvent { outgoing = false } is raised, a non-zero mask keeps a reference to the packet
// (shared_ptr) and drops it from the hook. Once its mask is clear, GameUpdate replays it through the local receive path
// (ReceivePacket), in order, after the outgoing resend. Connection packets are never held and clear both queues.
// RakNet, NetherNet and the local server (singleplayer) connections are all in this vector, so packets are held with
// every transport.
#pragma once
#include "game_packets.hpp"
#include "game_visual_entities.hpp"
#include <MinHook.h>
#include <atomic>
#include <deque>
#include <mutex>
#include <string>

namespace game {
struct LagRecord{std::string data;int id=0;int32_t a=0,b=0;int64_t at=0;uint8_t mask=0;};
struct LagIncoming{GamePacket packet;int id=0;int64_t at=0;uint8_t mask=0;};
struct LagShared{std::recursive_mutex guard;std::deque<LagRecord> queue;std::deque<LagIncoming> incoming;bool replaying=false;uint64_t connection=0,peer=0;std::atomic<bool> fault{false};std::atomic<long> gameThread{0};
 bool(*volatile active)()=nullptr;};
inline LagShared& Lag(){static auto* s=new LagShared;return *s;}
inline int64_t LagNs(){LARGE_INTEGER f{},c{};QueryPerformanceFrequency(&f);QueryPerformanceCounter(&c);return (int64_t)((double)c.QuadPart*1e9/(double)f.QuadPart);}
inline void(*lagLog)(const char*)=nullptr;
// Packet ids that are never held.
inline bool LagNeverHold(int id){switch(id){case 0:case 1:case 2:case 3:case 4:case 5:case 6:case 7:case 8:case 11:case 61:case 82:case 83:case 84:case 85:case 94:case 113:case 115:case 143:case 193:case 0x138:case 0x154:return true;}return false;}
// Connection packets.
inline bool LagConnectionPacket(int id){return (id<=0x3d&&((0x2000000000000822ull>>id)&1))||id==0x55;}
inline uint64_t LagNetwork(const Context& c){const uint64_t get=Virtual0(c,c.client,0x910);uint64_t sender{},net{};uint8_t closed{};if(!get||!CallGetter(get,c.client,&sender)||!sender)return 0;
 if(!Read(sender+0x28,&closed)||closed)return 0;return Read(sender+0x20,&net)?net:0;}
inline void LagDropIncoming(LagShared& s){if(!s.incoming.empty()&&lagLog){static int told=0;if(told++<5){char line[96];wsprintfA(line,"Lag: %d held incoming packets dropped",(int)s.incoming.size());lagLog(line);}}for(auto& r:s.incoming)ReleasePacket(r.packet);s.incoming.clear();}
// Server connection; a change clears the queues.
inline uint64_t LagConnection(const Context& c,LagShared& s,uint64_t* peerOut){const uint64_t net=LagNetwork(c);uint64_t b{},e{},conn{},peer{};uint8_t closed{};
 if(!net||!Read(net+0xe8,&b)||!Read(net+0xf0,&e)||b==e||!Read(b,&conn)||!conn||!Read(conn+0x199,&closed)||closed||!Read(conn+0x118,&peer)||!peer)return 0;
 if(conn!=s.connection||peer!=s.peer){s.queue.clear();LagDropIncoming(s);s.connection=conn;s.peer=peer;}if(peerOut)*peerOut=peer;return conn;}
// LagPacketEvent / LagPacketPassEvent listeners (Blink bit 1, Fake Lag bit 0).
struct LagEvent{bool outgoing;int id;const char* data;size_t size;uint64_t packet;uint8_t mask;};
inline void(*volatile lagBlinkEvent)(LagEvent&)=nullptr;inline void(*volatile lagFakeEvent)(LagEvent&)=nullptr;
inline void(*volatile lagFakePass)(const LagEvent&)=nullptr;
inline int LagPacketId(uint64_t packet){uint64_t t{},g{};return Read(packet,&t)&&t&&Read(t+8,&g)&&g?reinterpret_cast<int(*)(uint64_t)>(g)(packet):-1;}

using LagSendFunction=void(*)(uint64_t,uint64_t,uint64_t,uint64_t);inline LagSendFunction lagSendOriginal=nullptr;
inline bool LagSendWork(uint64_t self,uint64_t packet,uint64_t data){auto& s=Lag();if((long)GetCurrentThreadId()!=s.gameThread.load())return false;Context c{};if(!Client(&c))return false;
 const uint64_t net=LagNetwork(c);if(!net||net+0x20!=self)return false;std::lock_guard<std::recursive_mutex> lock(s.guard);if(!LagConnection(c,s,nullptr))return false;
 auto* text=reinterpret_cast<std::string*>(data);LagEvent ev{true,LagPacketId(packet),text->data(),text->size(),packet,0};
 if(auto f=lagFakeEvent)f(ev);if(auto bl=lagBlinkEvent)bl(ev);
 if(!ev.mask){if(auto p=lagFakePass)p(ev);return false;}
 LagRecord r;r.data=*text;r.id=ev.id;Read(packet+0xc,&r.a);Read(packet+0x28,&r.b);r.at=LagNs();r.mask=ev.mask;s.queue.push_back(std::move(r));return true;}
inline bool LagSendGuarded(uint64_t self,uint64_t packet,uint64_t data){__try{return LagSendWork(self,packet,data);}__except(EXCEPTION_EXECUTE_HANDLER){Lag().fault=true;return false;}}
inline void LagSendDetour(uint64_t self,uint64_t id,uint64_t packet,uint64_t data){if(!Lag().fault&&Lag().active&&Lag().active()&&LagSendGuarded(self,packet,data))return;lagSendOriginal(self,id,packet,data);}

// Resend, count and release.
inline void LagFlushWork(){auto& s=Lag();s.gameThread=(long)GetCurrentThreadId();Context c{};if(!Client(&c))return;std::lock_guard<std::recursive_mutex> lock(s.guard);
 uint64_t peer{};const uint64_t conn=LagConnection(c,s,&peer);if(!conn){s.queue.clear();LagDropIncoming(s);return;}
 while(!s.queue.empty()&&!s.queue.front().mask){LagRecord r=std::move(s.queue.front());s.queue.pop_front();
  uint64_t again{};const uint64_t now=LagConnection(c,s,&again);const uint64_t send=now==conn&&again==peer?Virtual0(c,peer,8):0;
  if(!send){s.queue.clear();break;}reinterpret_cast<void(*)(uint64_t,std::string*,int32_t,int32_t)>(send)(peer,&r.data,r.a,r.b);
  LagEvent ev{true,r.id,r.data.data(),r.data.size(),0,0};if(auto p=lagFakePass)p(ev);}
 while(!s.incoming.empty()&&!s.incoming.front().mask){LagIncoming r=std::move(s.incoming.front());s.incoming.pop_front();
  s.replaying=true;const bool ok=ReceivePacket(c,&r.packet);s.replaying=false;ReleasePacket(r.packet);
  if(!ok){if(lagLog){static int told=0;if(told++<3)lagLog("Lag: replay of a held incoming packet refused");}LagDropIncoming(s);break;}}}
inline void LagFlush(){if(Lag().fault)return;__try{LagFlushWork();}__except(EXCEPTION_EXECUTE_HANDLER){Lag().fault=true;}}
inline size_t LagCount(int bit){auto& s=Lag();std::lock_guard<std::recursive_mutex> lock(s.guard);size_t n=0;for(auto& r:s.queue)if(r.mask&(1u<<bit))++n;for(auto& r:s.incoming)if(r.mask&(1u<<bit))++n;return n;}
inline void LagRelease(int bit){auto& s=Lag();std::lock_guard<std::recursive_mutex> lock(s.guard);for(auto& r:s.queue)r.mask&=(uint8_t)~(1u<<bit);for(auto& r:s.incoming)r.mask&=(uint8_t)~(1u<<bit);}
// Position sent in a serialized PlayerAuthInputPacket: varint id (10 bits), then pitch, yaw, x, y, z.
inline bool LagAuthPosition(const LagEvent& ev,float* pos){if(ev.id!=0x90||!ev.data)return false;size_t at=0;uint32_t header=0;int shift=0;
 while(at<ev.size&&shift<35){const uint8_t b=(uint8_t)ev.data[at++];header|=(uint32_t)(b&0x7f)<<shift;shift+=7;if(!(b&0x80))break;}
 if((header&0x3ff)!=0x90||at+0x14>ev.size)return false;memcpy(pos,ev.data+at+8,12);return true;}

// Receive hook (shared_ptr<Packet>&): true = held, the game does not handle it now.
inline bool LagReceiveWork(void* shared){auto& s=Lag();uint64_t object{};if(!shared||!Read((uintptr_t)shared,&object)||!object)return false;
 std::lock_guard<std::recursive_mutex> lock(s.guard);if(s.replaying)return false;const int id=LagPacketId(object);if(id<0)return false;
 LagEvent ev{false,id,nullptr,0,object,0};if(auto f=lagFakeEvent)f(ev);if(auto bl=lagBlinkEvent)bl(ev);
 if(LagConnectionPacket(id)){LagDropIncoming(s);return false;}
 if(!ev.mask)return false;
 uint64_t control{};if(!Read((uintptr_t)shared+8,&control)||!control)return false;InterlockedIncrement(reinterpret_cast<volatile long*>(control)+2);
 LagIncoming r;r.packet.object=object;r.packet.control=control;r.id=id;r.at=LagNs();r.mask=ev.mask;s.incoming.push_back(std::move(r));
 {static int held=0;if(lagLog&&(++held==1||held%500==0)){char line[96];wsprintfA(line,"Lag: incoming packet held (%d so far, id %d)",held,id);lagLog(line);}}return true;}
inline bool LagReceive(void* shared){auto& s=Lag();if(s.fault||!s.active||!s.active())return false;__try{return LagReceiveWork(shared);}__except(EXCEPTION_EXECUTE_HANDLER){s.fault=true;return false;}}

inline int InstallLagHooks(void(*log)(const char*)){static int state=0;if(state)return state;lagLog=log;state=-1;uintptr_t base{};uint32_t size{};if(!Supported(&base,&size))return state;
 const uint8_t bytes[16]={0x55,0x41,0x57,0x41,0x56,0x41,0x55,0x41,0x54,0x56,0x57,0x53,0x48,0x83,0xec,0x58};
 for(int i=0;i<16;++i){uint8_t b{};if(!Read(base+0x2383370+i,&b)||b!=bytes[i]){if(log)log("Lag: unexpected code at the serialized send function");return state;}}
 if(MH_CreateHook((void*)(base+0x2383370),(void*)LagSendDetour,(void**)&lagSendOriginal)!=MH_OK||MH_EnableHook((void*)(base+0x2383370))!=MH_OK){if(log)log("Lag: hook not installed");return state;}
 if(log)log("Lag: serialized send hook installed");state=1;return state;}

// ---- Blink (bit 1): event, GameUpdate, enable / disable
struct BlinkShared{std::atomic<bool> enabled{false},movementOnly{false},autoSend{false},bothWays{false};std::atomic<int> threshold{40};float real[3]{};bool hasReal=false;};
inline BlinkShared& Blink(){static auto* b=new BlinkShared;return *b;}
inline void BlinkEvent(LagEvent& ev){auto& b=Blink();if(!b.enabled)return;Context c{};
 if((!ev.outgoing&&LagConnectionPacket(ev.id))||!Local(&c)){b.hasReal=false;return;}
 if(!ev.outgoing){if(b.bothWays&&!LagConnectionPacket(ev.id))ev.mask|=2;return;}   // Bi-directional: every incoming packet is held too
 if(LagNeverHold(ev.id))return;
 if(!b.hasReal){uint64_t st{};if(Read(c.actor+0x240,&st)&&st){Read(st,&b.real[0]);Read(st+4,&b.real[1]);Read(st+8,&b.real[2]);b.hasReal=true;}}
 const int id=ev.id;const bool hold=b.movementOnly?(id==0x90||id==0x13):((id<=0x2c&&((0x100008080200ull>>id)&1))||id==0x8a||id==0x90);
 if(hold)ev.mask|=2;}
inline void BlinkTick(){auto& b=Blink();if(!b.enabled)return;Context c{};if(!Local(&c)){b.hasReal=false;return;}
 if(b.autoSend&&LagCount(1)>=(size_t)b.threshold.load())LagRelease(1);}

// ---- Fake Lag (bit 0): event, pass, GameUpdate, stop
struct FakeLagShared{std::mutex guard;std::atomic<bool> enabled{false},freezeMode{false},onlyTarget{false};std::atomic<int> maxFreeze{400},cooldown{100};std::atomic<float> range{6.f};
 nickel::visual::TeamRules teams{};float real[3]{};bool hasReal=false,freezing=false,cooling=false;int64_t freezeAt=0,releaseAt=0;};
inline FakeLagShared& FakeLag(){static auto* f=new FakeLagShared;return *f;}
inline void FakeLagStop(FakeLagShared& f){LagRelease(0);f.releaseAt=LagNs();f.cooling=true;f.freezing=false;}
inline void FakeLagReset(FakeLagShared& f){FakeLagStop(f);f.hasReal=false;f.freezing=false;f.cooling=false;}
inline void FakeLagEvent(LagEvent& ev){auto& f=FakeLag();if(!f.enabled)return;Context c{};
 if((!ev.outgoing&&LagConnectionPacket(ev.id))||!Local(&c)){FakeLagReset(f);return;}
 if(!f.freezing)return;if((double)(LagNs()-f.freezeAt)/1e6>(double)f.maxFreeze.load()){FakeLagStop(f);return;}
 if(!ev.outgoing){if(f.freezeMode&&!LagConnectionPacket(ev.id))ev.mask|=1;return;}   // Freeze: other players stop moving on screen
 const int id=ev.id;if(id==0x13||id==0x90||id==0x142||(id==0x73&&f.freezeMode))ev.mask|=1;}
inline void FakeLagPass(const LagEvent& ev){auto& f=FakeLag();if(!f.enabled||!ev.outgoing)return;float p[3]{};if(LagAuthPosition(ev,p)){memcpy(f.real,p,12);f.hasReal=true;}}
inline bool FakeLagTarget(FakeLagShared& f){VisualEntity list[64];const int n=ReadVisualEntities(list,64,f.range.load(),&f.teams);for(int i=0;i<n;++i)if(list[i].player&&!list[i].teammate)return true;return false;}   // nearest player within range
inline bool FakeLagMoving(const Context& c){uint64_t st{};float p[6]{};if(!Read(c.actor+0x240,&st)||!st)return false;for(int i=0;i<6;++i)Read(st+i*4,&p[i]);
 return 0.0004f<(p[0]-p[3])*(p[0]-p[3])+(p[2]-p[5])*(p[2]-p[5]);}
inline void FakeLagTick(){auto& f=FakeLag();if(!f.enabled)return;Context c{};if(!Local(&c)){FakeLagReset(f);return;}
 const double sinceFreeze=(double)(LagNs()-f.freezeAt)/1e6;
 if(!f.freezing){if(!f.hasReal)return;if(f.cooling&&(double)(LagNs()-f.releaseAt)/1e6<=(double)f.cooldown.load())return;
  if(FakeLagMoving(c)&&(!f.onlyTarget||FakeLagTarget(f))){f.freezeAt=LagNs();f.freezing=true;}return;}
 if(sinceFreeze<=(double)f.maxFreeze.load()&&FakeLagMoving(c)&&(!f.onlyTarget||FakeLagTarget(f)))return;
 FakeLagStop(f);}

inline bool LagAnyActive(){return Blink().enabled.load()||FakeLag().enabled.load();}
inline void LagGameUpdate(){if(Lag().fault)return;__try{BlinkTick();FakeLagTick();}__except(EXCEPTION_EXECUTE_HANDLER){Lag().fault=true;}LagFlush();}
}
