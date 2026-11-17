// Native highlighting (Chams, Walls, Outline ESP, Hand Chams, Block ESP): marks render elements so the game draws them
// itself in its EditorHighlightScene pass.
// Game hooks (prologues checked):
// #0   MC 0x594c5c0  entity render (7 args): selection before the call, marking after it
// #223 MC 0xbb84ee0  render pass construction (21 args)
// hidden MC 0x6698e30  actor emission (3 args): context byte r8+0x7a = 1 during the call
// hidden MC 0x4df8370  actor material: 3rd argument 1 -> 2
// Game objects: frame builder = *(MC 0x128c5048); scene = *(builder+0x380);
// scene elements = MC 0x5aba5c0(scene+0x9d0) -> vector of 200-byte elements;
// insertion = builder->vtable[0x360/8](builder, {data, 8}).
// Element bits: +0x0b bit0 = highlighted actor element; +0x0a bit7 = active style element.
#pragma once
#include "game_events.hpp"
#include "game_hand.hpp"
#include "game_filter.hpp"
#include "../include/nickel/visual_highlight.hpp"
#include <MinHook.h>
#include <memory>
#include <mutex>
#include <vector>

namespace game {
// ---------------------------------------------------------------- game bindings
inline uintptr_t HlBase(){uintptr_t base{};uint32_t size{};return Supported(&base,&size)?base:0;}
inline uintptr_t HlBuilder(){const uintptr_t base=HlBase();uint64_t value{};return base&&Read(base+Rva(0x128c5048),&value)?(uintptr_t)value:0;}
inline uintptr_t HlScene(){const uintptr_t builder=HlBuilder();uint64_t scene{};return builder&&Read(builder+0x380,&scene)?(uintptr_t)scene:0;}
inline uintptr_t HlItems(uintptr_t scene){const uintptr_t base=HlBase();if(!base||!scene)return 0;return reinterpret_cast<uintptr_t(*)(uintptr_t)>(base+Rva(0x5aba5c0))(scene+0x9d0);}
inline uint64_t HlCount(uintptr_t list){uint64_t b{},e{};if(!list||!Read(list,&b)||!Read(list+8,&e)||e<b)return 0;return (e-b)/200;}
inline uint8_t* HlItem(uintptr_t list,uint64_t index){uint64_t b{};return Read(list,&b)?reinterpret_cast<uint8_t*>(b+index*200):nullptr;}
inline void HlCount_(int i);
inline void HlInsert(const uint8_t item[0x58]){const uintptr_t builder=HlBuilder();uint64_t table{},fn{};if(!builder||!Read(builder,&table)||!Read(table+0x360,&fn)||!fn){HlCount_(11);return;}HlCount_(10);
 struct Desc{const void* data;uint64_t kind;} desc{item,8};reinterpret_cast<void(*)(uintptr_t,Desc*)>(fn)(builder,&desc);}

// ---------------------------------------------------------------- state
struct HlRange{uintptr_t list;uint64_t begin,end;uint8_t style,slot;};
struct HlSub{uint64_t index=0;bool present=false;};
struct HlSceneRecord{uintptr_t scene=0;uint64_t base=0;uintptr_t lists[16]{};HlSub sub[16][8];std::vector<HlRange> ranges,skips;bool flag904=false;};
struct HlModules{   // filled by the menu from each module's Select / Style settings
 bool walls=false,chams=false,outline=false;
 bool wallsP=true,wallsM=true,wallsI=true,wallsPr=true;               // Walls: players / mobs / items / projectiles
 bool chamsP=true,chamsM=true,chamsI=false,chamsPr=false;int chamsMode=0;float chamsColor[4]{140/255.f,89/255.f,1,76/255.f},chamsIntensity=1.2f,chamsSpeed=1;
 bool outlinePlayersOnly=true,outlineSelf=false,outlineGlow=false,outlineVisibleOnly=false;float outlineColor[4]{1,1,1,1},outlineWidth=2,outlineGlowRadius=8,outlineGlowIntensity=1.5f;
 // Hand Chams: slot 1 = main hand, slot 2 = off hand, unless both styles are equal (then everything uses slot 1).
 bool hand=false,handMainOn=false,handOffOn=false,handArm=false,handMerged=false;nickel::visual::HighlightStyle handMain{},handOff{};
 // Block ESP: default style in slot 8.
 // Item ESP, Outline option: ground items filtered by name -> bit 4.
 bool item=false,itemWhite=false,itemBlack=false;std::vector<std::string> itemWhiteList,itemBlackList;
 bool block=false;nickel::visual::HighlightStyle blockStyle{};nickel::visual::HighlightStyle blockOwn[8]{};uint8_t blockUsed=1;   // styles 1..7 use slots 9..15
};
struct HlState{
 std::mutex guard;HlModules modules{};std::shared_ptr<const HlModules> snapshot=std::make_shared<const HlModules>();   // snapshot: read lock-free by the hooks, replaced by ConfigureHighlight
 std::vector<HlSceneRecord> scenes;nickel::visual::RainbowSlot rainbow[16]{};
 volatile long tid178=0,tid17c=0,tid180=0;bool flag184=false;LARGE_INTEGER start{};volatile long fault=0,seen=0,active=0;void(*log)(const char*)=nullptr;
 // Diagnostic counters: compiled out unless NICKEL_HL_DIAG is defined (nothing reads them).
 volatile long diag[24]{};volatile long faultWhere=0,lastType=0,lastEntity=0;volatile long seenType[8]{},seenCount[8]{},seenQuiet[8]{};
};
enum HlDiag{HdBefore,HdInactive,HdThread,HdQuiet,HdNoBits,HdNoScene,HdNoSlot,HdOn,HdEnsure,HdEnsureNoList,HdInsert,HdInsertNoFn,HdGraph,HdGraphHave,HdPasses,HdAfter,HdLocal,HdRegFail,HdTypeFail,HdPlayer,HdCount};
inline HlState& Highlight(){static auto* s=new HlState;return *s;}
#ifdef NICKEL_HL_DIAG
inline void HlCount_(int i){InterlockedIncrement(&Highlight().diag[i]);}
#else
inline void HlCount_(int){}
#endif
inline double HlNowMs(){auto& s=Highlight();LARGE_INTEGER now{},f{};QueryPerformanceCounter(&now);QueryPerformanceFrequency(&f);if(!s.start.QuadPart)s.start=now;return f.QuadPart?(double)(now.QuadPart-s.start.QuadPart)*1000.0/f.QuadPart:0;}
inline bool HlOnThread(volatile long& tid){return tid&&(DWORD)tid==GetCurrentThreadId();}
inline bool HlActive(){auto& s=Highlight();return s.active&&!s.fault;}   // false once a fault was caught; checked by the hidden detours
inline bool HlInScope(){auto& s=Highlight();return HlOnThread(s.tid178)||HlOnThread(s.tid17c);}  // true on the render-graph threads

// Slot order and per-slot masks.
inline constexpr uint8_t kHlOrder[16]={8,9,10,11,12,13,14,15,0,3,4,5,6,7,1,2};

// ---------------------------------------------------------------- slot 0 styles
// Chams: fill; Outline ESP: outline; Walls: flag 1.
inline void HlActorStyle(const HlModules& m,nickel::visual::HighlightStyle* s){
 std::memset(s,0,sizeof(*s));
 if(m.walls)s->flags=1;
 if(m.chams){const bool rainbow=m.chamsMode==2;const float white[4]{1,1,1,.3f};std::memcpy(s->fill,rainbow?white:m.chamsColor,16);
  s->mode=(uint8_t)(rainbow?2:(m.chamsMode==1?1:0));s->intensity=m.chamsIntensity;s->speed=m.chamsSpeed;s->shinyFlag=0;s->fillEnabled=1;}
 if(m.outline){std::memcpy(s->outline,m.outlineColor,16);s->outlineB[0]=m.outlineWidth;s->outlineB[1]=m.outlineGlow?m.outlineGlowRadius:0.f;s->outlineC[0]=m.outlineGlowIntensity;
  uint8_t* raw=reinterpret_cast<uint8_t*>(s);raw[0x44]=m.outlineVisibleOnly?1:0;s->outlineEnabled=1;}
}

// ---------------------------------------------------------------- style element layout
inline HlSceneRecord& HlRecord(HlState& s,uintptr_t scene){for(auto& r:s.scenes)if(r.scene==scene)return r;s.scenes.push_back(HlSceneRecord{});s.scenes.back().scene=scene;if(s.scenes.size()>64)s.scenes.erase(s.scenes.begin());return s.scenes.back();}
inline bool HlRecordValid(const HlSceneRecord& r){
 if(!r.lists[0]&&!r.lists[3]){return false;}
 for(uint8_t slot:kHlOrder)for(int k=0;k<8;++k){const HlSub& sub=r.sub[slot][k];if(!sub.present)continue;const uintptr_t list=r.lists[slot];
  if(sub.index<HlCount(list)){const uint8_t* item=HlItem(list,sub.index);uint8_t flags{};return item&&Read((uintptr_t)item+10,&flags)&&(flags&0x80);}return false;}
 const uintptr_t list=r.lists[0];return list&&r.base<=HlCount(list);
}
// Camera origin folded to 1024: client->vtable[0x5c8]() +0x480 -> +0x658 float3.
inline void HlCameraOrigin(uint8_t out[12]){std::memset(out,0,12);Context c{};if(!Client(&c))return;uint64_t table{},fn{},lr{},p{};
 if(!Read(c.client,&table)||!Read(table+0x5c8,&fn)||!fn)return;if(!CallGetter(fn,c.client,&lr)||!lr||!Read(lr+0x480,&p)||!p)return;float v[3];if(!Floats(p+0x658,v,3))return;
 for(int i=0;i<3;++i)v[i]=v[i]-std::floor(v[i]*0.0009765625f)*1024.f;std::memcpy(out,v,12);}
inline void HlEnsureScene(HlState& s,uintptr_t scene){
 HlSceneRecord& r=HlRecord(s,scene);if(HlRecordValid(r))return;HlCount_(HdEnsure);
 const uintptr_t list=HlItems(scene);if(!list){HlCount_(HdEnsureNoList);return;}
 nickel::visual::HighlightStyle st{};HlActorStyle(s.modules,&st);const double now=HlNowMs();nickel::visual::rainbow_step(s.rainbow[0],st,now);
 for(auto& slot:r.sub)for(auto& sub:slot)sub={};for(auto& l:r.lists)l=0;r.ranges.clear();r.skips.clear();
 r.base=HlCount(list);r.lists[0]=list;
 const float time=(float)std::fmod(now/1000.0,1000.0);uint8_t origin[12];HlCameraOrigin(origin);
 const uint8_t* raw=reinterpret_cast<const uint8_t*>(&st);const bool b0=raw[0]!=0,fill=st.fillEnabled!=0,outline=st.outlineEnabled!=0;
 r.flag904=(raw[0x44]==1)&&fill;   // byte 0x904 = (+0x44 == 1) && fill
 auto push=[&](int k,bool keepFlags,bool keepFill,bool keepOutline){nickel::visual::HighlightStyle v=st;uint8_t* rv=reinterpret_cast<uint8_t*>(&v);
  if(!keepFlags)rv[0]=0;if(!keepFill)v.fillEnabled=0;if(!keepOutline)v.outlineEnabled=0;uint8_t item[0x58];nickel::visual::build_style_item(item,v,time,origin,false,s.flag184);
  r.sub[0][k].index=HlCount(list);r.sub[0][k].present=true;HlInsert(item);};
 if(b0)push(1,true,false,false);
 if(fill)push(2,false,true,false);
 if(b0&&fill)push(3,true,true,false);
 if(outline)push(4,false,false,true);
 if(b0&&outline)push(5,true,false,true);
 if(fill&&outline)push(6,false,true,true);
 if(b0&&fill&&outline)push(7,true,true,true);
 // Element 65536 (slot 3) is inserted into every scene after the fill element; the game sets +0xbe = 1 on it.
 // Without it the EditorHighlightScene pass draws nothing.
 if(b0||fill){uint8_t item[0x58]{};const float big=65536.f;std::memcpy(item+0x50,&big,4);r.lists[3]=list;r.sub[3][2].index=HlCount(list);r.sub[3][2].present=true;HlInsert(item);}
 // Slots 1 and 2 (hand styles, last in slot order); rainbow colour per slot.
 auto pushSlot=[&](int slot,nickel::visual::HighlightStyle v0){nickel::visual::rainbow_step(s.rainbow[slot],v0,now);const uint8_t* rv0=reinterpret_cast<const uint8_t*>(&v0);
  const bool f0=rv0[0]!=0,f1=v0.fillEnabled!=0,f2=v0.outlineEnabled!=0;r.lists[slot]=list;
  auto one=[&](int k,bool a,bool b,bool c){nickel::visual::HighlightStyle v=v0;uint8_t* rv=reinterpret_cast<uint8_t*>(&v);if(!a)rv[0]=0;if(!b)v.fillEnabled=0;if(!c)v.outlineEnabled=0;uint8_t item[0x58];
   nickel::visual::build_style_item(item,v,time,origin,false,s.flag184);r.sub[slot][k].index=HlCount(list);r.sub[slot][k].present=true;HlInsert(item);};
  if(f0)one(1,true,false,false);if(f1)one(2,false,true,false);if(f0&&f1)one(3,true,true,false);if(f2)one(4,false,false,true);if(f0&&f2)one(5,true,false,true);if(f1&&f2)one(6,false,true,true);if(f0&&f1&&f2)one(7,true,true,true);};
 if(s.modules.hand){if(s.modules.handMainOn)pushSlot(1,s.modules.handMain);if(s.modules.handOffOn&&!s.modules.handMerged)pushSlot(2,s.modules.handOff);}
 if(s.modules.block){pushSlot(8,s.modules.blockStyle);for(int k=1;k<8;++k)if((s.modules.blockUsed>>k)&1)pushSlot(8+k,s.modules.blockOwn[k]);}
 InterlockedExchange(&s.seen,1);
}
// Bits available for a slot (sub-elements 1, 2, 4 present).
inline uint8_t HlSlotBits(HlState& s,uintptr_t scene,int slot){HlEnsureScene(s,scene);HlSceneRecord& r=HlRecord(s,scene);return (uint8_t)((r.sub[slot][4].present?4:0)|(r.sub[slot][2].present?2:0)|(r.sub[slot][1].present?1:0));}
// Marks the added elements and records their range.
inline void HlRecordRange(HlState& s,uintptr_t scene,uintptr_t list,uint64_t begin,uint8_t style,uint8_t slot){
 HlSceneRecord& r=HlRecord(s,scene);const uint64_t end=HlCount(list);
 auto mark=[&](uint64_t from,uint64_t to){for(uint64_t i=from;i<to&&i<HlCount(list);++i){uint8_t* it=HlItem(list,i);if(it)it[0x0b]|=1;}r.ranges.push_back({list,from,to,style,slot});};
 if((slot&0xfc)!=4){for(const auto& skip:r.skips){if(skip.list==list&&begin<skip.end&&skip.begin<end&&begin<skip.begin){mark(begin,skip.begin);begin=skip.end;}}}
 if(begin<end)mark(begin,end);
}

// ---------------------------------------------------------------- selection (module Select settings)
inline bool HlFilter(bool players,bool mobs,bool items,bool projectiles,uintptr_t actor,bool local){
 uint64_t registry{};uint32_t entity{};if(!Read(actor+0x10,&registry)||!registry||!Read(actor+0x18,&entity)){HlCount_(HdRegFail);return false;}
 uintptr_t storage{};uint32_t packed{};uintptr_t element{};int32_t type{};
 if(local)return false;
 if(Storage(registry,Fnv("PlayerComponent"),&storage)&&Packed(storage,entity,&packed)){HlCount_(HdPlayer);return players;}
 if(!Element(registry,entity,"ActorTypeComponent",4,&element)||!Read(element,&type)){HlCount_(HdTypeFail);return false;}
 if(type&0x400000)return projectiles;if(type&0x100)return mobs;return items&&type==0x40;   // not a game address: actor type flag
}
inline bool HlIsLocal(uintptr_t actor){uint64_t registry{};uint32_t entity{};uintptr_t storage{};uint32_t packed{};return Read(actor+0x10,&registry)&&registry&&Read(actor+0x18,&entity)&&Storage(registry,Fnv("LocalPlayerComponent"),&storage)&&Packed(storage,entity,&packed);}
inline uint8_t HlSelect(const HlModules& m,uintptr_t actor,bool local){   // local: HlIsLocal(actor), computed once by the caller
 uint8_t bits=0;
 if(m.walls&&HlFilter(m.wallsP,m.wallsM,m.wallsI,m.wallsPr,actor,local))bits|=1;
 if(m.chams&&HlFilter(m.chamsP,m.chamsM,m.chamsI,m.chamsPr,actor,local))bits|=2;
 if(m.item&&!local){uint64_t registry{};uint32_t entity{};uintptr_t element{};int32_t type{};uint64_t item{};char name[128]{};
  if(Read(actor+0x10,&registry)&&registry&&Read(actor+0x18,&entity)&&Element(registry,entity,"ActorTypeComponent",4,&element)&&Read(element,&type)&&type==0x40&&(item=StackItem(actor+0x3d0))!=0&&Text(item+0xd8,name,sizeof(name))){
   auto bare=[](std::string s){const size_t c=s.rfind(':');return c==std::string::npos?s:s.substr(c+1);};const std::string id=bare(name);auto in=[&](const std::vector<std::string>& l){for(auto& e:l)if(bare(e)==id)return true;return false;};
   if((!m.itemWhite||in(m.itemWhiteList))&&(!m.itemBlack||!in(m.itemBlackList)))bits|=4;}}
 if(m.outline){ // local player only with Include Local Player; Players Only limits to players, otherwise mobs too
  bool ok=false;if(local)ok=m.outlineSelf;else{uint64_t registry{};uint32_t entity{};uintptr_t storage{};uint32_t packed{};uintptr_t element{};int32_t type{};
   if(Read(actor+0x10,&registry)&&registry&&Read(actor+0x18,&entity)){const bool player=Storage(registry,Fnv("PlayerComponent"),&storage)&&Packed(storage,entity,&packed);
    ok=player||(!m.outlinePlayersOnly&&Element(registry,entity,"ActorTypeComponent",4,&element)&&Read(element,&type)&&(type&0x100));}}
  if(ok)bits|=4;}
 return bits;
}

// ---------------------------------------------------------------- hook #3: ActorRendererAddEvent
// On the render-graph thread (+0x178): cancel for the local player. Otherwise, if no highlight module is active, do
// nothing; local player: cancel only if ClientInstance::vtable[0x680] (RenderPlayerModel) returns true; other
// entities: cancel if the selection keeps them.
inline bool HlRenderPlayerModel(){Context c{};if(!Client(&c))return false;uint64_t table{},fn{};if(!Read(c.client,&table)||!Read(table+0x680,&fn)||!fn)return false;
 return reinterpret_cast<bool(*)(uintptr_t)>(fn)(c.client);}
inline bool HlAddWork(void* actorPtr){
 auto& s=Highlight();const uintptr_t actor=(uintptr_t)actorPtr;if(!actor||!HlActive())return false;
 const bool local=HlIsLocal(actor);if(HlOnThread(s.tid178))return local;
 if(local)return HlRenderPlayerModel();
 const auto mp=std::atomic_load(&s.snapshot);return HlSelect(*mp,actor,false)!=0;
}
inline bool HlAddGuarded(void* actor){__try{return HlAddWork(actor);}__except(EXCEPTION_EXECUTE_HANDLER){InterlockedExchange(&Highlight().faultWhere,4);InterlockedExchange(&Highlight().fault,1);return false;}}

// ---------------------------------------------------------------- hook #0: entity render
struct HlPending{bool on=false;uintptr_t scene=0,list=0;uint64_t count=0;uint8_t slot0=0,slot3=0;long saved17c=0;};
static_assert(sizeof(HlPending)<=sizeof(ActorRenderToken::data),"token size");
inline HlPending HlBefore(void* params,void* actorPtr){
 HlPending p{};auto& s=Highlight();HlCount_(HdBefore);if(!HlActive()){HlCount_(HdInactive);return p;}if(HlOnThread(s.tid178)||HlOnThread(s.tid180)){HlCount_(HdThread);return p;}
 uint64_t paramsObj{};uint8_t a{},b{};if(!Read((uintptr_t)params+0x28,&paramsObj)||!paramsObj||!Read(paramsObj+0x75,&a)||!Read(paramsObj+0x76,&b)||a||b){HlCount_(HdQuiet);return p;}
 const uintptr_t actor=(uintptr_t)actorPtr;if(!actor)return p;
 const auto mp=std::atomic_load(&s.snapshot);
 const bool local=HlIsLocal(actor);if(local)HlCount_(HdLocal);const uint8_t bits=HlSelect(*mp,actor,local);if(!bits&&!local){HlCount_(HdNoBits);return p;}
 const uintptr_t scene=HlScene();if(!scene){HlCount_(HdNoScene);return p;}
 std::lock_guard<std::mutex> lock(s.guard);const uint8_t slot0=HlSlotBits(s,scene,0);const uint8_t slot3=local?HlSlotBits(s,scene,3):0;
 if(!(slot0&bits)&&!slot3){HlCount_(HdNoSlot);return p;}HlCount_(HdOn);
 p.on=true;p.scene=scene;p.list=HlItems(scene);p.count=HlCount(p.list);p.slot0=(uint8_t)(slot0&bits);p.slot3=slot3;
 p.saved17c=InterlockedExchange(&s.tid17c,(long)GetCurrentThreadId());return p;
}
inline void HlAfter(const HlPending& p){
 auto& s=Highlight();InterlockedExchange(&s.tid17c,p.saved17c);std::lock_guard<std::mutex> lock(s.guard);
 if(p.slot0)HlRecordRange(s,p.scene,p.list,p.count,p.slot0,0);if(p.slot3)HlRecordRange(s,p.scene,p.list,p.count,p.slot3,3);
}
inline HlPending HlBeforeGuarded(void* params,void* actor){__try{return HlBefore(params,actor);}__except(EXCEPTION_EXECUTE_HANDLER){InterlockedExchange(&Highlight().faultWhere,1);InterlockedExchange(&Highlight().fault,1);return HlPending{};}}
inline void HlAfterGuarded(const HlPending& p){HlCount_(HdAfter);__try{HlAfter(p);}__except(EXCEPTION_EXECUTE_HANDLER){InterlockedExchange(&Highlight().faultWhere,2);InterlockedExchange(&Highlight().fault,1);}}
// Attached to the existing hook #0 detour (game_filter.hpp ActorDetour): no second hook at the same address.
// ---------------------------------------------------------------- Hand Chams: element ranges added by hand rendering
// Before hook #230: scene element count; hook #0 (local player, once): type 2 range (arm);
// hook #232: type 3 range (off hand); after hook #230: marking by ranges of the same type.
struct HlHandRange{uint64_t begin,end;uint8_t type;};
struct HlHandState{DWORD tid=0;uintptr_t scene=0,list=0;uint64_t before=0;uint8_t bitsMain=0,bitsOff=0;bool arm=false,merged=false,armSeen=false;std::vector<HlHandRange> ranges;};
inline HlHandState& HlHand(){static auto* h=new HlHandState;return *h;}
inline thread_local bool hlArmOpen=false;inline thread_local uint64_t hlArmBegin=0;
inline void HlHandPreWork(){auto& s=Highlight();auto& h=HlHand();h.tid=0;if(!HlActive())return;const auto mp=std::atomic_load(&s.snapshot);const HlModules& m=*mp;if(!m.hand)return;
 const uintptr_t scene=HlScene();if(!scene)return;std::lock_guard<std::mutex> lock(s.guard);
 const uint8_t main=m.handMainOn?HlSlotBits(s,scene,1):0;const uint8_t off=m.handOffOn?(m.handMerged?main:HlSlotBits(s,scene,2)):0;if(!main&&!off)return;
 h.scene=scene;h.list=HlItems(scene);h.before=HlCount(h.list);h.bitsMain=main;h.bitsOff=off;h.arm=m.handArm;h.merged=m.handMerged;h.armSeen=false;h.ranges.clear();h.tid=GetCurrentThreadId();}
inline void HlHandPostWork(){auto& s=Highlight();auto& h=HlHand();if(!h.tid||h.tid!=GetCurrentThreadId())return;h.tid=0;
 const uint64_t after=HlCount(h.list);if(after<=h.before)return;const uint64_t n=after-h.before;std::vector<uint8_t> type((size_t)n,1);
 auto apply=[&](const HlHandRange& rg){const uint64_t b=std::max(rg.begin,h.before),e=std::min(rg.end,after);for(uint64_t i=b;i<e;++i)type[(size_t)(i-h.before)]=rg.type;};
 for(const auto& rg:h.ranges)if(rg.type==2)apply(rg);for(const auto& rg:h.ranges)if(rg.type!=2)apply(rg);
 std::lock_guard<std::mutex> lock(s.guard);HlSceneRecord& r=HlRecord(s,h.scene);
 for(uint64_t i=0;i<n;){const uint8_t t=type[(size_t)i];uint64_t j=i+1;while(j<n&&type[(size_t)j]==t)++j;
  uint8_t bits=0,slot=1;if(t==1)bits=h.bitsMain;else if(t==3){bits=h.bitsOff;slot=h.merged?1:2;}else if(t==2&&h.arm)bits=h.bitsMain;
  if(bits){const uint64_t b=h.before+i,e=h.before+j;for(uint64_t k=b;k<e&&k<HlCount(h.list);++k){uint8_t* it=HlItem(h.list,k);if(!it)continue;it[0x0b]|=1;if(bits&2)*reinterpret_cast<uint64_t*>(it)&=0xfffdffffffffffffull;}
   r.ranges.push_back({h.list,b,e,bits,slot});}
  i=j;}}
inline void HlHandPre(){__try{HlHandPreWork();}__except(EXCEPTION_EXECUTE_HANDLER){HlHand().tid=0;InterlockedExchange(&Highlight().faultWhere,5);InterlockedExchange(&Highlight().fault,1);}}
inline void HlHandPost(){__try{HlHandPostWork();}__except(EXCEPTION_EXECUTE_HANDLER){HlHand().tid=0;InterlockedExchange(&Highlight().faultWhere,6);InterlockedExchange(&Highlight().fault,1);}}
inline void HlOffPre(uint64_t* mark){auto& h=HlHand();if(h.tid&&h.tid==GetCurrentThreadId())*mark=HlCount(h.list);}
inline void HlOffPost(uint64_t mark){auto& h=HlHand();if(mark!=~0ull&&h.tid&&h.tid==GetCurrentThreadId())h.ranges.push_back({mark,HlCount(h.list),3});}
inline void HlActorPre(void* params,void* actor,ActorRenderToken* token){
 {auto& h=HlHand();if(h.tid&&h.tid==GetCurrentThreadId()&&!h.armSeen&&actor&&HlIsLocal((uintptr_t)actor)){h.armSeen=true;hlArmOpen=true;hlArmBegin=HlCount(h.list);token->on=true;}}
 const HlPending p=HlBeforeGuarded(params,actor);std::memcpy(token->data,&p,sizeof(p));if(p.on)token->on=true;}
inline void HlActorPost(ActorRenderToken* token){HlPending p;std::memcpy(&p,token->data,sizeof(p));if(p.on)HlAfterGuarded(p);
 if(hlArmOpen){hlArmOpen=false;auto& h=HlHand();if(h.tid&&h.tid==GetCurrentThreadId())h.ranges.push_back({hlArmBegin,HlCount(h.list),2});}}

// ---------------------------------------------------------------- passes
inline void HlSetItems(const HlRange& rg,bool on){for(uint64_t i=rg.begin;i<rg.end&&i<HlCount(rg.list);++i){uint8_t* it=HlItem(rg.list,i);if(it){if(on)it[0x0b]|=1;else it[0x0b]&=0xfe;}}}
inline void HlSetStyle(const HlSceneRecord& r,int slot,int k,bool on){const HlSub& sub=r.sub[slot][k];if(!sub.present||!r.lists[slot])return;if(sub.index>=HlCount(r.lists[slot]))return;uint8_t* it=HlItem(r.lists[slot],sub.index);if(it){if(on)it[0x0a]|=0x80;else it[0x0a]&=0x7f;}}
inline void HlApplyPass(const HlSceneRecord& r,uint8_t slot,uint8_t mask){
 for(const auto& rg:r.ranges)HlSetItems(rg,false);
 for(const auto& rg:r.ranges)if(rg.slot==slot&&(rg.style&mask))HlSetItems(rg,true);
 if(slot==3){for(const auto& rg:r.ranges){bool dup=false;for(const auto& o:r.ranges)if(o.slot==3&&o.list==rg.list&&o.begin==rg.begin&&o.end==rg.end){dup=true;break;}if(!dup)HlSetItems(rg,false);}}
 for(uint8_t s:kHlOrder)for(int k=1;k<8;++k)HlSetStyle(r,s,k,s==slot&&k==mask);
}
inline void HlRestoreAll(const HlSceneRecord& r){for(const auto& rg:r.ranges)HlSetItems(rg,true);for(uint8_t s:kHlOrder)for(int k=0;k<8;++k)HlSetStyle(r,s,k,true);}

// ---------------------------------------------------------------- hook #223: render pass construction
using HlGraphFn=uint64_t(*)(uint64_t,uint64_t,uint64_t,uint64_t,uint64_t,uint64_t,uint64_t,uint64_t,uint64_t,uint64_t,uint64_t,uint64_t,uint64_t,uint64_t,uint64_t,uint64_t,uint64_t,uint64_t,uint64_t,uint64_t,uint64_t);
inline HlGraphFn hlGraphOriginal=nullptr;
struct HlArgs{uint64_t v[21];};
// One pass: arg9 / arg11 point to 24 bytes that are copied; outputs are chained through MC 0x5698290.
inline uint64_t HlCallPass(HlArgs& a,uint8_t copy9[24],uint8_t copy11[24],uint64_t idx,uint64_t count){
 const uintptr_t base=HlBase();uint64_t value{};
 if(idx+1==count){Read(a.v[10]+8,&value);}   // last pass: the real output is the incoming arg11.q1
 else{uint64_t q1{};Read(a.v[8]+8,&q1);value=reinterpret_cast<uint64_t(*)(uint64_t,uint64_t,uint64_t)>(base+Rva(0x5698290))(a.v[0],a.v[0],q1+8);}
 std::memcpy(copy11+8,&value,8);
 const uint64_t result=hlGraphOriginal(a.v[0],a.v[1],a.v[2],a.v[3],a.v[4],a.v[5],a.v[6],a.v[7],(uint64_t)copy9,a.v[9],(uint64_t)copy11,a.v[11],a.v[12],a.v[13],a.v[14],a.v[15],a.v[16],a.v[17],a.v[18],a.v[19],a.v[20]);
 std::memcpy(copy9+8,copy11+8,8);return result;
}
inline uint64_t HlGraphWork(HlArgs& a){
 auto& s=Highlight();uint8_t copy9[24],copy11[24];std::memcpy(copy9,(void*)a.v[8],24);std::memcpy(copy11,(void*)a.v[10],24);
 s.flag184=(uint32_t)a.v[11]==1;
 HlSceneRecord copy{};bool have=false;
 {std::lock_guard<std::mutex> lock(s.guard);for(auto& r:s.scenes)if(r.scene==(uintptr_t)a.v[2]){if(HlRecordValid(r)){copy=r;have=true;}break;}}
 struct Pass{uint8_t slot,mask;};std::vector<Pass> passes;
 if(have){HlCount_(HdGraphHave);
  for(int k=0;k<16;++k){uint8_t slot=kHlOrder[k];if(k==9){if(passes.empty())continue;slot=3;}
   for(int m=0;m<3;++m){const uint8_t bit=(uint8_t)(1<<m);bool any=false;for(const auto& rg:copy.ranges)if(rg.slot==slot&&(rg.style&bit)){any=true;break;}
    if(!any||!copy.sub[slot][bit].present)continue;
    bool merged=false;if(!passes.empty()&&passes.back().slot==slot){
     // Do not merge the outline (bit 4) into a fill pass when byte 0x904 is set (slot 0).
     const bool split=slot==0&&copy.flag904&&bit==4&&(passes.back().mask&2);
     bool conflict=split;if(!conflict)for(const auto& rg:copy.ranges)if(rg.slot==slot&&(((rg.style&passes.back().mask)!=0)!=((rg.style&bit)!=0))){conflict=true;break;}
     if(!conflict){passes.back().mask|=bit;merged=true;}}
    if(!merged)passes.push_back({slot,bit});}}
 }
 uint64_t result=0;
 if(passes.empty()){for(const auto& rg:copy.ranges)HlSetItems(rg,false);result=HlCallPass(a,copy9,copy11,0,1);}
 else{for(size_t i=0;i<passes.size();++i){HlApplyPass(copy,passes[i].slot,passes[i].mask);result=HlCallPass(a,copy9,copy11,i,passes.size());}}
 if(have)HlRestoreAll(copy);
 return result;
}
inline bool HlGraphGuarded(HlArgs& a,uint64_t* result){HlCount_(HdGraph);__try{*result=HlGraphWork(a);return true;}__except(EXCEPTION_EXECUTE_HANDLER){InterlockedExchange(&Highlight().faultWhere,3);InterlockedExchange(&Highlight().fault,1);return false;}}
inline uint64_t HlGraphDetour(uint64_t a1,uint64_t a2,uint64_t a3,uint64_t a4,uint64_t a5,uint64_t a6,uint64_t a7,uint64_t a8,uint64_t a9,uint64_t a10,uint64_t a11,uint64_t a12,uint64_t a13,uint64_t a14,uint64_t a15,uint64_t a16,uint64_t a17,uint64_t a18,uint64_t a19,uint64_t a20,uint64_t a21){
 auto& s=Highlight();
 if(!HlActive())return hlGraphOriginal(a1,a2,a3,a4,a5,a6,a7,a8,a9,a10,a11,a12,a13,a14,a15,a16,a17,a18,a19,a20,a21);
 HlArgs args{{a1,a2,a3,a4,a5,a6,a7,a8,a9,a10,a11,a12,a13,a14,a15,a16,a17,a18,a19,a20,a21}};
 const long saved=InterlockedExchange(&s.tid178,(long)GetCurrentThreadId());uint64_t result=0;
 const bool ok=HlGraphGuarded(args,&result);InterlockedExchange(&s.tid178,saved);
 if(!ok)return hlGraphOriginal(a1,a2,a3,a4,a5,a6,a7,a8,a9,a10,a11,a12,a13,a14,a15,a16,a17,a18,a19,a20,a21);
 return result;
}

// ---------------------------------------------------------------- hidden detours
// Hooks #224/#225: these ordinary draw paths are excluded from the Highlighted pass, otherwise they cover its output.
// 15 arguments: stack arguments 5..15 live at rbp+0xc80..0xcd0.
using HlExcludeFn=uint64_t(*)(uint64_t,uint64_t,uint64_t,uint64_t,uint64_t,uint64_t,uint64_t,uint64_t,uint64_t,uint64_t,uint64_t,uint64_t,uint64_t,uint64_t,uint64_t);
inline HlExcludeFn hlExcludeOriginal[2]{};
template<int N> inline uint64_t HlExcludeDetour(uint64_t a,uint64_t b,uint64_t c,uint64_t d,uint64_t e,uint64_t f,uint64_t g,uint64_t h,uint64_t i,uint64_t j,uint64_t k,uint64_t l,uint64_t m,uint64_t n,uint64_t o){
 if(nickel::visual::exclude_from_highlight_pass(c))return 0;
 return hlExcludeOriginal[N](a,b,c,d,e,f,g,h,i,j,k,l,m,n,o);
}
using HlInsertFn=uint64_t(*)(void*,void*,void*);inline HlInsertFn hlInsertOriginal=nullptr;
inline uint64_t HlInsertDetour(void* a,void* b,void* ctx){
 HlCount_(20);if(!HlActive()||!HlInScope()||!ctx)return hlInsertOriginal(a,b,ctx);HlCount_(21);
 uint8_t* flag=(uint8_t*)ctx+nickel::visual::kHighlightContextFlagOffset;const uint8_t old=*flag;uint64_t r{};
 // Also restore while an engine exception unwinds: never leave a shared context highlighted after this actor
 // emission returns.
 __try{*flag=1;r=hlInsertOriginal(a,b,ctx);}__finally{*flag=old;}return r;
}
using HlMaterialFn=uint64_t(*)(uint64_t,uint64_t,uint64_t,uint64_t,uint64_t,uint64_t,uint64_t,uint64_t,uint64_t,uint64_t);inline HlMaterialFn hlMaterialOriginal=nullptr;
inline uint64_t HlMaterialDetour(uint64_t a,uint64_t b,uint64_t c,uint64_t d,uint64_t e,uint64_t f,uint64_t g,uint64_t h,uint64_t i,uint64_t j){
 if((uint32_t)c==1)HlCount_(22);if((uint32_t)c==1&&HlActive())c=1+(HlInScope()?1:0);return hlMaterialOriginal(a,b,c,d,e,f,g,h,i,j);
}

// ---------------------------------------------------------------- Walls: block entities (shared hook #9)
// With Block Actors: context r9->vtable[0x48]() is true; matrix stack = *( *(rdx+0x28) + 0x18 ); W = top(+0x40),
// V = top(+0); s = |position arg6| <= 0.1 ? 1 : 0.1/|p|; W' = M(+0xc0)·diag(s,s,s,1)·V·W (column-major A·B);
// W is restored after drawing.
inline bool hlWallsBlockActors=false;
inline void HlMul(float out[16],const float* a,const float* b){float r[16];for(int j=0;j<4;++j)for(int i=0;i<4;++i){float v=0;for(int k=0;k<4;++k)v+=a[k*4+i]*b[j*4+k];r[j*4+i]=v;}std::memcpy(out,r,64);}
inline uintptr_t HlStackTop(uintptr_t stack){uint64_t size{},map{},mapSize{},offset{},top{};if(!Read(stack+0x20,&size)||!size||!Read(stack+8,&map)||!Read(stack+0x10,&mapSize)||!mapSize||!Read(stack+0x18,&offset))return 0;
 return Read(map+((mapSize-1)&(offset+size-1))*8,&top)?(uintptr_t)top:0;}
inline float* HlWallsWork(uint64_t params,uint64_t context,uint64_t position,float saved[16]){
 if(!hlWallsBlockActors||!HlActive()||!context)return nullptr;uint64_t table{},fn{};if(!Read(context,&table)||!Read(table+0x48,&fn)||!fn)return nullptr;
 if(!reinterpret_cast<bool(*)(uint64_t)>(fn)(context))return nullptr;
 uint64_t ctx{},stacks{};if(!Read(params+0x28,&ctx)||!ctx||!Read(ctx+0x18,&stacks)||!stacks)return nullptr;
 const uintptr_t world=HlStackTop(stacks+0x40),view=HlStackTop(stacks);if(!world||!view)return nullptr;
 float p[3],w[16],v[16],m[16];if(!Floats(position,p,3)||!Floats(world,w,16)||!Floats(view,v,16)||!Floats(stacks+0xc0,m,16))return nullptr;
 const float len=std::sqrt(p[1]*p[1]+p[0]*p[0]+p[2]*p[2]);const float sc=len<=0.1f?1.f:0.1f/len;
 const float S[16]={sc,0,0,0, 0,sc,0,0, 0,0,sc,0, 0,0,0,1};float t1[16],t2[16],t3[16];HlMul(t1,m,S);HlMul(t2,t1,v);HlMul(t3,t2,w);
 std::memcpy(saved,w,64);std::memcpy((void*)world,t3,64);return (float*)world;
}
inline float* HlWallsGuarded(uint64_t params,uint64_t context,uint64_t position,float saved[16]){__try{return HlWallsWork(params,context,position,saved);}__except(EXCEPTION_EXECUTE_HANDLER){InterlockedExchange(&Highlight().fault,1);return nullptr;}}

// ---------------------------------------------------------------- configuration and install
inline void ConfigureHighlight(const HlModules& m){auto& s=Highlight();{std::lock_guard<std::mutex> lock(s.guard);s.modules=m;std::atomic_store(&s.snapshot,std::make_shared<const HlModules>(m));if(!(m.walls||m.chams||m.outline||m.hand||m.block||m.item))s.scenes.clear();}
 InterlockedExchange(&s.active,(m.walls||m.chams||m.outline||m.hand||m.block||m.item)&&!s.fault?1:0);}
inline void ConfigureWallsBlockActors(bool on,void(*log)(const char*)){hlWallsBlockActors=on;if(on&&InstallBlockActorHook(log)==1)blockActorWalls=HlWallsGuarded;}
inline int InstallHighlightHooks(void(*log)(const char*)){
 static int status=0;if(status)return status;status=-1;Highlight().log=log;uintptr_t base{};uint32_t size{};if(!Supported(&base,&size)){if(log)log("Highlight: unsupported game image");return status;}
 if(InstallActorHooks(log)!=1){if(log)log("Highlight: shared actor render hook unavailable");return status;}
 struct Hook{uint32_t rva;uint8_t bytes[17];size_t byteCount;void* detour;void** original;};
 const Hook hooks[]={
  {0xbb84ee0,{0x55,0x41,0x57,0x41,0x56,0x41,0x55,0x41,0x54,0x56,0x57,0x53},12,(void*)HlGraphDetour,(void**)&hlGraphOriginal},
  {nickel::visual::kHighlightActorEmitRva,{0x55,0x41,0x57,0x41,0x56,0x41,0x55,0x41,0x54,0x56,0x57,0x53,0xb8,0x58,0x21,0x00,0x00},17,(void*)HlInsertDetour,(void**)&hlInsertOriginal},
  {0x4df8370,{0x55,0x41,0x57,0x41,0x56,0x41,0x55,0x41,0x54,0x56,0x57,0x53},12,(void*)HlMaterialDetour,(void**)&hlMaterialOriginal},
  {nickel::visual::kHighlightExcludePassRva[0],{0x55,0x41,0x57,0x41,0x56,0x41,0x55,0x41,0x54,0x56,0x57,0x53,0x48,0x81,0xec,0x98,0x0c},17,(void*)HlExcludeDetour<0>,(void**)&hlExcludeOriginal[0]},
  {nickel::visual::kHighlightExcludePassRva[1],{0x55,0x41,0x57,0x41,0x56,0x41,0x55,0x41,0x54,0x56,0x57,0x53,0x48,0x81,0xec,0x98,0x0c},17,(void*)HlExcludeDetour<1>,(void**)&hlExcludeOriginal[1]}};
 for(const auto& h:hooks)if(!CodeIs(base+Rva(h.rva),h.bytes,(int)h.byteCount)){if(log)log("Highlight: unexpected game code, hooks not installed");return status;}
 size_t created=0;auto rollback=[&](const char* why){for(size_t i=0;i<created;++i)MH_RemoveHook((void*)(base+Rva(hooks[i].rva)));if(log)log(why);return status;};
 for(const auto& h:hooks){if(MH_CreateHook((void*)(base+Rva(h.rva)),h.detour,h.original)!=MH_OK)return rollback("Highlight: could not create hooks");++created;}
 for(const auto& h:hooks)if(MH_QueueEnableHook((void*)(base+Rva(h.rva)))!=MH_OK)return rollback("Highlight: could not queue hooks");
 if(MH_ApplyQueued()!=MH_OK)return rollback("Highlight: could not enable hooks");
 actorRenderPre=HlActorPre;actorRenderPost=HlActorPost;actorAddHighlight=HlAddGuarded;
 if(InstallHandHooks(log)==1){handRenderPre=HlHandPre;handRenderPost=HlHandPost;offHandPre=HlOffPre;offHandPost=HlOffPost;}else if(log)log("Highlight: hand hooks unavailable, Hand Chams disabled");
 status=1;if(log)log("Highlight: render graph, actor emit 0x6698e30 and actor material hooks installed; actor render shared");return status;
}
}
