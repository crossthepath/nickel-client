// Inventory transactions used by Replenish, Auto Totem and Auto Armor.
// - Scheduler: with "inventory_screen" open an action runs immediately; otherwise it is queued (one per module) and,
// when idle, InteractPacket 0x21 (action 6 "open inventory", runtime id at +0x38) is sent. The ContainerOpenPacket
// 0x2e of type -1 that answers is remembered and cancelled (no screen opens); the queue then runs on the next frame
// and the window is closed with ContainerClosePacket 0x2f { window, type, 0 }. Without an answer within 1000 ms the
// queue is cleared and the request resent.
// - ItemStackRequest (move, type 2): client handler at *(player+0xae0), active if vtable[1]; game function at RVA
// 0x2b71330 (handler, no screen) when +0x68 is empty. Containers are resolved by RVA 0x2b6c950 (tables +0x108,
// +0xc8, then vector +0x98); both stacks are swapped locally (container vtable[0x68]); a transfer action (0x68
// bytes, own vtable, written by RVA 0x2b80b40) is added through RVA 0x2b72b40: Place (1, count) if the destination
// is empty, Swap (2) otherwise; finished by RVA 0x2b71680 then RVA 0x2b72860.
// The legacy transaction path (servers without ItemStackRequest) is not implemented; ActionCursorBusy guards against
// an occupied cursor.
#pragma once
#include "game_actions.hpp"
#include <functional>
#include <map>

namespace game {
// ---- scheduler
struct InvScheduler{std::recursive_mutex guard;int state=0;bool closing=false,known=false,retry=false;uint8_t window=0,type=0;int64_t sentAt=0;
 std::map<int,std::function<void(uintptr_t)>> queue;std::atomic<bool> fault{false};};
inline InvScheduler& Inventory(){static auto* s=new InvScheduler;return *s;}
inline void InvSend(const Context& c,int id,const std::function<void(uint64_t)>& fill){GamePacket p=CreatePacket(c,id);if(p.object){fill(p.object);SendPacket(c,p.object);}ReleasePacket(p);}
inline void InvOpen(const Context& c){uint64_t rid{};if(!RuntimeId(c.actor,&rid))return;InvSend(c,0x21,[&](uint64_t p){*(uint8_t*)(p+0x30)=6;*(uint64_t*)(p+0x38)=rid;*(uint8_t*)(p+0x4c)=0;});}
inline void InvReset(InvScheduler& s){s.queue.clear();s.state=0;s.closing=false;s.known=false;s.retry=true;}
inline void InvClose(InvScheduler& s,const Context* c){const bool known=s.known;const uint8_t w=s.window,t=s.type;InvReset(s);
 if(known&&c)InvSend(*c,0x2f,[&](uint64_t p){*(uint8_t*)(p+0x30)=w;*(uint8_t*)(p+0x31)=t;*(uint8_t*)(p+0x32)=0;});}
inline bool InvScreenOpen(){return !strcmp(currentScreen,"inventory_screen");}
inline void InvSubmit(int key,std::function<void(uintptr_t)> fn){auto& s=Inventory();std::lock_guard<std::recursive_mutex> lock(s.guard);Context c{};if(!Local(&c))return;
 if(InvScreenOpen()){s.queue.erase(key);fn(c.actor);return;}
 if(s.closing)return;s.queue[key]=std::move(fn);if(s.state==0){s.state=1;s.sentAt=ActionNs();s.retry=true;InvOpen(c);}}
inline bool InvReceiveWork(uintptr_t packet){auto& s=Inventory();std::lock_guard<std::recursive_mutex> lock(s.guard);if(s.state!=1)return false;
 uint64_t t{},g{};if(!Read(packet,&t)||!t||!Read(t+8,&g)||!g||reinterpret_cast<int(*)(uintptr_t)>(g)(packet)!=0x2e)return false;
 if(*(int8_t*)(packet+0x31)!=-1)return false;s.state=2;s.window=*(uint8_t*)(packet+0x30);s.type=*(uint8_t*)(packet+0x31);s.known=true;s.sentAt=ActionNs();
 if(s.closing){Context c{};InvClose(s,Client(&c)?&c:nullptr);}return true;}
inline void InvRenderWork(){auto& s=Inventory();std::lock_guard<std::recursive_mutex> lock(s.guard);if(s.state==0)return;Context c{};if(!Local(&c)){InvReset(s);return;}
 if(s.queue.empty()){if(s.state==2){InvClose(s,&c);return;}s.closing=true;}
 if(s.state==1){if(!s.retry||ActionMs(s.sentAt)<=1000.0)return;s.queue.clear();s.closing=true;s.sentAt=ActionNs();s.retry=true;InvOpen(c);return;}
 if(s.state==2){auto work=std::move(s.queue);s.queue.clear();for(auto& [k,f]:work)if(f)f(c.actor);InvClose(s,&c);}}

// ---- ItemStackRequest (move, type 2)
struct InvSlot{uint64_t name=0;int32_t extra=0;uint64_t container=0;uint8_t slot=0;bool ok=false;};
inline bool InvResolve(const Context& c,uint64_t mgr,uint8_t container,int slot,InvSlot* out){const uint8_t e=container==0xc?(slot<9?0x1c:0x1d):container;
 uint64_t ctx=reinterpret_cast<uint64_t(*)(uint64_t)>(c.base+0x2b6c950)(mgr);if(!ctx)return false;bool found=false;
 for(uint32_t off:{0x108u,0xc8u}){uint64_t head{};if(!Read(ctx+off+8,&head)||!head)continue;uint64_t node{};Read(head,&node);
  for(int g=0;g<4096&&node&&node!=head;++g){uint8_t key{};uint64_t holder{},cont{};if(Read(node+0x10,&key)&&key==e&&Read(node+0x50,&holder)&&holder&&Read(holder+8,&cont)&&cont){
    Read(node+0x10,&out->name);Read(node+0x18,&out->extra);out->container=cont;found=true;break;}if(!Read(node,&node))break;}
  if(found)break;}
 if(!found){int idx=-1;if(e==0x1c||e==0x1d)idx=3;else if(e==6)idx=0;else if(e==0x22)idx=1;uint64_t b{},en{},holder{},cont{};
  if(idx<0||!Read(ctx+0x98,&b)||!Read(ctx+0xa0,&en)||(uint64_t)idx>=(en-b)/8||!Read(b+idx*8,&holder)||!holder||!Read(holder+8,&cont)||!cont)return false;out->name=e;out->extra=0;out->container=cont;}
 if(slot<0||slot>0xff||slot>=ActionSize(c,out->container))return false;out->slot=(uint8_t)slot;out->ok=true;return true;}
// ItemStackRequestSlotInfo: name (12 bytes), slot, stack network id variant (+0x80).
struct alignas(8) InvSlotInfo{uint64_t name;int32_t extra;uint8_t slot;uint8_t pad[3];uint64_t a;int32_t b;int32_t pad2;int8_t index;uint8_t pad3[7];};
inline InvSlotInfo InvInfo(const InvSlot& r,uint64_t stack){InvSlotInfo i{};i.name=r.name;i.extra=r.extra;i.slot=r.slot;i.index=-1;const uint64_t v=stack+0x80;int8_t idx=-1;Read(v+0x10,&idx);
 if(idx==0){int32_t x{};Read(v,&x);i.a=(uint32_t)x;i.index=0;}else if(idx==1||idx==2){Read(v,&i.a);Read(v+8,&i.b);i.index=idx;}return i;}
// Transfer action with our own vtable.
inline uint64_t invWriteAddress=0;
inline void InvVariantDtor(uint8_t* var,int8_t idx){if((uint32_t)(idx+1)>=2){const uint64_t table=*(uint64_t*)var;reinterpret_cast<void(*)(void*,int)>(*(uint64_t*)table)(var,0);}}
inline void* InvActionDtor(uint8_t* self,int flags){InvVariantDtor(self+0x50,*(int8_t*)(self+0x60));InvVariantDtor(self+0x28,*(int8_t*)(self+0x38));if(flags&1)::operator delete(self);return self;}
inline int InvActionZero(void*){return 0;}inline int InvActionMinus(void*){return -1;}inline void InvActionNothing(void*){}
inline void InvActionWrite(void* self,void* stream){reinterpret_cast<void(*)(void*,void*)>(invWriteAddress)(self,stream);}
inline void* invActionTable[6]={(void*)InvActionDtor,(void*)InvActionZero,(void*)InvActionMinus,(void*)InvActionNothing,(void*)InvActionWrite,(void*)InvActionZero};
inline void InvPutInfo(uint8_t* at,const InvSlotInfo& i){memcpy(at,&i.name,8);memcpy(at+8,&i.extra,4);at[0xc]=i.slot;*(int8_t*)(at+0x20)=-1;
 if(i.index==0){*(int32_t*)(at+0x10)=(int32_t)i.a;}else if(i.index>0){*(uint64_t*)(at+0x10)=i.a;*(int32_t*)(at+0x18)=i.b;}*(int8_t*)(at+0x20)=i.index;}
inline uint8_t* InvNewAction(uint8_t type,uint8_t count,const InvSlotInfo& src,const InvSlotInfo& dst){uint8_t* a=(uint8_t*)::operator new(0x68);memset(a,0,0x68);*(void***)a=invActionTable;
 a[8]=type;a[0x10]=1;a[0x11]=type!=2;a[0x12]=count;InvPutInfo(a+0x18,src);InvPutInfo(a+0x40,dst);return a;}
// Move a whole stack (Replenish).
inline int InvMoveStack(const Context& c,uintptr_t actor,uint8_t srcC,int srcSlot,uint8_t dstC,int dstSlot,int count){
 if(ActionCursorBusy(c,actor))return 2;uint64_t mgr{};if(!Read(actor+0xae0,&mgr)||!mgr)return 1;
 const uint64_t enabled=Virtual0(c,mgr,8);if(!enabled||!(reinterpret_cast<bool(*)(uint64_t)>(enabled)(mgr)))return 1;
 if(count<1||count>255)return 5;
 bool valid=false;{uint64_t cur{};if(Read(mgr+0x68,&cur)&&!cur){reinterpret_cast<void(*)(uint64_t,int)>(c.base+0x2b71330)(mgr,InvScreenOpen()?0:1);Read(mgr+0x68,&cur);valid=cur!=0;}}
 int result=5;InvSlot src{},dst{};
 if(InvResolve(c,mgr,srcC,srcSlot,&src)&&InvResolve(c,mgr,dstC,dstSlot,&dst)){
  const uint64_t local=ActionContainer(actor);const uint64_t srcStack=srcC==0xc?ActionStackAt(c,local,srcSlot):ActionStackAt(c,src.container,srcSlot),dstStackLocal=dstC==0xc?ActionStackAt(c,local,dstSlot):ActionStackAt(c,dst.container,dstSlot);   // offhand (0x22) / armor (6): stacks of their own container
  uint8_t have{};Read(srcStack+0x22,&have);
  if(have==count&&!(src.container==dst.container&&srcSlot==dstSlot)&&valid){
   const uint64_t dstModel=ActionStackAt(c,dst.container,dstSlot);const InvSlotInfo si=InvInfo(src,srcStack),di=InvInfo(dst,dstModel);
   alignas(16) uint8_t oldSrc[0x100]{},oldDst[0x100]{};auto copy=reinterpret_cast<void(*)(void*,uint64_t)>(c.base+0x1a52620);auto kill=[&](void* stack){reinterpret_cast<void(*)(void*,int)>(c.base+0x2f5950)(stack,0);};   // destructor without deallocation
   copy(oldSrc,srcStack);copy(oldDst,dstStackLocal);
   const uint64_t setSrc=Virtual0(c,src.container,0x68),setDst=Virtual0(c,dst.container,0x68);
   if(setSrc)reinterpret_cast<void(*)(uint64_t,int,void*,int)>(setSrc)(src.container,srcSlot,oldDst,0);if(setDst)reinterpret_cast<void(*)(uint64_t,int,void*,int)>(setDst)(dst.container,dstSlot,oldSrc,0);
   if(srcC==0xc&&local&&local!=src.container){const uint64_t f=Virtual0(c,local,0x68);if(f)reinterpret_cast<void(*)(uint64_t,int,void*,int)>(f)(local,srcSlot,oldDst,0);}   // slot 0xff
   if(dstC==0xc&&local&&local!=dst.container){const uint64_t f=Virtual0(c,local,0x68);if(f)reinterpret_cast<void(*)(uint64_t,int,void*,int)>(f)(local,dstSlot,oldSrc,0);}
   const bool emptyDst=ActionItem((uint64_t)oldDst)==0;invWriteAddress=c.base+0x2b80b40;uint8_t* action=emptyDst?InvNewAction(1,(uint8_t)count,si,di):InvNewAction(2,0,si,di);
   uint8_t* owned=action;reinterpret_cast<void(*)(uint64_t,uint8_t**)>(c.base+0x2b72b40)(mgr,&owned);if(owned)InvActionDtor(owned,1);
   kill(oldDst);kill(oldSrc);result=0;}}
 if(valid){reinterpret_cast<void(*)(uint64_t)>(c.base+0x2b71680)(mgr);reinterpret_cast<void(*)(uint64_t)>(c.base+0x2b72860)(mgr);}
 return result;}

// ---- Replenish: first hotbar slot 0..8 that is non-empty and for which a slot 9+ holds the same item in a larger
// count; that whole stack is moved into the hotbar slot.
struct ReplenishShared{std::atomic<bool> enabled{false},fault{false};};
inline ReplenishShared& Replenish(){static auto* r=new ReplenishShared;return *r;}
inline bool ReplenishChoose(const Context& c,uintptr_t actor,int* src,int* dst){const uint64_t box=ActionContainer(actor);const int n=ActionSize(c,box);
 for(int i=0;i<n&&i<9;++i){const uint64_t a=ActionStackAt(c,box,i);const uint64_t ia=ActionItem(a);if(!ia)continue;uint8_t ca{};Read(a+0x22,&ca);
  for(int j=9;j<n;++j){const uint64_t b=ActionStackAt(c,box,j);if(ActionItem(b)!=ia)continue;uint8_t cb{};Read(b+0x22,&cb);if(ca<cb){*src=j;*dst=i;return true;}}}
 return false;}
inline void ReplenishWork(){if(!Replenish().enabled)return;Context c{};if(!Local(&c))return;int s{},d{};if(!ReplenishChoose(c,c.actor,&s,&d))return;
 InvSubmit(1,[](uintptr_t actor){if(!Replenish().enabled)return;Context k{};if(!Local(&k)||k.actor!=actor)return;int a{},b{};if(!ReplenishChoose(k,actor,&a,&b))return;
  uint8_t count{};Read(ActionStackAt(k,ActionContainer(actor),a)+0x22,&count);InvMoveStack(k,actor,0xc,a,0xc,b,count);});}
inline void(*volatile inventoryModulesFrame)()=nullptr;   // Auto Totem / Auto Armor (game_player.hpp)
inline void InvCancel(int key){auto& s=Inventory();std::lock_guard<std::recursive_mutex> lock(s.guard);s.queue.erase(key);}
inline void InventoryFrame(){if(Inventory().fault)return;__try{ReplenishWork();if(auto f=inventoryModulesFrame)f();InvRenderWork();}__except(EXCEPTION_EXECUTE_HANDLER){Inventory().fault=true;if(actionLog)actionLog("Inventory: fault caught, transactions stopped until restart");}}
inline bool InventoryReceive(uintptr_t packet){if(Inventory().fault)return false;__try{return InvReceiveWork(packet);}__except(EXCEPTION_EXECUTE_HANDLER){Inventory().fault=true;return false;}}
}
