// Player modules, group B: Chest Stealer, Chest Filler, Inventory Manager, Auto Refill.
// Items move through the ItemStackRequest path of game_inventory.hpp (container resolver RVA 0x2b6c950, own transfer
// action). Every frame entry runs on the screen render hook (#246); packets are observed, never cancelled.
// - Interaction tracking (send, InventoryTransactionPacket 0x1e, transaction at +0x170): ItemUse (+8 == 2) with
// action +0x68 == 0 stores the clicked BlockPos (+0x70); ItemUseOnActor (+8 == 3) with action +0x70 == 0 stores the
// ActorUniqueID of the runtime id at +0x68. Both keep the time of the click.
// - ContainerOpenPacket 0x2e (window +0x30, type +0x31, BlockPos +0x34, entity +0x40, -1 = block): types 0, 10, 34 are
// chests; 6, 7, 8, 11 utility containers; 12 mounts; anything else is ignored. Skip Menus accepts the window only within
// 1000 ms of a tracked click on the same block (squared distance <= 1) or on the same entity. ContainerClosePacket 0x2f
// (either way) ends the session.
// - The open container is checked on the container manager model (*(player+0x5c8): window +0x50, type +0x51) and found by
// enum name 7, then 0x3a, then 0x1e (RVA 0x3a6e6b0, returns a shared_ptr that is released at once).
// - Chest Stealer: after Start Delay, one stack per frame in Random (Fisher-Yates) or Slot order; Ignored Items are left;
// Only Useful skips swords / pickaxes / axes when the inventory already holds the same kind of tier >= (tags
// minecraft:<netherite..wooden>_tier, item+0x1f8, 0x30 bytes each) and armour not better than the worn piece. The stack goes
// to the first empty slot 9.. (inventory of 10+ slots), else 0..8; next take after Delay. When nothing can be taken,
// Auto Close sends ContainerClosePacket { window, type, 0 } unless nothing was taken yet, the window is under 1000 ms old
// and the container is still entirely empty (contents not received yet).
// - Chest Filler: mirror of the stealer from the player inventory (slots from 9 with Keep Hotbar) into the first empty
// container slot; Mode Except Kept / Only Dumped filters by list; never runs while Chest Stealer is enabled; closes as
// soon as nothing can be put in.
// - Inventory Manager: one action per Delay. Best sword = (tier, damage item->vtable[0x128] + 1.25 x Sharpness,
// enchantment sum 9+12+13+14+17+26, remaining durability), best pickaxe / axe = (tier, sum 15+16+17+18+26, durability);
// ties keep the preferred slot. Rules in order: move the best sword / pickaxe / axe to its preferred slot, fill the first
// preferred block slot that holds no block item (item+0x178) with the largest block stack outside the range, then drop
// (game RVA 0x2af140, Inventory::dropSlot(slot, false, true, false)) a Droppable Items stack, else the first food
// (item->vtable[0x98]) with Drop Food, else the first useless tool or worse armour with Drop Useless Items. The selected
// slot is locked outside the inventory screen. With "inventory_screen" shown the action waits 100 ms after the last mouse
// button change; Mode Always submits through the inventory scheduler otherwise.
// - Auto Refill: source = first slot 9.. whose item is listed in Items; destination = first empty slot of the Slots range
// (at most 9) not reserved by an enabled Inventory Manager (sword / pickaxe / axe slots with a best tool, block range when
// a block source exists); runs only with no action sequence pending and a free cursor; next refill after Delay. Close
// Inventory (Mode Inventory Only) asks the screen (ScreenView+0x40)->vtable[0x80]("inventory_screen") to close once a
// refill happened and nothing is left to move.
// Not ported: the legacy inventory transaction path (servers without ItemStackRequest), as in game_inventory.hpp.
#pragma once
#include "game_actions.hpp"
#include "game_inventory.hpp"
#include "game_player.hpp"
#include "game_render.hpp"
#include "game_sticky_aim.hpp"
#include "game_utility_packets.hpp"
#include <algorithm>
#include <atomic>
#include <cstring>
#include <mutex>
#include <random>
#include <set>
#include <string>
#include <vector>

namespace game {
enum{kInvKeyInventoryManager=23,kInvKeyAutoRefill=24};

// ---- settings and state
struct PbChestCfg{float delayLo=100,delayHi=200,startLo=150,startHi=300;int order=0;bool autoClose=true,skipMenus=true,onlyUseful=false,utility=false,mount=false,keepHotbar=true;int mode=0;
 std::set<std::string> ignored,kept,dumped;};
struct ManagerCfg{int mode=0,delay=100,sword=1,pick=2,axe=3;float blockLo=4,blockHi=9;std::set<std::string> drop;bool food=false,useless=false;};
struct RefillCfg{std::set<std::string> items{"minecraft:splash_potion"};int mode=0;bool close=false;float delayLo=50,delayHi=100,slotsLo=1,slotsHi=9;};
struct PbChestTrack{int32_t pos[3]{};bool hasPos=false;int64_t unique=0;bool hasUnique=false;int64_t at=0;bool valid=false;};
struct PbChestState{uint8_t window=0,type=0;std::vector<int> order;int64_t orderAt=0;bool orderValid=false,took=false,active=false;PbChestTrack track;double wait=0;int64_t waitAt=0;bool waitValid=false;};
struct PlayerBShared{std::recursive_mutex guard;
 bool stealer=false,filler=false,manager=false,refill=false;   // written through PlayerBEnable
 PbChestCfg stealCfg,fillCfg;ManagerCfg managerCfg;RefillCfg refillCfg;   // written by the menu under guard
 PbChestState steal,fill;
 int64_t imLastAt=0,imClickAt=0;bool imLastValid=false,imClickValid=false,imButtons[2]{};
 double arWait=0;int64_t arLastAt=0;bool arLastValid=false,arRefilled=false;
 std::atomic<bool> fault{false};};
inline PlayerBShared& PlayerB(){static auto* s=new PlayerBShared;return *s;}

// ---- item helpers
inline uint8_t PbCount(uint64_t stack){uint8_t n{};return stack&&Read(stack+0x22,&n)?n:0;}
inline bool PbName(uint64_t item,std::string* out){char t[160]{};if(!item||!Text(item+0xd8,t,sizeof(t)))return false;*out=t;return true;}
// List lookup: exact name, then "minecraft:" + name when the name has no namespace.
inline bool PbListed(const std::set<std::string>& list,uint64_t item){if(list.empty())return false;std::string n;if(!PbName(item,&n))return false;if(list.count(n))return true;
 return n.find(':')==std::string::npos&&list.count("minecraft:"+n)>0;}
inline bool PbEndsBare(uint64_t item,const char* suffix){std::string n;if(!PbName(item,&n))return false;const size_t colon=n.find(':');if(colon!=std::string::npos)n=n.substr(colon+1);
 const size_t l=strlen(suffix);return n.size()>=l&&!n.compare(n.size()-l,l,suffix);}
inline bool PbHasTag(uint64_t item,const char* tag){uint64_t b{},e{};char t[48]{};if(!item||!Read(item+0x1f8,&b)||!Read(item+0x200,&e)||e<b||(e-b)>0x30*256)return false;
 for(uint64_t at=b;at<e;at+=0x30)if(Text(at+8,t,sizeof(t))&&!strcmp(t,tag))return true;return false;}
inline bool PbIsSword(uint64_t item){return PbHasTag(item,"minecraft:is_sword");}
inline bool PbIsPickaxe(uint64_t item){return PbEndsBare(item,"_pickaxe");}
inline bool PbIsAxe(uint64_t item){return PbEndsBare(item,"_axe");}
inline int PbTier(uint64_t item){static const char* tiers[7]={"minecraft:netherite_tier","minecraft:diamond_tier","minecraft:iron_tier","minecraft:copper_tier","minecraft:stone_tier","minecraft:golden_tier","minecraft:wooden_tier"};
 for(int i=0;i<7;++i)if(PbHasTag(item,tiers[i]))return 7-i;return 0;}
inline bool PbFood(const Context& c,uint64_t item){const uint64_t f=Virtual0(c,item,0x98);return f&&reinterpret_cast<bool(*)(uint64_t)>(f)(item);}
inline bool PbBlockItem(uint64_t item){uint64_t b{};return item&&Read(item+0x178,&b)&&b;}
inline int PbEnch(const Context& c,uint64_t stack,int id){const int l=EnchantLevel(c,stack,(int16_t)id);return l==-1?0:l;}
inline float PbDamage(const Context& c,uint64_t item,uint64_t stack){const uint64_t f=Virtual0(c,item,0x128);const int base=f?reinterpret_cast<int(*)(uint64_t)>(f)(item):0;
 const int sharp=EnchantLevel(c,stack,9);return (sharp==-1?-0.0f:(float)sharp*1.25f)+(float)base;}
inline float PbArmorValue(const Context& c,uint64_t stack){std::string n;return PbName(StackItem(stack),&n)&&ArmorSlotOf(n)>=0?ArmorScore(c,stack):0.f;}
// True when the stack is armour whose worn piece (same slot of the armour container) is at least as good.
inline bool PbArmorNotBetter(const Context& c,uint64_t stack,uint64_t armor){std::string n;if(!armor||!PbName(StackItem(stack),&n))return false;const int slot=ArmorSlotOf(n);
 if(slot<0||slot>=ActionSize(c,armor))return false;const uint64_t worn=ActionStackAt(c,armor,slot);if(!StackItem(worn))return false;return PbArmorValue(c,stack)<=PbArmorValue(c,worn);}
inline std::vector<int> PbOrder(int from,int to,int mode){std::vector<int> v;for(int i=from;i<to;++i)v.push_back(i);
 if(mode==0&&v.size()>1){static std::mt19937_64 gen{std::random_device{}()};for(size_t i=1;i<v.size();++i){const size_t j=std::uniform_int_distribution<size_t>(0,i)(gen);if(j!=i)std::swap(v[i],v[j]);}}
 return v;}
inline bool PbPlannerBusy(){auto& p=Planner();std::lock_guard<std::recursive_mutex> lock(p.guard);return !p.records.empty();}
inline bool PbGameFocused(){DWORD pid=0;const HWND w=GetForegroundWindow();if(!w)return false;GetWindowThreadProcessId(w,&pid);return pid==GetCurrentProcessId();}

// ---- containers
inline bool PbChestEnum(uint8_t e){return e<0x3b&&((0x400000040000080ull>>e)&1);}   // 7, 0x1e, 0x3a
// Container of an enum name in the screen context (tables +0x108 then +0xc8).
inline uint64_t PbContainer(const Context& c,uint64_t mgr,uint8_t e){const uint64_t ctx=reinterpret_cast<uint64_t(*)(uint64_t)>(c.base+0x2b6c950)(mgr);if(!ctx)return 0;
 for(uint32_t off:{0x108u,0xc8u}){uint64_t head{},node{};if(!Read(ctx+off+8,&head)||!head||!Read(head,&node))continue;
  for(int g=0;g<4096&&node&&node!=head;++g){uint8_t key{};uint64_t holder{},box{};if(Read(node+0x10,&key)&&key==e&&Read(node+0x50,&holder)&&holder&&Read(holder+8,&box)&&box)return box;if(!Read(node,&node))break;}}
 return 0;}
inline void PbRelease(const Context& c,uint64_t control){if(!control)return;if(InterlockedDecrement((volatile long*)(control+8))!=0)return;
 if(const uint64_t destroy=Virtual0(c,control,0))reinterpret_cast<void(*)(uint64_t)>(destroy)(control);
 if(InterlockedDecrement((volatile long*)(control+0xc))==0)if(const uint64_t drop=Virtual0(c,control,8))reinterpret_cast<void(*)(uint64_t)>(drop)(control);}
// The window the module follows is the one shown: container manager model *(player+0x5c8), id +0x50, type +0x51,
// container found by enum name 7, 0x3a, then 0x1e.
inline bool PbOpenEnum(const Context& c,uintptr_t actor,uint8_t window,uint8_t type,int* e){uint64_t model{};uint8_t w{},t{};
 if(!Read(actor+0x5c8,&model)||!model||!Read(model+0x50,&w)||w!=window||!Read(model+0x51,&t)||t!=type)return false;
 for(int name:{7,0x3a,0x1e}){uint64_t out[2]{};reinterpret_cast<uint64_t*(*)(uint64_t,uint64_t*,int)>(c.base+0x3a6e6b0)(model,out,name);const bool found=out[0]!=0;PbRelease(c,out[1]);if(found){*e=name;return true;}}
 return false;}

// ---- transfer (type 1 = whole stack to an empty slot, type 2 = swap / place). Same request as InvMoveStack, except that
// the request is opened in screen mode (0) when a chest enum is involved or "inventory_screen" is shown, and type 1 is
// refused (1) when the destination is not empty. Results: 0 done, 1 refused, 2 busy, 4 same slot, 5 invalid.
inline int PbTransfer(const Context& c,uintptr_t actor,int type,uint8_t srcC,int srcSlot,uint8_t dstC,int dstSlot,int count){
 if(ActionCursorBusy(c,actor))return 2;uint64_t mgr{};if(!Read(actor+0xae0,&mgr)||!mgr)return 1;
 const uint64_t enabled=Virtual0(c,mgr,8);if(!enabled||!(reinterpret_cast<bool(*)(uint64_t)>(enabled)(mgr)))return 1;
 const bool screen=PbChestEnum(srcC)||PbChestEnum(dstC)||InvScreenOpen();
 bool valid=false;{uint64_t cur{};if(Read(mgr+0x68,&cur)&&!cur){reinterpret_cast<void(*)(uint64_t,int)>(c.base+0x2b71330)(mgr,screen?0:1);Read(mgr+0x68,&cur);valid=cur!=0;}}
 if(!valid)return 2;
 int result=5;InvSlot src{},dst{};
 if(count>=1&&count<=255&&InvResolve(c,mgr,srcC,srcSlot,&src)&&InvResolve(c,mgr,dstC,dstSlot,&dst)){
  const uint64_t local=ActionContainer(actor);
  const uint64_t srcStack=srcC==0xc?ActionStackAt(c,local,srcSlot):ActionStackAt(c,src.container,srcSlot),dstStackLocal=dstC==0xc?ActionStackAt(c,local,dstSlot):ActionStackAt(c,dst.container,dstSlot);
  const int have=PbCount(srcStack);
  if(!(count<=have&&(count==have||type!=2)))result=5;
  else if(src.container==dst.container&&srcSlot==dstSlot)result=4;
  else if(type==1&&(count!=have||StackItem(dstStackLocal)))result=1;
  else{const uint64_t dstModel=ActionStackAt(c,dst.container,dstSlot);const InvSlotInfo si=InvInfo(src,srcStack),di=InvInfo(dst,dstModel);
   alignas(16) uint8_t oldSrc[0x100]{},oldDst[0x100]{};auto copy=reinterpret_cast<void(*)(void*,uint64_t)>(c.base+0x1a52620);auto kill=[&](void* stack){reinterpret_cast<void(*)(void*,int)>(c.base+0x2f5950)(stack,0);};
   copy(oldSrc,srcStack);copy(oldDst,dstStackLocal);
   const uint64_t setSrc=Virtual0(c,src.container,0x68),setDst=Virtual0(c,dst.container,0x68);
   if(setSrc)reinterpret_cast<void(*)(uint64_t,int,void*,int)>(setSrc)(src.container,srcSlot,oldDst,0);if(setDst)reinterpret_cast<void(*)(uint64_t,int,void*,int)>(setDst)(dst.container,dstSlot,oldSrc,0);
   if(srcC==0xc&&local&&local!=src.container){const uint64_t f=Virtual0(c,local,0x68);if(f)reinterpret_cast<void(*)(uint64_t,int,void*,int)>(f)(local,srcSlot,oldDst,0);}
   if(dstC==0xc&&local&&local!=dst.container){const uint64_t f=Virtual0(c,local,0x68);if(f)reinterpret_cast<void(*)(uint64_t,int,void*,int)>(f)(local,dstSlot,oldSrc,0);}
   const bool place=type==1||ActionItem((uint64_t)oldDst)==0;invWriteAddress=c.base+0x2b80b40;uint8_t* action=place?InvNewAction(1,(uint8_t)count,si,di):InvNewAction(2,0,si,di);
   uint8_t* owned=action;reinterpret_cast<void(*)(uint64_t,uint8_t**)>(c.base+0x2b72b40)(mgr,&owned);if(owned)InvActionDtor(owned,1);
   kill(oldDst);kill(oldSrc);result=0;}}
 reinterpret_cast<void(*)(uint64_t)>(c.base+0x2b71680)(mgr);reinterpret_cast<void(*)(uint64_t)>(c.base+0x2b72860)(mgr);
 return result;}

// ---- Chest Stealer / Chest Filler
inline void PbChestReset(PbChestState& s){s.order.clear();s.order.shrink_to_fit();s.active=false;}
inline int PbChestClass(uint8_t type){if(type<0x23){if((0x9c0ull>>type)&1)return 1;if((0x400000401ull>>type)&1)return 0;if(type==0xc)return 2;}return 3;}
inline void PbChestTrackSend(const Context& c,PbChestTrack& t,uintptr_t packet){uint64_t tr{};int32_t kind{},action{};if(!Read(packet+0x170,&tr)||!tr||!Read(tr+8,&kind))return;
 if(kind==3){uint64_t rid{};uintptr_t actor{};if(!Read(tr+0x70,&action)||action!=0||!Read(tr+0x68,&rid)||!ActorByRuntimeId(c.registry,rid,&actor)||!actor)return;
  uintptr_t registry{},element{};uint32_t entity{};uint64_t unique{};t.hasPos=false;t.hasUnique=ActorEntity(actor,&registry,&entity)&&Element(registry,entity,"ActorUniqueIDComponent",8,&element)&&Read(element,&unique);
  t.unique=t.hasUnique?(int64_t)unique:0;t.at=ActionNs();t.valid=true;return;}
 if(kind==2&&Read(tr+0x68,&action)&&action==0){Read(tr+0x70,&t.pos[0]);Read(tr+0x74,&t.pos[1]);Read(tr+0x78,&t.pos[2]);t.hasPos=true;t.hasUnique=false;t.at=ActionNs();t.valid=true;}}
// Skip Menus: only windows answering a click of the last 1000 ms on the same block / entity.
inline bool PbChestAccepted(const PbChestTrack& t,uintptr_t packet,bool skip){if(!skip)return true;if(ActionMs(t.at)>1000.0)return false;int64_t who{};if(!Read(packet+0x40,&who))return false;
 if(who==-1){int32_t p[3]{};if(!t.hasPos||!Read(packet+0x34,&p[0])||!Read(packet+0x38,&p[1])||!Read(packet+0x3c,&p[2]))return false;const int dx=t.pos[0]-p[0],dy=t.pos[1]-p[1],dz=t.pos[2]-p[2];return (uint32_t)(dz*dz+dy*dy+dx*dx)<=1u;}
 return t.hasUnique&&t.unique==who;}
inline void PbChestOpen(PbChestState& s,uintptr_t packet,float startLo,float startHi){Read(packet+0x30,&s.window);Read(packet+0x31,&s.type);s.active=true;s.wait=ActionRandom(startLo,startHi);s.waitAt=ActionNs();s.waitValid=true;}
inline void PbChestClose(const Context& c,const PbChestState& s){const uint8_t w=s.window,t=s.type;InvSend(c,0x2f,[&](uint64_t p){*(uint8_t*)(p+0x30)=w;*(uint8_t*)(p+0x31)=t;*(uint8_t*)(p+0x32)=0;});}
inline void PbChestStealerReceive(PlayerBShared& s,uintptr_t packet,int id){auto& st=s.steal;const PbChestCfg& k=s.stealCfg;
 if(id==0x2f){if(st.active)PbChestReset(st);return;}
 if(st.active)PbChestReset(st);uint8_t type{};if(!Read(packet+0x31,&type))return;const int cls=PbChestClass(type);
 if(cls==3||(cls==1&&!k.utility)||(cls==2&&!k.mount))return;if(!PbChestAccepted(st.track,packet,k.skipMenus))return;
 st.order.clear();st.orderAt=ActionNs();st.orderValid=true;st.took=false;PbChestOpen(st,packet,k.startLo,k.startHi);}
inline void PbChestFillerReceive(PlayerBShared& s,uintptr_t packet,int id){auto& st=s.fill;const PbChestCfg& k=s.fillCfg;
 if(id==0x2f){if(st.active)PbChestReset(st);return;}
 if(st.active)PbChestReset(st);uint8_t type{};if(!Read(packet+0x31,&type))return;const int cls=PbChestClass(type);
 if(cls>=2||(cls==1&&!k.utility))return;if(s.stealer)return;   // never together with an enabled Chest Stealer
 Context c{};if(!Local(&c))return;if(!PbChestAccepted(st.track,packet,k.skipMenus))return;
 st.order=PbOrder(k.keepHotbar?9:0,ActionSize(c,ActionContainer(c.actor)),k.order);PbChestOpen(st,packet,k.startLo,k.startHi);}
// 1 = keep waiting before Auto Close: nothing taken yet, window under 1000 ms old and the container still empty.
inline bool PbChestStealerWaits(const Context& c,const PbChestState& st,uint64_t box){if(st.took)return false;if(st.orderValid&&ActionMs(st.orderAt)>1000.0)return false;
 for(int i=0,n=ActionSize(c,box);i<n;++i)if(StackItem(ActionStackAt(c,box,i)))return false;return true;}
inline bool PbChestStealerUseless(const Context& c,uintptr_t actor,uint64_t stack,uint64_t item,uint64_t inv){int kind=PbIsSword(item)?1:(PbIsPickaxe(item)?2:(PbIsAxe(item)?3:0));
 if(!kind){const uint64_t armor=EquipmentContainer(c,actor,1);return armor&&PbArmorNotBetter(c,stack,armor);}
 const int rank=PbTier(item);for(int i=0,n=ActionSize(c,inv);i<n;++i){const uint64_t other=StackItem(ActionStackAt(c,inv,i));if(!other)continue;
  const bool same=kind==1?PbIsSword(other):(kind==2?PbIsPickaxe(other):PbIsAxe(other));if(same&&PbTier(other)>=rank)return true;}
 return false;}
inline int PbFreeSlot(const Context& c,uint64_t inv){const int m=ActionSize(c,inv);if(m>=10)for(int i=9;i<m;++i)if(!StackItem(ActionStackAt(c,inv,i)))return i;
 for(int i=0;i<(m<9?m:9);++i)if(!StackItem(ActionStackAt(c,inv,i)))return i;return -1;}
inline void PbChestStealerFrame(){auto& s=PlayerB();std::lock_guard<std::recursive_mutex> lock(s.guard);auto& st=s.steal;if(!s.stealer||!st.active)return;const PbChestCfg& k=s.stealCfg;
 Context c{};if(!Local(&c)){PbChestReset(st);return;}int e{};if(!PbOpenEnum(c,c.actor,st.window,st.type,&e))return;
 if(!st.waitValid||ActionMs(st.waitAt)<=st.wait)return;if(ActionCursorBusy(c,c.actor))return;
 uint64_t mgr{};if(!Read(c.actor+0xae0,&mgr)||!mgr)return;const uint64_t box=PbContainer(c,mgr,(uint8_t)e);if(!box)return;const int n=ActionSize(c,box);
 if(n!=(int)st.order.size()){st.order=PbOrder(0,n,k.order);st.orderAt=ActionNs();st.orderValid=true;}
 const uint64_t inv=ActionContainer(c.actor);const std::vector<int> order=st.order;
 for(const int slot:order){if(slot>=n)continue;const uint64_t stack=ActionStackAt(c,box,slot);const uint64_t item=StackItem(stack);if(!item||!PbCount(stack))continue;
  if(PbListed(k.ignored,item))continue;if(k.onlyUseful&&PbChestStealerUseless(c,c.actor,stack,item,inv))continue;
  const int free=PbFreeSlot(c,inv);if(free<0)break;
  if(PbTransfer(c,c.actor,1,(uint8_t)e,slot,0xc,free,PbCount(stack))==0){st.took=true;st.wait=ActionRandom(k.delayLo,k.delayHi);st.waitAt=ActionNs();st.waitValid=true;}
  return;}
 if(k.autoClose&&!PbChestStealerWaits(c,st,box)){PbChestClose(c,st);PbChestReset(st);}}
inline void PbChestFillerFrame(){auto& s=PlayerB();std::lock_guard<std::recursive_mutex> lock(s.guard);auto& st=s.fill;if(!s.filler||!st.active)return;const PbChestCfg& k=s.fillCfg;
 Context c{};if(!Local(&c)){PbChestReset(st);return;}int e{};if(!PbOpenEnum(c,c.actor,st.window,st.type,&e))return;
 if(!st.waitValid||ActionMs(st.waitAt)<=st.wait)return;if(ActionCursorBusy(c,c.actor))return;
 uint64_t mgr{};if(!Read(c.actor+0xae0,&mgr)||!mgr)return;const uint64_t box=PbContainer(c,mgr,(uint8_t)e);if(!box)return;
 const uint64_t inv=ActionContainer(c.actor);const std::vector<int> order=st.order;
 for(const int slot:order){if(slot>=ActionSize(c,inv))continue;const uint64_t stack=ActionStackAt(c,inv,slot);const uint64_t item=StackItem(stack);if(!item||!PbCount(stack))continue;
  if(k.mode==1&&PbListed(k.kept,item))continue;if(k.mode==2&&!PbListed(k.dumped,item))continue;
  int free=-1;for(int i=0,n=ActionSize(c,box);i<n;++i)if(!StackItem(ActionStackAt(c,box,i))){free=i;break;}if(free<0)break;
  if(PbTransfer(c,c.actor,1,0xc,slot,(uint8_t)e,free,PbCount(stack))==0){st.wait=ActionRandom(k.delayLo,k.delayHi);st.waitAt=ActionNs();st.waitValid=true;}
  return;}
 if(k.autoClose){PbChestClose(c,st);PbChestReset(st);}}

// ---- Inventory Manager
struct ManagerPlan{int sword=-1,pick=-1,axe=-1,low=0,high=-1;const std::set<std::string>* drop=nullptr;bool food=false,useless=false;uint64_t inv=0,armor=0;int lock=-1,bestSword=-1,bestPick=-1,bestAxe=-1;};
struct ManagerAction{int kind=0,src=-1,dst=-1,op=0;bool present=false;};
inline bool ManagerEligible(const Context& c,const ManagerPlan& p,uint64_t item){return !PbListed(*p.drop,item)&&(!p.food||!PbFood(c,item));}
inline int ManagerBestTool(const Context& c,const ManagerPlan& p,bool(*kind)(uint64_t),int preferred){const int n=ActionSize(c,p.inv);int best=-1,key[3]={-1,-1,-1};
 for(int i=0;i<n;++i){const uint64_t stack=ActionStackAt(c,p.inv,i);const uint64_t item=StackItem(stack);if(!item||!kind(item)||!ManagerEligible(c,p,item))continue;
  const int k[3]={PbTier(item),PbEnch(c,stack,15)+PbEnch(c,stack,18)+PbEnch(c,stack,16)+PbEnch(c,stack,17)+PbEnch(c,stack,26),ArmorDurability(c,stack)};
  const bool greater=std::lexicographical_compare(key,key+3,k,k+3),equal=std::equal(k,k+3,key);if(greater||(equal&&preferred==i)){best=i;memcpy(key,k,sizeof(key));}}
 return best;}
// Plan: preferred slots (0-based), block range, lists, inventory and armour containers, locked selected slot, best tools.
inline void ManagerBuildPlan(const Context& c,uintptr_t actor,const ManagerCfg& k,bool lock,ManagerPlan* p){p->sword=k.sword-1;p->pick=k.pick-1;p->axe=k.axe-1;p->low=(int)k.blockLo-1;p->high=(int)k.blockHi-1;
 p->drop=&k.drop;p->food=k.food;p->useless=k.useless;p->inv=ActionContainer(actor);p->armor=EquipmentContainer(c,actor,1);p->lock=-1;if(lock)if(int32_t* sel=ActionSelected(actor))p->lock=*sel;
 const int n=ActionSize(c,p->inv);int rank=-1,ench=-1,dur=-1;float dmg=-3.4028235e38f;p->bestSword=-1;
 for(int i=0;i<n;++i){const uint64_t stack=ActionStackAt(c,p->inv,i);const uint64_t item=StackItem(stack);if(!item||!PbIsSword(item)||!ManagerEligible(c,*p,item))continue;
  const int r=PbTier(item);const float d=PbDamage(c,item,stack);const int e=PbEnch(c,stack,9)+PbEnch(c,stack,14)+PbEnch(c,stack,13)+PbEnch(c,stack,12)+PbEnch(c,stack,17)+PbEnch(c,stack,26);const int u=ArmorDurability(c,stack);
  bool take;if(r==rank){int cmp=d>dmg?1:-1;if(d==dmg)cmp=e!=ench?(e>ench?1:-1):(u!=dur?(u>dur?1:-1):0);take=cmp>0||(cmp==0&&p->sword==i);}else take=r>rank;
  if(take){p->bestSword=i;rank=r;dmg=d;ench=e;dur=u;}}
 p->bestPick=ManagerBestTool(c,*p,PbIsPickaxe,p->pick);p->bestAxe=ManagerBestTool(c,*p,PbIsAxe,p->axe);}
// Block source: largest block stack outside the block range and the tool / locked slots (first one on ties).
inline int ManagerBlockSource(const Context& c,const ManagerPlan& p){int best=-1,count=0;
 for(int i=0,n=ActionSize(c,p.inv);i<n;++i){if(i==p.lock||!(i<p.low||p.high<i)||i==p.sword||i==p.pick||i==p.axe)continue;const uint64_t stack=ActionStackAt(c,p.inv,i);const uint64_t item=StackItem(stack);
  if(!item||!PbBlockItem(item)||PbListed(*p.drop,item)||(p.food&&PbFood(c,item)))continue;const int have=PbCount(stack);if(count<have){count=have;best=i;}}
 return best;}
inline ManagerAction ManagerDecide(const Context& c,const ManagerPlan& p){ManagerAction a;const uint64_t inv=p.inv;const int n=ActionSize(c,inv),L=p.lock;
 auto move=[&](int src,int dst){a.kind=0;a.src=src;a.dst=dst;a.op=StackItem(ActionStackAt(c,inv,dst))?2:1;a.present=true;return a;};
 if(!(p.bestSword==p.sword||p.bestSword<0||(L>=0&&(p.sword==L||p.bestSword==L))))return move(p.bestSword,p.sword);
 if(!(p.bestPick<0||(p.bestSword>=0&&p.pick==p.sword)||p.bestPick==p.pick||(L>=0&&(p.pick==L||p.bestPick==L))))return move(p.bestPick,p.pick);
 if(!(p.bestAxe<0||(p.bestPick>=0&&p.axe==p.pick)||(p.bestSword>=0&&p.axe==p.sword)||p.bestAxe==p.axe||(L>=0&&(p.axe==L||p.bestAxe==L))))return move(p.bestAxe,p.axe);
 if(p.low<=p.high)for(int i=p.low;i<n;++i){
  if(i!=L&&i!=p.sword&&i!=p.pick&&i!=p.axe){const uint64_t item=StackItem(ActionStackAt(c,inv,i));
   if(!(item&&PbBlockItem(item)&&!PbListed(*p.drop,item)&&(!p.food||!PbFood(c,item)))){
    if(i>=0){const int src=ManagerBlockSource(c,p);if(src>=0&&src!=i&&(L<0||(src!=L&&L!=i)))return move(src,i);}
    break;}}
  if(p.high<=i)break;}
 bool food=false,useless=false;int foodSlot=-1,uselessSlot=-1;
 for(int i=0;i<n;++i){const uint64_t stack=ActionStackAt(c,inv,i);const uint64_t item=StackItem(stack);if(!item||i==L)continue;
  if(PbListed(*p.drop,item)){a.kind=1;a.src=i;a.dst=-1;a.op=0;a.present=true;return a;}
  if(p.food&&PbFood(c,item)){if(!food)foodSlot=i;food=true;}
  else if(!useless&&p.useless){const int best=PbIsSword(item)?p.bestSword:(PbIsPickaxe(item)?p.bestPick:(PbIsAxe(item)?p.bestAxe:-2));
   const bool bad=best!=-2&&i!=best?true:(p.armor&&PbArmorNotBetter(c,stack,p.armor));if(bad){useless=true;uselessSlot=i;}}}
 a.kind=1;a.dst=-1;a.op=0;if(food){a.src=foodSlot;a.present=true;}else{a.src=uselessSlot;a.present=useless;}return a;}
// Hotbar slots an enabled Inventory Manager keeps for itself (Auto Refill): plan without lock.
inline unsigned ManagerReserved(const Context& c,uintptr_t actor,const ManagerCfg& k){ManagerPlan p;ManagerBuildPlan(c,actor,k,false,&p);const int src=ManagerBlockSource(c,p);unsigned mask=0;
 for(int j=0;j<9;++j){const bool r=(j==p.sword&&p.bestSword>=0)||(j==p.pick&&p.bestPick>=0)||(j==p.axe&&p.bestAxe>=0)||(j!=p.sword&&j!=p.pick&&j!=p.axe&&p.low<=j&&j<=p.high&&src>=0);if(r)mask|=1u<<j;}
 return mask;}
inline void ManagerStep(const Context& c,uintptr_t actor,const ManagerCfg& k,bool lock){if(ActionCursorBusy(c,actor))return;ManagerPlan p;ManagerBuildPlan(c,actor,k,lock,&p);const ManagerAction a=ManagerDecide(c,p);if(!a.present)return;
 if(a.kind==1)reinterpret_cast<void(*)(uint64_t,int,bool,bool,bool)>(c.base+0x2af140)(p.inv,a.src,false,true,false);   // Inventory::dropSlot
 else{if(a.op==0)return;if(PbTransfer(c,actor,a.op==2?2:1,0xc,a.src,0xc,a.dst,PbCount(ActionStackAt(c,p.inv,a.src)))!=0)return;}
 auto& s=PlayerB();std::lock_guard<std::recursive_mutex> l(s.guard);s.imLastAt=ActionNs();s.imLastValid=true;}
inline void ManagerFrame(){auto& s=PlayerB();ManagerCfg k;bool due;const bool open=InvScreenOpen();
 {std::lock_guard<std::recursive_mutex> lock(s.guard);if(!s.manager)return;k=s.managerCfg;
  // mouse button changes with the inventory shown (left / right, press or release)
  const bool focus=PbGameFocused();const bool b[2]={focus&&(GetAsyncKeyState(VK_LBUTTON)&0x8000)!=0,focus&&(GetAsyncKeyState(VK_RBUTTON)&0x8000)!=0};
  if(open&&(b[0]!=s.imButtons[0]||b[1]!=s.imButtons[1])){s.imClickAt=ActionNs();s.imClickValid=true;}s.imButtons[0]=b[0];s.imButtons[1]=b[1];
  due=s.imLastValid&&(double)k.delay<ActionMs(s.imLastAt);}
 if(!due)return;Context c{};if(!Local(&c))return;
 if(open){InvCancel(kInvKeyInventoryManager);bool click;{std::lock_guard<std::recursive_mutex> lock(s.guard);click=s.imClickValid&&ActionMs(s.imClickAt)>100.0;}if(click)ManagerStep(c,c.actor,k,false);return;}
 if(k.mode!=1||!StickyHasInput(c)||ActionCursorBusy(c,c.actor))return;ManagerPlan p;ManagerBuildPlan(c,c.actor,k,true,&p);if(!ManagerDecide(c,p).present)return;
 InvSubmit(kInvKeyInventoryManager,[](uintptr_t actor){auto& sh=PlayerB();ManagerCfg kk;{std::lock_guard<std::recursive_mutex> lock(sh.guard);if(!sh.manager)return;kk=sh.managerCfg;}
  Context x{};if(!Local(&x)||x.actor!=actor||!StickyHasInput(x))return;ManagerStep(x,actor,kk,true);});}

// ---- Auto Refill
struct RefillPlan{int src=-1,dst=-1;bool present=false;};
inline RefillPlan RefillChoose(const Context& c,uintptr_t actor,const RefillCfg& k,bool manager,const ManagerCfg& mk){RefillPlan r;const uint64_t inv=ActionContainer(actor);const int n=ActionSize(c,inv);int src=-1;
 for(int i=9;i<n;++i)if(PbListed(k.items,StackItem(ActionStackAt(c,inv,i)))){src=i;break;}if(src<0)return r;
 const int low=(int)k.slotsLo;int high=(int)k.slotsHi;if(n<high)high=n;if(high>9)high=9;const unsigned reserved=manager?ManagerReserved(c,actor,mk):0u;
 if(low<=high)for(int d=low-1;d<high;++d){if(d<0||StackItem(ActionStackAt(c,inv,d)))continue;if(d>8)break;if(!((reserved>>d)&1)){r.src=src;r.dst=d;r.present=true;return r;}}
 return r;}
inline bool RefillExec(const Context& c,uintptr_t actor,const RefillPlan& p,const RefillCfg& k){const uint64_t inv=ActionContainer(actor);
 if(PbTransfer(c,actor,1,0xc,p.src,0xc,p.dst,PbCount(ActionStackAt(c,inv,p.src)))!=0)return false;
 auto& s=PlayerB();std::lock_guard<std::recursive_mutex> lock(s.guard);s.arWait=ActionRandom(k.delayLo,k.delayHi);s.arLastAt=ActionNs();s.arLastValid=true;return true;}
// Close request for the shown screen: (ScreenView+0x40)->vtable[0x80](const std::string& name).
inline void RefillCloseScreen(const Context& c){void* view=screenViewCurrent;uint64_t owner{};if(!view||!Read((uintptr_t)view+0x40,&owner)||!owner)return;const uint64_t f=Virtual0(c,owner,0x80);if(!f)return;
 static char name[32]="inventory_screen";struct{const char* data;uint64_t pad,size,capacity;} text{name,0,16,31};reinterpret_cast<void(*)(uint64_t,void*)>(f)(owner,&text);}
inline void RefillFrame(){auto& s=PlayerB();{std::lock_guard<std::recursive_mutex> lock(s.guard);if(!s.refill)return;}
 RefillCfg k;ManagerCfg mk;bool manager,due;const bool open=InvScreenOpen();Context c{};if(!Local(&c))return;
 {std::lock_guard<std::recursive_mutex> lock(s.guard);if(!s.refill)return;k=s.refillCfg;manager=s.manager;if(manager)mk=s.managerCfg;if(!open)s.arRefilled=false;due=s.arLastValid&&s.arWait<ActionMs(s.arLastAt);}
 if(!due)return;
 if(open){InvCancel(kInvKeyAutoRefill);if(ActionCursorBusy(c,c.actor)||PbPlannerBusy())return;const RefillPlan p=RefillChoose(c,c.actor,k,manager,mk);
  if(p.present){if(RefillExec(c,c.actor,p,k)){std::lock_guard<std::recursive_mutex> lock(s.guard);s.arRefilled=true;}return;}
  bool refilled;{std::lock_guard<std::recursive_mutex> lock(s.guard);refilled=s.arRefilled;}if(refilled&&k.mode==0&&k.close)RefillCloseScreen(c);return;}
 if(k.mode!=1||!StickyHasInput(c)||ActionCursorBusy(c,c.actor)||PbPlannerBusy()||!RefillChoose(c,c.actor,k,manager,mk).present)return;
 InvSubmit(kInvKeyAutoRefill,[](uintptr_t actor){auto& sh=PlayerB();RefillCfg kk;ManagerCfg mm;bool mgr;{std::lock_guard<std::recursive_mutex> lock(sh.guard);if(!sh.refill||sh.refillCfg.mode!=1)return;kk=sh.refillCfg;mgr=sh.manager;if(mgr)mm=sh.managerCfg;}
  Context x{};if(!Local(&x)||x.actor!=actor||!StickyHasInput(x)||ActionCursorBusy(x,actor)||PbPlannerBusy())return;const RefillPlan p=RefillChoose(x,actor,kk,mgr,mm);if(p.present)RefillExec(x,actor,p,kk);});}

// ---- entry points
// Menu thread, every frame: enable / disable edges (Chest Stealer and Chest Filler drop their window both ways; Inventory
// Manager and Auto Refill restart their timers when turned on).
inline void PlayerBEnable(bool stealer,bool filler,bool manager,bool refill){auto& s=PlayerB();std::lock_guard<std::recursive_mutex> lock(s.guard);
 if(stealer!=s.stealer)PbChestReset(s.steal);if(filler!=s.filler)PbChestReset(s.fill);
 if(manager&&!s.manager){s.imLastAt=ActionNs();s.imLastValid=true;s.imClickAt=ActionNs();s.imClickValid=true;}
 if(refill&&!s.refill){s.arWait=0;s.arLastAt=ActionNs();s.arLastValid=true;s.arRefilled=false;}
 s.stealer=stealer;s.filler=filler;s.manager=manager;s.refill=refill;}
// Screen render (inventory frame, before the inventory scheduler runs its queue).
inline void PlayerBFrameWork(){PbChestStealerFrame();PbChestFillerFrame();ManagerFrame();RefillFrame();}
inline void PlayerBFrame(){if(PlayerB().fault)return;__try{PlayerBFrameWork();}__except(EXCEPTION_EXECUTE_HANDLER){PlayerB().fault=true;if(actionLog)actionLog("Player B: fault caught, Chest Stealer / Chest Filler / Inventory Manager / Auto Refill stopped until restart");}}
// Packet send hook: interaction tracking and ContainerClose (observation only).
inline void PlayerBSendWork(uintptr_t packet){const int id=PacketIdOf(packet);if(id!=0x2f&&id!=0x1e)return;auto& s=PlayerB();std::lock_guard<std::recursive_mutex> lock(s.guard);if(!s.stealer&&!s.filler)return;
 if(id==0x2f){if(s.stealer&&s.steal.active)PbChestReset(s.steal);if(s.filler&&s.fill.active)PbChestReset(s.fill);return;}
 Context c{};if(!Local(&c))return;if(s.stealer)PbChestTrackSend(c,s.steal.track,packet);if(s.filler)PbChestTrackSend(c,s.fill.track,packet);}
inline void PlayerBSend(uintptr_t packet){static volatile long stopped_=0;if(PlayerB().fault)return;if(!stopped_)__try{PlayerBSendWork(packet);}__except(game::FeatureFault("PlayerBSend",&stopped_,GetExceptionInformation())){}}
// Packet receive: ContainerOpen / ContainerClose (observation only).
inline void PlayerBReceiveWork(uintptr_t packet){const int id=PacketIdOf(packet);if(id!=0x2e&&id!=0x2f)return;auto& s=PlayerB();std::lock_guard<std::recursive_mutex> lock(s.guard);
 if(s.stealer)PbChestStealerReceive(s,packet,id);if(s.filler)PbChestFillerReceive(s,packet,id);}
inline void PlayerBReceive(uintptr_t packet){static volatile long stopped_=0;if(PlayerB().fault)return;if(!stopped_)__try{PlayerBReceiveWork(packet);}__except(game::FeatureFault("PlayerBReceive",&stopped_,GetExceptionInformation())){}}
}
