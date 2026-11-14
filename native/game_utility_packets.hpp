// Utility modules that listen to packets or block actions.
// Packet Logger: Receive Only / Send Only; Whitelist (non-empty list without the id -> ignored); Blacklist (id
// listed -> ignored); message "§7[§dNickel§7] §7[§aSEND§7] / [§cRECV§7] §b<name> §7(§e<id>§7)"
// (name = packet->vtable[2]).
// Nick Hider: the local player's name (actor+0xbe8) is replaced by Name in TextPacket 9 (+0x78 if +0x98 = 1;
// variant +0xe8: 0 -> +0xa8; 1 -> +0xa8 and +0xc8; 2 -> +0xa8 and each string of the array +0xc8..+0xd0); with
// Scoreboard: SetScorePacket 0x6c (0x60-byte entries at +0x30..+0x38, string at +0x38 when byte +0x58 is 3) and
// 0x6b (string at +0x70). The local player's nametag text is replaced too (hook #1).
// Auto Sign: when placing with a "*_sign" item in hand, positions are remembered for 2 s (the neighbour on the
// clicked face, and the clicked block if it has no block entity). OpenSignPacket 0x12f (+0x30 position, +0x3c front
// face) on a remembered position: the text (lines joined with "\n", at most 256 characters) is written by
// MC 0x4c5b880 (face 0, and face 1 with Side = Both); the update packet = entity->vtable[0x90](&p, region), sent
// through sender->vtable[0x10]; the sign screen is cancelled.
// Insta Break, Client mode (ContinueDestroyBlock MC 0x376a590): allowed block (name whitelist) -> progress
// gamemode+0x20 and +0x24 = 1.0; Haste: after the game call, if *arg5 is 1, gamemode+0x90 = 0.
// Insta Break, Packet mode: replays the last PlayerAuthInputPacket.
#pragma once
#include "game_packets.hpp"
#include "game_packet_hooks.hpp"
#include "game_xray.hpp"
#include <mutex>
#include <vector>

namespace game {
inline int PacketIdOf(uintptr_t packet){uint64_t table{},get{};if(!packet||!Read(packet,&table)||!table||!Read(table+8,&get)||!get)return -1;return reinterpret_cast<int(*)(uintptr_t)>(get)(packet);}
// Packet name: packet->vtable[2](packet, &view) writes a string view { data, length }.
inline std::string PacketNameOf(uintptr_t packet){uint64_t table{},get{};if(!packet||!Read(packet,&table)||!table||!Read(table+0x10,&get)||!get)return "";struct View{const char* data;size_t size;}view{};
 reinterpret_cast<void(*)(uintptr_t,View*)>(get)(packet,&view);if(!view.data||view.size>256)return "";return std::string(view.data,view.size);}

// ---- Packet Logger
struct PacketLoggerShared{std::mutex guard;bool enabled=false,receiveOnly=false,sendOnly=false,white=false,black=false;std::vector<int> whiteIds,blackIds;};
inline PacketLoggerShared& PacketLogger(){static auto* s=new PacketLoggerShared;return *s;}
inline void PacketLoggerWork(uintptr_t packet,bool sent){auto& l=PacketLogger();bool white,black;std::vector<int> w,b;
 {std::lock_guard<std::mutex> lock(l.guard);if(!l.enabled||(sent&&l.receiveOnly)||(!sent&&l.sendOnly))return;white=l.white;black=l.black;w=l.whiteIds;b=l.blackIds;}
 const int id=PacketIdOf(packet);
 if(white&&!w.empty()&&std::find(w.begin(),w.end(),id)==w.end())return;
 if(black&&std::find(b.begin(),b.end(),id)!=b.end())return;
 Context c{};if(!Local(&c))return;
 ClientMessage(c,std::string("\xc2\xa7""7[\xc2\xa7""dNickel\xc2\xa7""7] \xc2\xa7""7[")+(sent?"\xc2\xa7""aSEND":"\xc2\xa7""cRECV")+"\xc2\xa7""7] \xc2\xa7""b"+PacketNameOf(packet)+" \xc2\xa7""7(\xc2\xa7""e"+std::to_string(id)+"\xc2\xa7""7)");}

// ---- Nick Hider
struct NickHiderShared{std::mutex guard;bool enabled=false,scoreboard=true;std::string name;};
inline NickHiderShared& NickHider(){static auto* s=new NickHiderShared;return *s;}
inline void ReplaceAll(std::string& text,const std::string& from,const std::string& to){if(from.empty())return;for(size_t at=0;(at=text.find(from,at))!=std::string::npos;at+=to.size())text.replace(at,from.size(),to);}
inline void NickHiderWork(uintptr_t packet){auto& n=NickHider();std::string to;bool scoreboard;{std::lock_guard<std::mutex> lock(n.guard);if(!n.enabled)return;to=n.name;scoreboard=n.scoreboard;}
 Context c{};if(!Local(&c))return;char own[256]{};if(!RawText(c.actor+0xbe8,own,sizeof(own))||!own[0]||to==own)return;const std::string from=own;
 const int id=PacketIdOf(packet);
 if(id==9){if(*reinterpret_cast<uint8_t*>(packet+0x98)==1)ReplaceAll(PacketText(packet+0x78),from,to);const uint8_t index=*reinterpret_cast<uint8_t*>(packet+0xe8);
  if(index==0)ReplaceAll(PacketText(packet+0xa8),from,to);
  else if(index==1){ReplaceAll(PacketText(packet+0xc8),from,to);ReplaceAll(PacketText(packet+0xa8),from,to);}
  else if(index==2){ReplaceAll(PacketText(packet+0xa8),from,to);uint64_t begin=*reinterpret_cast<uint64_t*>(packet+0xc8),end=*reinterpret_cast<uint64_t*>(packet+0xd0);for(uint64_t at=begin;at&&at<end;at+=0x20)ReplaceAll(PacketText(at),from,to);}
  return;}
 if(!scoreboard)return;
 if(id==0x6c){uint64_t begin=*reinterpret_cast<uint64_t*>(packet+0x30),end=*reinterpret_cast<uint64_t*>(packet+0x38);for(uint64_t at=begin;at&&at+0x60<=end;at+=0x60)if(*reinterpret_cast<uint8_t*>(at+0x58)==3)ReplaceAll(PacketText(at+0x38),from,to);return;}
 if(id==0x6b)ReplaceAll(PacketText(packet+0x70),from,to);}
inline bool NickHiderTag(void* actor,std::string* out){auto& n=NickHider();{std::lock_guard<std::mutex> lock(n.guard);if(!n.enabled)return false;*out=n.name;}Context c{};return Local(&c)&&(uintptr_t)actor==c.actor;}

// ---- Auto Sign
struct SignSpot{int32_t x,y,z;uint64_t at;};
struct AutoSignShared{std::mutex guard;bool enabled=false,both=false;std::vector<std::string> lines;std::vector<SignSpot> spots;};
inline AutoSignShared& AutoSign(){static auto* s=new AutoSignShared;return *s;}
inline uint64_t ActorRegion(const Context& c){uint64_t owner{};if(!Read(c.actor+0x1f0,&owner)||!owner)return 0;const uint64_t of=Virtual0(c,owner,0x68);return of?reinterpret_cast<uint64_t(*)(uint64_t)>(of)(owner):0;}
inline uint64_t RegionBlockEntity(const Context& c,uint64_t region,const int32_t* pos){const uint64_t f=Virtual0(c,region,0x18);return f?reinterpret_cast<uint64_t(*)(uint64_t,const int32_t*)>(f)(region,pos):0;}
inline uint64_t RegionBlock(const Context& c,uint64_t region,const int32_t* pos){const uint64_t f=Virtual0(c,region,0x10);return f?reinterpret_cast<uint64_t(*)(uint64_t,const int32_t*)>(f)(region,pos):0;}
inline bool HeldItemName(const Context& c,char* out,size_t capacity){uint64_t inventory{},container{};int32_t slot{};out[0]=0;if(!Read(c.actor+0x5e0,&inventory)||!inventory||!Read(inventory+0xb8,&container)||!container||!Read(inventory+0x10,&slot))return false;
 const uint64_t get=Virtual0(c,container,0x38);const uint64_t item=StackItem(get?StackAt(get,container,slot):0);return item&&Text(item+0xd8,out,capacity);}
inline void SignPrune(AutoSignShared& s){const uint64_t now=GetTickCount64();s.spots.erase(std::remove_if(s.spots.begin(),s.spots.end(),[&](const SignSpot& p){return now-p.at>2000;}),s.spots.end());}
inline void SignAdd(AutoSignShared& s,const int32_t* p){s.spots.erase(std::remove_if(s.spots.begin(),s.spots.end(),[&](const SignSpot& q){return q.x==p[0]&&q.y==p[1]&&q.z==p[2];}),s.spots.end());s.spots.push_back({p[0],p[1],p[2],GetTickCount64()});}
inline void AutoSignBuildWork(const int32_t* pos,uint8_t face){auto& s=AutoSign();{std::lock_guard<std::mutex> lock(s.guard);if(!s.enabled)return;}
 Context c{};if(!Local(&c)||!pos)return;char name[128]{};if(!HeldItemName(c,name,sizeof(name)))return;const size_t n=strlen(name);if(n<=4||strcmp(name+n-5,"_sign"))return;
 const uint64_t region=ActorRegion(c);if(!region)return;static const int dx[6]={0,0,0,0,-1,1},dy[6]={-1,1,0,0,0,0},dz[6]={0,0,-1,1,0,0};
 int32_t next[3]={pos[0]+(face<6?dx[face]:0),pos[1]+(face<6?dy[face]:0),pos[2]+(face<6?dz[face]:0)};
 std::lock_guard<std::mutex> lock(s.guard);SignPrune(s);SignAdd(s,next);if(!RegionBlockEntity(c,region,pos))SignAdd(s,pos);}
inline bool AutoSignReceiveWork(uintptr_t packet){auto& s=AutoSign();std::string text;bool both;
 {std::lock_guard<std::mutex> lock(s.guard);if(!s.enabled||PacketIdOf(packet)!=0x12f||*reinterpret_cast<uint8_t*>(packet+0x3c)!=1)return false;SignPrune(s);
  const int32_t* p=reinterpret_cast<const int32_t*>(packet+0x30);const size_t before=s.spots.size();
  s.spots.erase(std::remove_if(s.spots.begin(),s.spots.end(),[&](const SignSpot& q){return q.x==p[0]&&q.y==p[1]&&q.z==p[2];}),s.spots.end());if(s.spots.size()==before)return false;
  for(auto& line:s.lines){if(!text.empty())text+="\n";text+=line;}if(text.size()>256)text.resize(256);both=s.both;}
 Context c{};if(!Local(&c))return false;const uint64_t region=ActorRegion(c);if(!region)return false;const uint64_t be=RegionBlockEntity(c,region,reinterpret_cast<const int32_t*>(packet+0x30));if(!be)return false;
 const uint8_t type=*reinterpret_cast<uint8_t*>(be+0x14);if(type!=0x32&&type!=4)return false;
 using SetMessage=void(*)(uint64_t,int,std::string*);{std::string a=text;reinterpret_cast<SetMessage>(c.base+0x4c5b880)(be,0,&a);}if(both){std::string b=text;reinterpret_cast<SetMessage>(c.base+0x4c5b880)(be,1,&b);}
 const uint64_t make=Virtual0(c,be,0x90);if(!make)return false;uint64_t update=0;reinterpret_cast<void(*)(uint64_t,uint64_t*,uint64_t)>(make)(be,&update,region);if(!update)return false;
 const uint64_t get=Virtual0(c,c.client,0x910);const uint64_t sender=get?reinterpret_cast<uint64_t(*)(uint64_t)>(get)(c.client):0;const uint64_t send=sender?Virtual0(c,sender,0x10):0;
 if(send)reinterpret_cast<void(*)(uint64_t,uint64_t)>(send)(sender,update);const uint64_t destroy=Virtual0(c,update,0);if(destroy)reinterpret_cast<void(*)(uint64_t,int)>(destroy)(update,1);return true;}

// ---- Insta Break (Client mode)
struct InstaBreakShared{std::mutex guard;bool enabled=false,client=true,haste=true,white=false;std::vector<std::string> blocks;int packets=10;
 // Packet mode state: 0x88-byte copy of the last PlayerAuthInputPacket (+0x88 present), packet credit (+0x90),
 // send in progress (+0x94).
 uint8_t snapshot[0x88]{};bool hasSnapshot=false;std::atomic<bool> sending{false};int budget=0;
 // Copies sent since the game's last own PlayerAuthInputPacket (one per tick): the hook below can run every frame,
 // so "Packets" is enforced per tick here; without it a high frame rate floods the server (PNX kicks above 1500/s).
 int tickSent=0;};
inline InstaBreakShared& InstaBreak(){static auto* s=new InstaBreakShared;return *s;}
inline bool InstaBreakAllowed(uint64_t gamemode,const int32_t* pos,const std::vector<std::string>& blocks){uint64_t player{};Context c{};if(!Read(gamemode+8,&player)||!player||!Local(&c))return false;
 const uint64_t region=ActorRegion(c);const uint64_t block=region?RegionBlock(c,region,pos):0;std::string name;if(!block||!XrayBlockName(block,&name))return false;return std::find(blocks.begin(),blocks.end(),name)!=blocks.end();}


// ---- Insta Break, Packet mode
// Copy / rebuild of the PlayerAuthInputPacket fields; input bits are cleared.
inline const std::pair<int,int> kInstaFields[]={{0x00,0x30},{0x08,0x38},{0x10,0x40},{0x14,0x44},{0x20,0x50},{0x18,0x48},{0x24,0x54},{0x2c,0x5c},{0x34,0x64},{0x3c,0x6c},{0x44,0x74},
 {0x4c,0x7c},{0x50,0x80},{0x58,0x88},{0x60,0x90},{0x68,0x98},{0x6c,0x9c},{0x70,0xa0},{0x78,0xa8},{0x80,0xd8}};
inline int InstaFieldSize(int snap){return snap==0x10||snap==0x14||snap==0x20||snap==0x4c||snap==0x68||snap==0x6c||snap==0x70?4:8;}
inline void InstaSnapshot(uint8_t* snap,uintptr_t packet){for(auto [s,p]:kInstaFields)memcpy(snap+s,(void*)(packet+p),InstaFieldSize(s));
 static const int bits[26]={25,26,27,28,29,30,31,32,33,34,35,36,37,39,40,41,42,43,44,53,56,57,60,59,63,62};uint64_t* words=(uint64_t*)(snap+0x58);for(int b:bits)words[b>>6]&=~(1ull<<(b&63));}
inline void InstaRebuild(uintptr_t packet,const uint8_t* snap){for(auto [s,p]:kInstaFields)memcpy((void*)(packet+p),snap+s,InstaFieldSize(s));}
// At most n copies, only on the ground and standing still.
inline bool InstaOnGround(uintptr_t actor){uintptr_t registry{},ground{};uint32_t entity{},packed{};return ActorEntity(actor,&registry,&entity)&&Storage(registry,Fnv("OnGroundFlagComponent"),&ground)&&Packed(ground,entity,&packed);}   // on-ground check
inline int InstaSendCopies(InstaBreakShared& s,uintptr_t actor,int n){const int room=s.packets-s.tickSent;if(n>room)n=room;if(n<=0||!s.hasSnapshot||!InstaOnGround(actor))return 0;const uint8_t* q=s.snapshot;
 auto f=[&](int at){float v;memcpy(&v,q+at,4);return v;};if(f(0x34)!=0.f||f(0x38)!=0.f||f(0x2c)!=0.f||f(0x30)!=0.f||fabsf(f(0x18))>=0.0001f||fabsf(f(0x20))>=0.0001f)return 0;
 uint8_t copy[0x88];memcpy(copy,q,sizeof(copy));Context c{};if(!Client(&c))return 0;s.sending=true;
 for(int i=0;i<n;++i){GamePacket pk=CreatePacket(c,0x90);if(pk.object){InstaRebuild(pk.object,copy);SendPacket(c,pk.object);}ReleasePacket(pk);}s.sending=false;s.tickSent+=n;return n;}
// ContinueDestroyBlock (Packet mode); *destroyed is read after the game call.
inline bool InstaPacketPre(uint64_t gm,int32_t* pos,float* delta){auto& s=InstaBreak();bool white;std::vector<std::string> blocks;int limit;
 {std::lock_guard<std::mutex> lock(s.guard);if(!s.enabled||s.client)return false;white=s.white;if(white)blocks=s.blocks;limit=s.packets;}
 Context c{};uint64_t actor{};if(!Read(gm+8,&actor)||!actor||!Local(&c)||actor!=c.actor)return false;
 uintptr_t registry{},type{};uint32_t entity{};int32_t gameType=-1;if(ActorEntity(actor,&registry,&entity)&&Element(registry,entity,"ActorGameTypeComponent",4,&type))Read(type,&gameType);if(gameType==1)return false;
 if(white&&!InstaBreakAllowed(gm,pos,blocks))return false;
 uint8_t destroying{};Read(actor+0xb4a,&destroying);int32_t cur[3]{};for(int i=0;i<3;++i)Read(gm+0x10+i*4,&cur[i]);
 if(destroying==1&&cur[0]==pos[0]&&cur[1]==pos[1]&&cur[2]==pos[2]){const uint64_t region=ActorRegion(c);if(!region)return false;const uint64_t block=RegionBlock(c,region,pos);
  const float d=reinterpret_cast<float(*)(uint64_t,uint64_t)>(c.base+0x376b010)(gm,block);if(!(d>0.f))return false;   // destroy speed of the block
  float progress=*(float*)(gm+0x24);int ticks=(int)ceilf((0.99999f-progress)/d);if(ticks<1)ticks=0;const int before=s.budget;int want=ticks+before-1;want=ticks+before>0?want:0;if(limit<want)want=limit;
  const int sent=InstaSendCopies(s,actor,want);const int used=sent<before?sent:before;s.budget=before-used;
  *(float*)(gm+0x24)=(float)((sent-used)-((before-used!=0&&used<=before)?1:0))*d+progress;*(uint64_t*)(gm+0x90)=0;*delta=d;return true;}
 int n=s.budget<limit?s.budget:limit;const int sent=InstaSendCopies(s,actor,n);s.budget-=sent;if(s.budget==0)*(uint64_t*)(gm+0x90)=0;return false;}
inline bool InstaPacketPreGuarded(uint64_t gm,int32_t* pos,float* d){__try{return InstaPacketPre(gm,pos,d);}__except(EXCEPTION_EXECUTE_HANDLER){return false;}}
inline void InstaPacketPost(uint64_t gm,float d){auto& s=InstaBreak();int64_t ms=*(int64_t*)(gm+0xa0);s.budget=d<0.99999f?(int)(ms/50):0;}
// PacketSendEvent: copy every PlayerAuthInputPacket the game sends; credit -1.
inline void InstaPacketSendWork(uintptr_t packet){auto& s=InstaBreak();{std::lock_guard<std::mutex> lock(s.guard);if(!s.enabled||s.client)return;}if(s.sending)return;
 uint64_t t{},g{};if(!Read(packet,&t)||!t||!Read(t+8,&g)||!g||reinterpret_cast<int(*)(uintptr_t)>(g)(packet)!=0x90)return;InstaSnapshot(s.snapshot,packet);s.hasSnapshot=true;s.tickSent=0;s.budget=(s.budget>1?s.budget:1)-1;}
inline void InstaPacketSend(uintptr_t packet){__try{InstaPacketSendWork(packet);}__except(EXCEPTION_EXECUTE_HANDLER){}}

// ---- subscribers of the shared hooks
inline bool(*volatile actionReceive)(uintptr_t)=nullptr;   // MobEquipment for the action queue (game_actions.hpp)
inline bool(*volatile inventoryReceive)(uintptr_t)=nullptr;   // ContainerOpen for the inventory scheduler (game_inventory.hpp)
inline bool(*volatile scriptReceive)(uintptr_t)=nullptr;
inline bool(*volatile playerReceive)(uintptr_t)=nullptr;
inline bool(*volatile playerReceiveC)(uintptr_t)=nullptr;   // Player batch C (game_player_c.hpp): true = packet not handled
inline bool(*volatile velocityReceive)(uintptr_t)=nullptr;   // Velocity (game_movement3.hpp)
inline void(*volatile pearlChaserReceive)(uintptr_t)=nullptr;   // Pearl Chaser (game_movement_e.hpp, read only)
inline void(*volatile autoToolDestroy)(uint64_t,const int32_t*)=nullptr;   // Auto Tool (game_player.hpp)
inline bool(*volatile combatReceiveCancel)(uintptr_t)=nullptr;   // Combat: true cancels the packet (Auto Mace)
inline bool UtilityReceiveWork(uintptr_t packet){if(auto mr=movementReceive)mr(packet,PacketIdOf(packet));if(auto pc=pearlChaserReceive)pc(packet);if(auto pr=playerReceive)if(pr(packet))return true;if(auto pc=playerReceiveC)if(pc(packet))return true;if(auto vr=velocityReceive)if(vr(packet))return true;if(auto sr=scriptReceive)if(sr(packet))return true;if(auto cr=combatReceiveCancel)if(cr(packet))return true;if(auto a=actionReceive)if(a(packet))return true;if(auto inv=inventoryReceive)if(inv(packet))return true;NickHiderWork(packet);const bool cancel=AutoSignReceiveWork(packet);if(!cancel)PacketLoggerWork(packet,false);return cancel;}
inline bool UtilityReceive(uintptr_t packet){__try{return UtilityReceiveWork(packet);}__except(EXCEPTION_EXECUTE_HANDLER){return false;}}
inline void PacketLoggerSend(uintptr_t packet){__try{PacketLoggerWork(packet,true);}__except(EXCEPTION_EXECUTE_HANDLER){}}

// BuildBlock (hook #28, MC 0x376b1f0) and ContinueDestroyBlock (hook #27, MC 0x376a590) hooks.
using BuildBlockFunction=uint64_t(*)(uint64_t,int32_t*,uint8_t,uint8_t,uint64_t);inline BuildBlockFunction buildBlockOriginal=nullptr;
inline void AutoSignBuildGuarded(const int32_t* pos,uint8_t face){__try{AutoSignBuildWork(pos,face);}__except(EXCEPTION_EXECUTE_HANDLER){}}
inline bool(*volatile buildCancel)()=nullptr;   // Schematics: normal placement is cancelled during Auto place
inline void(*volatile buildObserve)(const int32_t* pos,uint8_t face)=nullptr;   // Combat: Auto Anchor remembers placed anchors
inline uint64_t BuildBlockDetour(uint64_t gm,int32_t* pos,uint8_t face,uint8_t r9,uint64_t a5){AutoSignBuildGuarded(pos,face);if(auto bo=buildObserve)bo(pos,face);if(auto bc=buildCancel)if(bc())return 0;return buildBlockOriginal(gm,pos,face,r9,a5);}
using DestroyFunction=uint32_t(*)(uint64_t,int32_t*,uint8_t,uint64_t,uint8_t*);inline DestroyFunction destroyOriginal=nullptr;
inline bool InstaBreakPre(uint64_t gm,int32_t* pos,bool* haste){auto& s=InstaBreak();{Context c{};uint64_t player{};if(!Local(&c)||!Read(gm+8,&player)||player!=c.actor)return false;}   // event only for the local player
 bool white;std::vector<std::string> blocks;{std::lock_guard<std::mutex> lock(s.guard);if(!s.enabled||!s.client)return false;*haste=s.haste;white=s.white;if(white)blocks=s.blocks;}
 if(!white||InstaBreakAllowed(gm,pos,blocks)){*reinterpret_cast<float*>(gm+0x24)=1.f;*reinterpret_cast<float*>(gm+0x20)=1.f;}return true;}
inline bool InstaBreakPreGuarded(uint64_t gm,int32_t* pos,bool* haste){__try{return InstaBreakPre(gm,pos,haste);}__except(EXCEPTION_EXECUTE_HANDLER){return false;}}
inline void(*instaLog)(const char*)=nullptr;
inline uint32_t DestroyDetour(uint64_t gm,int32_t* pos,uint8_t face,uint64_t r9,uint8_t* destroyed){if(auto at=autoToolDestroy)at(gm,pos);bool haste=false;const bool on=InstaBreakPreGuarded(gm,pos,&haste);float delta=0;const bool packetMode=InstaPacketPreGuarded(gm,pos,&delta);
 {static int told=0;if(told<3&&instaLog){++told;char l[128];snprintf(l,sizeof(l),"Insta Break diag: continue-destroy called, module %s",on?"applied":"not applied");instaLog(l);}}const uint32_t r=destroyOriginal(gm,pos,face,r9,destroyed);
 if(on&&haste&&destroyed&&*destroyed==1)*reinterpret_cast<uint64_t*>(gm+0x90)=0;if(packetMode&&destroyed&&*destroyed==1)InstaPacketPost(gm,delta);return r;}
inline int InstallBlockActionHooks(void(*log)(const char*)){static int state=0;if(state)return state;state=-1;uintptr_t base{};uint32_t size{};if(!Supported(&base,&size))return state;
 const uint8_t build[16]={0x55,0x41,0x57,0x41,0x56,0x41,0x55,0x41,0x54,0x56,0x57,0x53,0x48,0x81,0xec,0x58},destroy[16]={0x55,0x41,0x57,0x41,0x56,0x41,0x55,0x41,0x54,0x56,0x57,0x53,0x48,0x81,0xec,0xd8};
 for(int i=0;i<16;++i){uint8_t a{},b{};if(!Read(base+0x376b1f0+i,&a)||a!=build[i]||!Read(base+0x376a590+i,&b)||b!=destroy[i]){if(log)log("Utility: unexpected code at the block action hooks");return state;}}
 if(MH_CreateHook((void*)(base+0x376b1f0),(void*)BuildBlockDetour,(void**)&buildBlockOriginal)!=MH_OK||MH_CreateHook((void*)(base+0x376a590),(void*)DestroyDetour,(void**)&destroyOriginal)!=MH_OK){if(log)log("Utility: block action hooks not created");return state;}
 MH_QueueEnableHook((void*)(base+0x376b1f0));MH_QueueEnableHook((void*)(base+0x376a590));if(MH_ApplyQueued()!=MH_OK){if(log)log("Utility: block action hooks not enabled");return state;}
 if(log)log("Utility: build and continue-destroy hooks installed");state=1;return state;}
}
