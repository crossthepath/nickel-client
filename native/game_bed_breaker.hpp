// Bed Breaker. LevelTick: enable guard, world identity, cache of target positions found by scanning sub-chunk palettes
// (layers 0 and 1) of the 3 x 3 chunks around the player (1 ms budget, modified sub-chunks, round-robin),
// spawn point (Own Bed), pauses (Creative/Spectator, health, player breaking, Scaffold/Nuker/Regen), choice of the
// cheapest target (Dijkstra in the 2L+1 cube, cost = break ticks with the best tool), then the generic block breaker:
// action-queue sequence (Ghost Switch), rotation (mode 2 Silent / 1, priority 3), start/continue/stopDestroyBlock on the
// game mode, 1500 ms stall timeout, confirmation by SubChunkSetBlock (1500 ms).
// Drawing (Outline, ScreenViewRender), the module list suffix and the HUD progress bar read BedBreaker().snap.
// The "rotation reached" callback only acts on the tick thread (the rotation manager calls it on every stack pop);
// SubChunkSetBlock confirmation is handled on the next tick; the player validity check reads the local player.
#pragma once
#include "game_actions.hpp"
#include "game_rotation.hpp"
#include "game_xray.hpp"
#include <array>
#include <atomic>
#include <functional>
#include <cmath>
#include <mutex>
#include <queue>
#include <string>
#include <unordered_map>
#include <vector>

namespace game {
enum{kRotBedBreaker=8,kBedBreakerAction=9};
struct BbConfig{bool enabled=false;float range=4.5f;bool beds=true,cakes=false,eggs=false,custom=false;std::vector<std::string> blocks;bool through=true;int maxLayers=3;
 int ownBed=0;int spawnRadius=12;bool rotate=true,silent=true,throughWalls=false,ghost=true,swing=true,outline=true;bool othersOn=false;};
struct BbSnapshot{int state=0;bool uncover=false;float progress=0;int32_t n[3]{},p[3]{};};
struct BbShared{std::mutex guard;BbConfig cfg;BbSnapshot snap;std::mutex pendGuard;std::vector<uint64_t> dirty;struct Hit{uint64_t sub;uint8_t layer;uint16_t index;};std::vector<Hit> hits;
 volatile long fault=0;std::atomic<uint32_t> slotReady{0},seqDone{0},current{0};};
inline BbShared& BedBreaker(){static auto* s=new BbShared;return *s;}

struct BbSub{uint64_t l0=0,l1=0,s0=0,s1=0;std::vector<std::array<int32_t,3>> pos;};
struct BbChunk{uint64_t chunk=0,begin=0;uint32_t count=0;bool read=false;uint64_t gen=0;int x=0,z=0;std::vector<BbSub> subs;};
struct BbBreaker{int state=0;bool active=false;uint32_t counter=0,id=0;int32_t P[3]{},N[3]{};uint8_t face=0;int slot=0;float best=0;double clock=0;int64_t wait=0,stall=0,confirm=0;bool gone=false,second=false;};
struct BbState{uint64_t level=0,region=0;std::vector<std::string> blocks;std::unordered_map<uint64_t,uint8_t> category;std::unordered_map<uint64_t,BbChunk> chunks;std::vector<uint64_t> active;size_t robin=0;uint64_t gen=0;
 bool spawnKnown=false;float spawn[3]{};bool prevKnown=false;float prev[3]{};bool wasAlive=false;bool remembered=false;int32_t memory[3]{};BbBreaker br;bool wasEnabled=false;};
inline BbState& BbData(){static auto* s=new BbState;return *s;}

struct BbCtx{Context c;uintptr_t actor=0;uint64_t region=0,gm=0;float eye[3]{},box[6]{};float range=4.5f;bool noWalls=true;int16_t minY=0;};
inline bool BbLower(char a,char b){return (a>='A'&&a<='Z'?a+32:a)==(b>='A'&&b<='Z'?b+32:b);}
inline bool BbContains(const std::string& hay,const std::string& needle){if(needle.empty()||needle.size()>hay.size())return false;
 for(size_t i=0;i+needle.size()<=hay.size();++i){size_t k=0;while(k<needle.size()&&BbLower(hay[i+k],needle[k]))++k;if(k==needle.size())return true;}return false;}
// Category: 1 bed, 2 cake, 3 dragon egg, 4 Custom list (substring, case-insensitive)
inline uint8_t BbCategory(BbState& st,uint64_t block){if(!block)return 0;auto f=st.category.find(block);if(f!=st.category.end())return f->second;std::string name;uint8_t k=0;
 if(XrayBlockName(block,&name)){if(name=="minecraft:bed")k=1;else if(name=="minecraft:cake")k=2;else if(name=="minecraft:dragon_egg")k=3;else for(auto& b:st.blocks)if(BbContains(name,b)){k=4;break;}}
 if(st.category.size()<65536)st.category.emplace(block,k);return k;}
inline uint64_t BbPalette(const Context& c,uint64_t storage){if(!storage)return 0;const uint64_t get=Virtual(c,storage,0xa8);if(!get)return 0;uint64_t out[2]{};reinterpret_cast<void*(*)(uint64_t,uint64_t*)>(get)(storage,out);return out[0];}
// Scan of one sub-chunk
inline void BbScanSub(const Context& c,BbState& st,BbSub& sub,uint64_t address,int cx,int cz,int index,int minY){sub.pos.clear();sub.l0=sub.l1=sub.s0=sub.s1=0;
 if(!Read(address+0x30,&sub.l0)||!Read(address+0x38,&sub.l1))return;sub.s0=BbPalette(c,sub.l0);sub.s1=BbPalette(c,sub.l1);
 for(int layer=0;layer<2;++layer){const uint64_t storage=layer?sub.l1:sub.l0;if(!storage)continue;const uint64_t palette=Virtual(c,storage,0xa8),bitsOf=Virtual(c,storage,0xa0),wordsOf=Virtual(c,storage,0x90);if(!palette||!bitsOf||!wordsOf)continue;
  uint64_t out[2]{};reinterpret_cast<void*(*)(uint64_t,uint64_t*)>(palette)(storage,out);if(!out[0]||!out[1])continue;const uint32_t entries=(uint32_t)std::min<uint64_t>(out[0],4096);
  static uint8_t match[4096];bool any=false;for(uint32_t i=0;i<entries;++i){uint64_t b{};match[i]=Read(out[1]+8ull*i,&b)&&BbCategory(st,b)!=0;any=any||match[i];}if(!any)continue;
  auto add=[&](uint32_t at){sub.pos.push_back({cx*16+(int32_t)(at>>8),minY+16*index+(int32_t)(at&15),cz*16+(int32_t)((at>>4)&15)});};
  const int bits=reinterpret_cast<int(*)(uint64_t)>(bitsOf)(storage);if(bits<=0){if(match[0])for(uint32_t at=0;at<4096;++at)add(at);continue;}if(bits>32)continue;
  uint64_t words[2]{};reinterpret_cast<void*(*)(uint64_t,uint64_t*)>(wordsOf)(storage,words);if(!words[0]||!words[1])continue;const uint32_t per=32/bits,mask=bits==32?0xffffffffu:((1u<<bits)-1);
  for(uint32_t at=0;at<4096;++at){const uint32_t word=at/per;if(word>=words[0])break;uint32_t v{};if(!Read(words[1]+4ull*word,&v))break;const uint32_t pick=(v>>((at%per)*bits))&mask;if(pick<entries&&match[pick])add(at);}}}
inline void BbClearCaches(BbState& st,const BbConfig& cfg){st.category.clear();st.chunks.clear();st.active.clear();st.robin=0;st.blocks=cfg.blocks;}

// ---- Shared checks
inline bool BbSolid(uint64_t block){uint8_t a{},b{};return BlockFlags(block,&a,&b)&&a!=0&&!(b&2)&&(b&4);}
inline uint64_t BbBlock(const BbCtx& x,const int32_t* p){return RegionBlock(x.c,x.region,p);}
inline bool BbSupport(const BbCtx& x,const int32_t* b){const int feet=(int)std::floor(x.box[1]-0.01f);
 if(!((float)b[0]+1>x.box[0]&&(float)b[2]+1>x.box[2]&&(float)b[0]<x.box[3]&&(float)b[2]<x.box[5]))return false;if(b[1]>feet)return false;if(b[1]==feet)return true;
 for(int y=b[1]+1;y<=feet;++y){const int32_t q[3]={b[0],y,b[2]};if(BbSolid(BbBlock(x,q)))return false;}return true;}
inline float BbDist2(const float* e,const int32_t* b){const float dx=b[0]+.5f-e[0],dy=b[1]+.5f-e[1],dz=b[2]+.5f-e[2];return dx*dx+dy*dy+dz*dz;}
inline bool BbReach(const BbCtx& x,const int32_t* b){return BbDist2(x.eye,b)<=x.range*x.range&&!BbSupport(x,b);}
// Best slot: destroy speed item->vtable[0x2c0](item, stack, block), empty = 1; must be strictly better
inline float BbSpeed(const Context& c,uint64_t stack,uint64_t block){const uint64_t item=ActionItem(stack);if(!item)return 1.f;const uint64_t f=Virtual0(c,item,0x2c0);if(!f)return 1.f;return reinterpret_cast<float(*)(uint64_t,uint64_t,uint64_t)>(f)(item,stack,block);}
inline int BbBestSlot(const Context& c,uintptr_t actor,uint64_t block,int current){const uint64_t box=ActionContainer(actor);const int size=ActionSize(c,box);const int n=size<9?size:9;
 int best=-1;float score=1.f;if(current>=0&&current<n){best=current;score=BbSpeed(c,ActionStackAt(c,box,current),block);}
 for(int i=0;i<n;++i){const float s=BbSpeed(c,ActionStackAt(c,box,i),block);if(s>score){score=s;best=i;}}return best;}
struct BbCost{int slot=-1;int ticks=0;bool ok=false;};
inline BbCost BbCostOf(const BbCtx& x,std::unordered_map<uint64_t,BbCost>& cache,uint64_t block){auto f=cache.find(block);if(f!=cache.end())return f->second;BbCost r{};
 int32_t* sel=ActionSelected(x.actor);if(sel&&block){const int s=BbBestSlot(x.c,x.actor,block,*sel);if(s>=0){const int32_t saved=*sel;*sel=s;const float p=reinterpret_cast<float(*)(uint64_t,uint64_t)>(x.c.base+Rva(0x376b010))(x.gm,block);*sel=saved;
   if(std::isfinite(p)&&p>0){r.slot=s;r.ticks=(int)std::ceil(0.99999f/p);r.ok=true;}}}
 cache.emplace(block,r);return r;}
// Exposed face: faces 0 down, 1 up, 2 north, 3 south, 4 west, 5 east
inline const int (&BbNormal(int f))[3]{static const int n[6][3]={{0,-1,0},{0,1,0},{0,0,-1},{0,0,1},{-1,0,0},{1,0,0}};return n[f];}
inline bool BbFace(const BbCtx& x,const int32_t* b,uint8_t* face){float best=3.4e38f;int found=-1;
 for(int f=0;f<6;++f){const int* n=BbNormal(f);const float fc[3]={b[0]+.5f+.5f*n[0],b[1]+.5f+.5f*n[1],b[2]+.5f+.5f*n[2]};
  if(!((x.eye[0]-fc[0])*n[0]+(x.eye[1]-fc[1])*n[1]+(x.eye[2]-fc[2])*n[2]>0))continue;const int32_t nb[3]={b[0]+n[0],b[1]+n[1],b[2]+n[2]};if(BbSolid(BbBlock(x,nb)))continue;
  const float d=(fc[0]-x.eye[0])*(fc[0]-x.eye[0])+(fc[1]-x.eye[1])*(fc[1]-x.eye[1])+(fc[2]-x.eye[2])*(fc[2]-x.eye[2]);if(!(d<best))continue;
  if(x.noWalls){const uint64_t clipOf=Virtual(x.c,x.region,0x198);if(!clipOf)continue;using Clip=void*(*)(uint64_t,void*,const float*,const float*,bool,bool,int,bool,bool,uint64_t,const std::function<bool()>*,bool);
   static const std::function<bool()> accept=[]{return true;};const float to[3]={b[0]+.5f+.45f*n[0],b[1]+.5f+.45f*n[1],b[2]+.5f+.45f*n[2]};alignas(16) uint8_t res[0x200]{};
   reinterpret_cast<Clip>(clipOf)(x.region,res,x.eye,to,false,true,(int)x.range+2,false,false,x.actor,&accept,false);
   uint64_t weak{};memcpy(&weak,res+0x40,8);if(weak&&InterlockedDecrement((volatile long*)(weak+0xc))==0){uint64_t t{},rel{};if(Read(weak,&t)&&Read(t+8,&rel)&&rel)reinterpret_cast<void(*)(uint64_t)>(rel)(weak);}
   int type{};memcpy(&type,res+0x18,4);if(type==0){int32_t hp[3];memcpy(hp,res+0x20,12);if(hp[0]!=b[0]||hp[1]!=b[1]||hp[2]!=b[2])continue;}}
  best=d;found=f;}
 if(found<0)return false;*face=(uint8_t)found;return true;}
// Team colour: last § code of the name tag before the player name, first line
inline int BbTeamColor(uintptr_t actor){uintptr_t registry{},element{};uint32_t entity{};uint64_t begin{},end{},item{};char tag[512]{},name[256]{};
 if(!ActorEntity(actor,&registry,&entity)||!Element(registry,entity,"SynchedActorDataComponent",0x48,&element)||!Read(element,&begin)||!Read(element+8,&end)||end<begin||end-begin<=0x20||!Read(begin+0x20,&item)||!item)return -1;
 if(!RawText(item+0x10,tag,sizeof(tag)))return -1;RawText(actor+0xbe8,name,sizeof(name));std::string t=tag;const size_t nl=t.find('\n');if(nl!=std::string::npos)t.resize(nl);
 if(name[0]){const size_t at=t.find(name);if(at!=std::string::npos)t.resize(at);}
 static const char* codes="0123456789abcdefghijmnpqstuv";int color=-1;
 for(size_t i=0;i+2<t.size();++i)if((uint8_t)t[i]==0xc2&&(uint8_t)t[i+1]==0xa7){char ch=t[i+2];if(ch>='A'&&ch<='Z')ch=(char)(ch+32);if(ch=='r'){color=-1;}else{const char* p=strchr(codes,ch);if(p&&ch)color=(int)(p-codes);}i+=2;}
 return color>=0&&color<16?color:-1;}
inline bool BbOwnColor(const BbCtx& x,const int32_t* p){const int team=BbTeamColor(x.actor);if(team<0)return false;const uint64_t be=RegionBlockEntity(x.c,x.region,p);uint8_t type{};int32_t bed{};
 if(!be||!Read(be+0x14,&type)||type!=0x1b||!Read(be+0xf8,&bed)||bed<0||bed>=16)return false;
 static const uint16_t masks[16]={1u<<0,1u<<4,1u<<2,1u<<6,1u<<1,(1u<<5)|(1u<<13),1u<<14,(1u<<7)|(1u<<8),1u<<8,(1u<<4)|(1u<<12),(1u<<2)|(1u<<10),(1u<<6)|(1u<<12),1u<<1,(1u<<9)|(1u<<13),1u<<11,1u<<15};
 return (masks[team]>>bed)&1;}
// Target filter
inline bool BbTarget(BbState& st,const BbConfig& cfg,const BbCtx& x,const int32_t* p){const uint8_t k=BbCategory(st,BbBlock(x,p));if(!k)return false;
 if((k==1&&!cfg.beds)||(k==2&&!cfg.cakes)||(k==3&&!cfg.eggs)||(k==4&&!cfg.custom))return false;if(k!=1)return true;
 auto nearSpawn=[&]{if(!st.spawnKnown)return false;const float d=BbDist2(st.spawn,p);return d<=(float)cfg.spawnRadius*(float)cfg.spawnRadius;};
 bool mine=false;switch(cfg.ownBed){case 0:mine=nearSpawn()||BbOwnColor(x,p);break;case 1:mine=BbOwnColor(x,p);break;case 2:mine=nearSpawn();break;default:mine=false;}
 return !mine;}
// Evaluation: Dijkstra in the 2L+1 cube
struct BbPlan{int32_t P[3]{},N[3]{};uint8_t face=0;int slot=0;int cost=0;float dist=0;bool ok=false;};
inline BbPlan BbEvaluate(BbState& st,const BbConfig& cfg,const BbCtx& x,const int32_t* P,int L,std::unordered_map<uint64_t,BbCost>& costs){BbPlan r{};
 if(!BbTarget(st,cfg,x,P)||!BbReach(x,P))return r;const BbCost c0=BbCostOf(x,costs,BbBlock(x,P));if(!c0.ok)return r;
 const int side=2*L+1;std::vector<int> cost((size_t)side*side*side,0x7fffffff);auto idx=[&](const int32_t* q){return ((q[0]-P[0]+L)*side+(q[1]-P[1]+L))*side+(q[2]-P[2]+L);};
 struct Node{int c;int32_t p[3];bool operator<(const Node& o)const{return c>o.c;}};std::priority_queue<Node> heap;cost[idx(P)]=c0.ticks;heap.push({c0.ticks,{P[0],P[1],P[2]}});
 while(!heap.empty()){const Node n=heap.top();heap.pop();if(n.c>cost[idx(n.p)])continue;uint8_t face{};
  if(BbFace(x,n.p,&face)){const BbCost cn=BbCostOf(x,costs,BbBlock(x,n.p));memcpy(r.P,P,12);memcpy(r.N,n.p,12);r.face=face;r.slot=cn.slot;r.cost=n.c;r.dist=BbDist2(x.eye,n.p);r.ok=true;return r;}
  for(int f=0;f<6;++f){const int* d=BbNormal(f);const int32_t v[3]={n.p[0]+d[0],n.p[1]+d[1],n.p[2]+d[2]};if(std::abs(v[0]-P[0])>L||std::abs(v[1]-P[1])>L||std::abs(v[2]-P[2])>L)continue;
   const uint64_t vb=BbBlock(x,v);if(!BbSolid(vb)||!BbReach(x,v))continue;const BbCost cv=BbCostOf(x,costs,vb);if(!cv.ok)continue;const int c=n.c+cv.ticks;if(c<cost[idx(v)]){cost[idx(v)]=c;heap.push({c,{v[0],v[1],v[2]}});}}}
 return r;}

// ---- Breaker
inline double BbGmClock(uint64_t gm){uint64_t o{},t{},f{};if(!Read(gm+0xb8,&o)||!o||!Read(o,&t)||!Read(t+8,&f)||!f)return 0;return reinterpret_cast<double(*)(uint64_t)>(f)(o);}
inline bool BbValid(BbState& st,const BbConfig& cfg,const BbCtx& x){const BbBreaker& b=st.br;const float lim=x.range+.75f;if(BbDist2(x.eye,b.N)>lim*lim||BbSupport(x,b.N))return false;
 return !memcmp(b.N,b.P,12)?BbTarget(st,cfg,x,b.P):BbSolid(BbBlock(x,b.N));}
inline bool BbGone(BbState& st,const BbConfig& cfg,const BbCtx& x){const BbBreaker& b=st.br;return !memcmp(b.N,b.P,12)?!BbTarget(st,cfg,x,b.P):!BbSolid(BbBlock(x,b.N));}
inline void BbCall(BbState& st,const BbCtx& x,const std::function<void()>& f){int32_t* sel=ActionSelected(x.actor);uint8_t* breaking=(uint8_t*)(x.actor+0xb4a);if(!sel)return;
 const int32_t saved=*sel;const uint8_t was=*breaking;*sel=st.br.slot;*breaking=1;f();*sel=saved;*breaking=was;}
inline void BbBroken(BbState& st,const BbConfig& cfg,const BbCtx& x){auto& b=st.br;b.state=4;b.gone=BbGone(st,cfg,x);b.second=false;b.confirm=ActionNs();}
inline void BbStart(BbState& st,const BbConfig& cfg,const BbCtx& x){auto& b=st.br;bool done=false;const uint64_t f=Virtual0(x.c,x.gm,0x08);if(!f)return;
 BbCall(st,x,[&]{int32_t at[3]={b.N[0],b.N[1],b.N[2]};reinterpret_cast<bool(*)(uint64_t,int32_t*,uint8_t,bool*)>(f)(x.gm,at,b.face,&done);});
 if(cfg.swing)ActionSwing(x.c,x.actor,2);b.state=2;b.best=0;b.clock=BbGmClock(x.gm);b.stall=ActionNs();if(done)BbBroken(st,cfg,x);}
inline void BbContinue(BbState& st,const BbConfig& cfg,const BbCtx& x){auto& b=st.br;bool done=false;const uint64_t f=Virtual0(x.c,x.gm,0x18);if(!f)return;
 BbCall(st,x,[&]{int32_t at[3]={b.N[0],b.N[1],b.N[2]};float eye[3]={x.eye[0],x.eye[1],x.eye[2]};reinterpret_cast<bool(*)(uint64_t,int32_t*,uint8_t,float*,bool*)>(f)(x.gm,at,b.face,eye,&done);});
 if(done)BbBroken(st,cfg,x);else if(cfg.swing)ActionSwing(x.c,x.actor,2);}
inline volatile DWORD bbTickThread=0;inline BbCtx* volatile bbCurrentCtx=nullptr;inline BbCtx bbLastCtx{};
// Rotation: face centre; "reached" callback -> Start (state 1) or Continue
inline void BbRotate(BbState& st,const BbConfig& cfg,const BbCtx& x,bool act){auto& b=st.br;const int* n=BbNormal(b.face);const float pt[3]={b.N[0]+.5f+.5f*n[0],b.N[1]+.5f+.5f*n[1],b.N[2]+.5f+.5f*n[2]};
 const float dx=pt[0]-x.eye[0],dy=pt[1]-x.eye[1],dz=pt[2]-x.eye[2];RotRequest r;r.module=kRotBedBreaker;r.yaw=std::atan2(-dx,dz)*57.29578f;r.pitch=-std::atan2(dy,std::sqrt(dx*dx+dz*dz))*57.29578f;
 r.mode=cfg.silent?2:1;r.priority=3;const uint32_t id=b.id;
 if(act)r.reached=[id](uintptr_t){if(bbTickThread!=GetCurrentThreadId())return;auto& s=BbData();if(!s.br.active||s.br.id!=id)return;BbCtx* x=bbCurrentCtx;if(!x)return;
  BbConfig cfg;{std::lock_guard<std::mutex> l(BedBreaker().guard);cfg=BedBreaker().cfg;}if(s.br.state==1)BbStart(s,cfg,*x);else if(s.br.state==2||s.br.state==3)BbContinue(s,cfg,*x);};
 RotSubmit(std::move(r));}
inline void BbAdvance(BbState& st,const BbConfig& cfg,const BbCtx& x){if(cfg.rotate)BbRotate(st,cfg,x,true);else if(st.br.state==1)BbStart(st,cfg,x);else BbContinue(st,cfg,x);}
// Stop: returns true if the target is destroyed (normal stop)
inline bool BbStop(BbState& st,const BbCtx* x,bool cancel){auto& b=st.br;const bool wasBreaking=b.state==2||b.state==3;
 if(cancel&&wasBreaking&&x&&x->gm){const uint64_t f=Virtual0(x->c,x->gm,0x20);if(f){int32_t at[3]={b.N[0],b.N[1],b.N[2]};reinterpret_cast<void(*)(uint64_t,int32_t*)>(f)(x->gm,at);}}
 const bool destroyed=!cancel&&!memcmp(b.N,b.P,12);b.state=0;b.active=false;b.id=0;BedBreaker().current=0;RotRelease(kRotBedBreaker);return destroyed;}
inline void BbStopRemember(BbState& st,const BbCtx* x){if(!st.br.active)return;int32_t P[3];memcpy(P,st.br.P,12);if(!BbStop(st,x,true)){memcpy(st.memory,P,12);st.remembered=true;}}
// Start and action-queue sequence
inline void BbBegin(BbState& st,const BbConfig& cfg,const BbPlan& plan){auto& b=st.br;auto& s=BedBreaker();b.id=++b.counter;if(!b.id)b.id=++b.counter;s.current=b.id;
 memcpy(b.P,plan.P,12);memcpy(b.N,plan.N,12);b.face=plan.face;b.slot=plan.slot;b.state=1;b.active=true;b.wait=ActionNs();b.best=0;
 const uint32_t id=b.id;const int slot=plan.slot;ActionSequence q;q.module=kBedBreakerAction;q.silent=cfg.ghost?1:0;
 q.steps.push_back([slot](ActionCtx& x){ActionSwitch(x,slot);return 1;});if(!cfg.ghost)q.steps.push_back(StepWait(50.0));
 q.steps.push_back([id](ActionCtx&){auto& s=BedBreaker();s.slotReady=id;return s.current.load()==id?1:2;});
 q.steps.push_back([id](ActionCtx&){return BedBreaker().current.load()==id?0:1;});q.steps.push_back(StepSwitchBack());
 q.done=[id](int result){if(result==1)BedBreaker().seqDone=id;};ActionSubmit(std::move(q));}
// Breaker tick
inline void BbBreakerTick(BbState& st,const BbConfig& cfg,const BbCtx& x){auto& b=st.br;auto& s=BedBreaker();
 if(s.seqDone.load()==b.id){BbStopRemember(st,&x);return;}
 if(b.state==4){const bool hit=b.gone&&BbGone(st,cfg,x);if(hit||b.second){BbStop(st,&x,false);return;}if(ActionMs(b.confirm)>1500.0){BbStop(st,&x,false);return;}if(cfg.rotate)BbRotate(st,cfg,x,false);return;}
 if(!BbValid(st,cfg,x)){BbStopRemember(st,&x);return;}
 if(b.state==2||b.state==3){const double clk=BbGmClock(x.gm);if(clk<=b.clock){const uint64_t f=Virtual0(x.c,x.gm,0x48);if(f)reinterpret_cast<void(*)(uint64_t)>(f)(x.gm);}b.clock=BbGmClock(x.gm);
  float progress{};Read(x.gm+0x24,&progress);if(progress>b.best){b.best=progress;b.stall=ActionNs();}else if(ActionMs(b.stall)>1500.0){BbStopRemember(st,&x);return;}
  BbAdvance(st,cfg,x);return;}
 if(b.state==1){if(ActionMs(b.wait)>1000.0){BbStopRemember(st,&x);return;}if(s.slotReady.load()==b.id)BbAdvance(st,cfg,x);}}
inline void BbSnap(BbState& st,const BbCtx* x){auto& s=BedBreaker();BbSnapshot n{};const auto& b=st.br;n.state=b.active?b.state:0;n.uncover=b.active&&memcmp(b.N,b.P,12)!=0;memcpy(n.n,b.N,12);memcpy(n.p,b.P,12);
 if(n.state==4)n.progress=1.f;else if((n.state==2||n.state==3)&&x&&x->gm){float p{};Read(x->gm+0x24,&p);n.progress=std::clamp(p,0.f,1.f);}
 std::lock_guard<std::mutex> l(s.guard);s.snap=n;}

// ---- Main loop
inline void BedBreakerWork(){auto& s=BedBreaker();auto& st=BbData();BbConfig cfg;{std::lock_guard<std::mutex> l(s.guard);cfg=s.cfg;}
 bbCurrentCtx=nullptr;Context c{};const bool have=Local(&c);BbCtx x{};x.c=c;
 if(have){x.actor=c.actor;Read(c.actor+0xac8,&x.gm);x.region=ActorRegion(c);}
 if(!cfg.enabled){if(st.br.active||!st.chunks.empty()||st.wasEnabled){BbStopRemember(st,x.gm?&x:nullptr);st.level=0;st.region=0;BbClearCaches(st,cfg);st.spawnKnown=false;st.prevKnown=false;st.wasAlive=false;st.blocks.clear();st.remembered=false;}
  st.wasEnabled=false;BbSnap(st,nullptr);return;}
 st.wasEnabled=true;
 uint64_t where{},bb{};if(!have||!x.region||!x.gm||!Read(c.actor+0x240,&where)||!where||!Floats(where,x.eye,3)||!Read(c.actor+0x248,&bb)||!bb||!Floats(bb,x.box,6)||!Read(x.region+0x3a,&x.minY)){
  BbStopRemember(st,x.gm?&x:nullptr);BbClearCaches(st,cfg);BbSnap(st,nullptr);return;}
 x.range=cfg.range;x.noWalls=!cfg.throughWalls;bbLastCtx=x;bbCurrentCtx=&bbLastCtx;bbTickThread=GetCurrentThreadId();   // kept until the rotation manager pops its stack ("reached" callback)
 uint64_t level{};Read(c.actor+0x200,&level);
 if(level!=st.level||x.region!=st.region){st.spawnKnown=false;st.prevKnown=false;st.wasAlive=false;BbClearCaches(st,cfg);st.level=level;st.region=x.region;}
 else if(st.blocks!=cfg.blocks)BbClearCaches(st,cfg);
 // Scan, 1 ms budget
 const int64_t t0=ActionNs();auto late=[&]{return ActionMs(t0)>1.0;};++st.gen;
 const uint64_t chunkAt=Virtual(c,x.region,0x148);
 if(chunkAt){float lift=0;uintptr_t off{};if(Element(c.registry,c.entity,"OffsetsComponent",0x44,&off))Floats(off,&lift,1);const int fx=(int)std::floor(x.eye[0]),fz=(int)std::floor(x.eye[2]);(void)lift;
  static const int offs[9][2]={{0,0},{-1,0},{1,0},{0,-1},{0,1},{-1,-1},{-1,1},{1,-1},{1,1}};st.active.clear();
  for(auto& o:offs){int cp[2]={(fx>>4)+o[0],(fz>>4)+o[1]};const uint64_t chunk=reinterpret_cast<uint64_t(*)(uint64_t,int*)>(chunkAt)(x.region,cp);if(!chunk)continue;uint64_t begin{},end{};
   if(!Read(chunk+0x1c8,&begin)||!Read(chunk+0x1d0,&end)||end<begin)continue;const uint32_t count=(uint32_t)std::min<uint64_t>((end-begin)/0x68,64);const uint64_t key=((uint64_t)(uint32_t)cp[0]<<32)|(uint32_t)cp[1];BbChunk& r=st.chunks[key];
   if(r.chunk!=chunk||r.begin!=begin||(r.read&&r.count!=count)){r.chunk=chunk;r.begin=begin;r.count=count;r.read=false;r.x=cp[0];r.z=cp[1];r.subs.assign(count,BbSub{});}r.count=count;if(r.subs.size()!=count)r.subs.resize(count);r.gen=st.gen;st.active.push_back(key);}
  for(auto it=st.chunks.begin();it!=st.chunks.end();)it=it->second.gen==st.gen?std::next(it):st.chunks.erase(it);if(st.robin>=st.active.size())st.robin=0;
  std::vector<uint64_t> dirty;std::vector<BbShared::Hit> hits;{std::lock_guard<std::mutex> l(s.pendGuard);dirty.swap(s.dirty);hits.swap(s.hits);}
  auto scan=[&](BbChunk& r,uint32_t i){BbScanSub(c,st,r.subs[i],r.begin+0x68ull*i,r.x,r.z,(int)i,x.minY);};
  if(!dirty.empty()){std::sort(dirty.begin(),dirty.end());dirty.erase(std::unique(dirty.begin(),dirty.end()),dirty.end());
   for(uint64_t k:st.active){BbChunk& r=st.chunks[k];if(!r.read)continue;for(uint64_t a:dirty)if(a>=r.begin&&a<r.begin+0x68ull*r.count&&(a-r.begin)%0x68==0)scan(r,(uint32_t)((a-r.begin)/0x68));}}
  for(uint64_t k:st.active){if(late())break;BbChunk& r=st.chunks[k];if(r.read)continue;for(uint32_t i=0;i<r.count;++i)scan(r,i);r.read=true;}
  for(size_t turn=0;turn<st.active.size()&&!late();++turn){st.robin=(st.robin+1)%st.active.size();BbChunk& r=st.chunks[st.active[st.robin]];if(!r.read)continue;
   for(uint32_t i=0;i<r.count;++i){const uint64_t a=r.begin+0x68ull*i;uint64_t l0{},l1{};if(!Read(a+0x30,&l0)||!Read(a+0x38,&l1))continue;BbSub& sub=r.subs[i];if(l0!=sub.l0||l1!=sub.l1||BbPalette(c,l0)!=sub.s0||BbPalette(c,l1)!=sub.s1)scan(r,i);}}
  // Break confirmation
  if(st.br.active&&st.br.state==4)for(auto& h:hits){if(h.layer!=0)continue;int cp[2]={st.br.N[0]>>4,st.br.N[2]>>4};const uint64_t chunk=reinterpret_cast<uint64_t(*)(uint64_t,int*)>(chunkAt)(x.region,cp);uint64_t begin{};
   if(!chunk||!Read(chunk+0x1c8,&begin))continue;const int i=(st.br.N[1]-x.minY)>>4;if(i<0||h.sub!=begin+0x68ull*i)continue;
   if(h.index!=(uint16_t)((st.br.N[1]&15)|((st.br.N[2]&15)<<4)|((st.br.N[0]&15)<<8)))continue;if(!st.br.gone)st.br.gone=true;else st.br.second=true;}}
 // Spawn point
 {float h{},m{};const bool alive=ReadHealth(&h,&m)&&h>0.f;
  if(alive&&st.prevKnown){const float dx=x.eye[0]-st.prev[0],dy=x.eye[1]-st.prev[1],dz=x.eye[2]-st.prev[2];if(dx*dx+dy*dy+dz*dz>1024.f||!st.wasAlive){memcpy(st.spawn,x.eye,12);st.spawnKnown=true;}}
  st.wasAlive=alive;memcpy(st.prev,x.eye,12);st.prevKnown=true;
  // Pauses
  uint8_t breaking{};Read(c.actor+0xb4a,&breaking);int32_t gameType=-1;uintptr_t type{};if(Element(c.registry,c.entity,"ActorGameTypeComponent",4,&type))Read(type,&gameType);
  if(breaking||gameType==1||gameType==6||!alive||cfg.othersOn){BbStopRemember(st,&x);BbSnap(st,&x);return;}}
 // Target choice or breaker tick
 if(st.br.active){BbBreakerTick(st,cfg,x);BbSnap(st,&x);return;}
 if(ActionCursorBusy(c,c.actor)){BbSnap(st,&x);return;}
 const int L=cfg.through?cfg.maxLayers:0;std::unordered_map<uint64_t,BbCost> costs;BbPlan best{};
 if(st.remembered){best=BbEvaluate(st,cfg,x,st.memory,L,costs);}
 if(!best.ok){const float lim=(float)L+cfg.range;
  for(auto& e:st.chunks)for(auto& sub:e.second.subs)for(auto& p:sub.pos){if(BbDist2(x.eye,p.data())>lim*lim)continue;const BbPlan r=BbEvaluate(st,cfg,x,p.data(),L,costs);if(!r.ok)continue;
   if(!best.ok||r.cost<best.cost||(r.cost==best.cost&&r.dist<best.dist))best=r;}}
 st.remembered=false;if(best.ok)BbBegin(st,cfg,best);BbSnap(st,&x);}
inline void BedBreakerTick(){auto& s=BedBreaker();if(s.fault)return;__try{BedBreakerWork();}__except(EXCEPTION_EXECUTE_HANDLER){InterlockedExchange(&s.fault,1);if(actionLog)actionLog("Bed Breaker: fault caught, module stopped until restart");}}
inline void BedBreakerSetBlock(uint64_t sub,uint8_t layer,uint16_t index){auto& s=BedBreaker();if(s.fault)return;{std::lock_guard<std::mutex> l(s.guard);if(!s.cfg.enabled)return;}
 std::lock_guard<std::mutex> l(s.pendGuard);if(s.dirty.size()<4096)s.dirty.push_back(sub);if(s.hits.size()<4096)s.hits.push_back({sub,layer,index});}
}
