// Player movement simulator shared by Auto MLG (game_movement_d.hpp) and Parkour (game_movement_b.hpp).
//
// A self-contained re-implementation of the game's local player movement, run on a copy of the player's state, one
// call of MsTick = one game tick. Nothing is written to the game; the world is only read:
// - block collision shapes through BlockSource vtable +0xb0 (fetch collision shapes: region, std::vector of 0x30-byte
//   records {+8 AABB, +0x20 block position, +0x2c flag}, query box, 1, 0, 0) into a game-allocated vector of 0x2000
//   records; chunk presence through BlockSource vtable +0x148 (chunk position {x, z});
// - blocks through vtable +0x10 (layer 0) and +0x28 (liquid layer); names from BlockLegacy +0xe8, flag bytes
//   BlockLegacy +0x28 (0 = air) / +0x29 (bit 1 liquid, (b & 0x17) == 0x14 solid); liquid depth from the NBT
//   "states" / "liquid_depth" of the block (+0xf0);
// - components of the local entity: AABB shape 0xbac1b3cf (0x20: box + width/height), fall distance 0xce6b34f6,
//   actor data flags (bits 1 sneak, 2 riding, 3 sprint, 32 glide, 57 swim, 114 crawl), abilities 0x6de78dc5, game
//   mode 0x88d3eddf, on ground 0xc29078a0, move input (vector +0x24, flags +0x60), head rotation 0xbabe7211,
//   attributes (9 = movement speed, value +0x7c), effects (8 jump boost, 24 levitation, 27 slow falling, 33 weaving),
//   armor enchantments (depth strider 7, soul speed 36, swift sneak 37, riptide 30 on the held item), elytra in the
//   chest slot, leather boots, firework rockets attached to the player (entity type 72).
// Physics implemented (same constants as the game): collision stepping axis by axis against the shapes with the 0.6
// step-up, friction from the block below (ice 0.98, blue ice 0.989, slime and honey 0.8, else 0.6), ground speed
// 0.546 / friction^3, air speed 0.02 / 0.026 sprinting, gravity 0.08 and drag 0.98, jump 0.42 (+0.1 per Jump Boost
// level, +0.2 sprint boost), sneaking pose 1.5, swimming / gliding / crawling poses, sneak edge protection, ladders and
// vines (0.2 climb, -0.15 descent clamp), cobweb / powder snow / sweet berry slowdown, honey and soul sand, bed and slime
// bounces, water and lava movement with flow, levitation and slow falling, elytra glide with rockets, fall distance.
// The state is a byte image laid out like the original (offsets in comments below).
#pragma once
#include "game_actions.hpp"
#include "game_player.hpp"
#include "game_sticky_aim.hpp"
#include "game_xray.hpp"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace game {
// ---- the game's allocator (the shape vector is filled by the game, which may grow it) ----------------------------
struct MsHeapFns{void*(*allocate)(size_t)=nullptr;void(*release)(void*)=nullptr;};
inline MsHeapFns& MsHeap(){
 static MsHeapFns heap=[]{MsHeapFns h;HMODULE redirect=GetModuleHandleW(L"MallocRedirect.dll");
  if(redirect){h.allocate=(void*(*)(size_t))GetProcAddress(redirect,"malloc");h.release=(void(*)(void*))GetProcAddress(redirect,"free");}
  if(!h.allocate||!h.release){HMODULE crt=GetModuleHandleW(L"ucrtbase.dll");if(crt){h.allocate=(void*(*)(size_t))GetProcAddress(crt,"malloc");h.release=(void(*)(void*))GetProcAddress(crt,"free");}}
  if(!h.allocate||!h.release){h.allocate=::malloc;h.release=::free;}return h;}();
 return heap;
}
inline uint64_t MsAllocate(uint64_t bytes){
 if(!bytes)return 0;auto& h=MsHeap();if(bytes<0x1000)return (uint64_t)h.allocate((size_t)bytes);
 void* raw=h.allocate((size_t)(bytes+0x27));if(!raw)return 0;const uint64_t aligned=((uint64_t)raw+0x27)&~0x1full;*(uint64_t*)(aligned-8)=(uint64_t)raw;return aligned;
}
inline void MsFree(uint64_t pointer,uint64_t bytes){
 if(!pointer)return;auto& h=MsHeap();
 if(bytes>=0x1000){const uint64_t raw=*(uint64_t*)(pointer-8);if(pointer-8-raw>0x1f)return;h.release((void*)raw);return;}
 h.release((void*)pointer);
}
// MSVC std::vector of 0x30-byte collision records (begin, end, capacity). Freed only on the game thread.
struct MsShapes{uint64_t v[3]{};};
inline void MsShapesFree(MsShapes& s){if(s.v[0])MsFree(s.v[0],s.v[2]-s.v[0]);s.v[0]=s.v[1]=s.v[2]=0;}

// ---- environment: world access + configuration bytes + scratch vectors ------------------------------------------
// cfg mirrors the original configuration block: [0] mode (2), [4] / [8] float limits, [0xc] eye offset box, [0xd]
// force sneak test, [0xe] clamp velocity, [0x10] clamp, [0x14] skip distance test (1), [0x18] int64 swim delay,
// [0x20] byte. Auto MLG and Parkour both use mode 2 and [0x14] = 1, the rest zero.
struct MsEnv{
 Context c{};uintptr_t actor=0;uint64_t region=0;MsShapes* shapes=nullptr;
 uint8_t cfg[0x28]{};
 std::vector<float> boxes;     // world boxes (6 floats each)
 std::vector<float> local;     // block-relative boxes (6 floats each)
 std::vector<int32_t> cellsA,cellsB;   // liquid cells (x, y, z)
 std::vector<float> axisA,axisB;       // face coverage breakpoints
};
struct MsState{alignas(16) uint8_t m[0x230];};
struct MsInput{alignas(16) uint8_t m[0x58];};
struct MsOutput{alignas(16) uint8_t m[0x58];};

#define MF(o) (*(float*)(s+(o)))
#define MB(o) (*(uint8_t*)(s+(o)))
#define MI(o) (*(int32_t*)(s+(o)))
#define MQ(o) (*(int64_t*)(s+(o)))
#define MU(o) (*(uint64_t*)(s+(o)))

inline bool MsFinite(float f){uint32_t u;memcpy(&u,&f,4);return (u&0x7fffffff)<0x7f800000;}
inline int MsToInt(float f){if(!(f>=-2147483648.f))f=-2147483648.f;if(f>2147483520.f)f=2147483520.f;return (int)f;}
inline float MsClampMin(float v,float lo){return lo<=v?v:lo;}
// The game's table sine: sin(((int64)(x * 10430.378) & 0xffff) * 2pi / 65536).
inline float MsTableSin(float x){
 const float f=x*10430.3779296875f;double d=0.0;
 if(f<9.223372036854776e18f){d=(double)((uint32_t)(int64_t)f&0xffff)*3.141592653589793;d=(d+d)*1.52587890625e-05;}
 if(f<-9.223372036854776e18f)d=0.0;return (float)std::sin(d);
}
// Same on an already scaled value (scaled = angle * 10430.378), as several callers compute it once.
inline double MsTableSinScaled(float f){
 double d=0.0;if(f<9.223372036854776e18f){d=(double)((uint32_t)(int64_t)f&0xffff)*3.141592653589793;d=(d+d)*1.52587890625e-05;}
 if(f<-9.223372036854776e18f)d=0.0;return std::sin(d);
}
// Swimming pose active (flag 57 with water around or the swim delay running).
inline bool MsSwim(const uint8_t* s){return MB(0x1b8)==1&&(MB(0x1c1)!=0||MQ(0x1c8)>0);}
inline float MsHeight(const uint8_t* s){if(MsSwim(s)||(MB(0x1dc)&1))return MF(0xbc);return MI(0x1f4)<1?MF(0xc0):MF(0xbc);}
inline float MsEyeOff(const MsEnv& e,const uint8_t* s){return e.cfg[0xc]?MF(0xb0):0.f;}

// ---- world reads ---------------------------------------------------------------------------------------------
inline uint64_t MsVirtual(const MsEnv& e,uint64_t object,uint32_t off){return Virtual0(e.c,object,off);}
inline uint64_t MsBlock(const MsEnv& e,const int32_t* p){return RegionBlock(e.c,e.region,p);}
inline uint64_t MsLiquidLayer(const MsEnv& e,const int32_t* p){const uint64_t f=MsVirtual(e,e.region,0x28);return f?reinterpret_cast<uint64_t(*)(uint64_t,const int32_t*)>(f)(e.region,p):0;}
inline bool MsLegacy(uint64_t block,uint8_t* a,uint8_t* b){return BlockFlags(block,a,b);}
inline bool MsAir(uint64_t block){uint8_t a{},b{};return MsLegacy(block,&a,&b)&&a==0;}
inline bool MsSolidBlock(uint64_t block){uint8_t a{},b{};return MsLegacy(block,&a,&b)&&(b&0x17)==0x14;}
inline std::string MsName(uint64_t block){std::string n;if(block)XrayBlockName((uintptr_t)block,&n);return n;}
// Chunks present over the box (all of them).
inline bool MsLoaded(const MsEnv& e,const float* box){
 const int x0=(int)std::floor(box[0])>>4,x1=(int)std::floor(box[3])>>4,z0=(int)std::floor(box[2])>>4,z1=(int)std::floor(box[5])>>4;
 if(x0>x1||z0>z1)return true;const uint64_t f=MsVirtual(e,e.region,0x148);if(!f)return false;
 for(int x=x0;x<=x1;++x)for(int z=z0;z<=z1;++z){const int32_t cp[2]={x,z};if(!reinterpret_cast<uint64_t(*)(uint64_t,const int32_t*)>(f)(e.region,cp))return false;}
 return true;
}
// Box within the world limits (coordinates inside int range, at most 256 chunks wide, 4096 blocks high).
inline bool MsBoxSane(const float* b){
 auto in=[](float v){return v<=2147483520.f&&-2147483648.f<=v&&MsFinite(v);};
 const float x0=std::floor(b[0]),y0=std::floor(b[1]),z0=std::floor(b[2]);
 if(!in(x0)||!in(y0)||!in(z0))return false;
 const float x1=std::ceil(b[3])-1.f,y1=std::ceil(b[4])-1.f,z1=std::ceil(b[5])-1.f;
 if(!in(x1)||!in(y1)||!in(z1))return false;
 const int dx=((int)x1>>4)-((int)x0>>4),dz=((int)z1>>4)-((int)z0>>4);const uint64_t dy=(uint64_t)((int64_t)(int)y1-(int64_t)(int)y0);
 return dy<0x1000&&dx<256&&dz<256&&dx>-1&&dz>-1;
}
// Fetch the collision records over a box into the game vector (capacity 0x2000 records, cleared first).
inline bool MsFetch(MsEnv& e,const float* box){
 MsShapes& v=*e.shapes;const uint64_t f=MsVirtual(e,e.region,0xb0);if(!f)return false;
 if(!v.v[0]||(v.v[2]-v.v[0])/0x30<0x2000){const uint64_t fresh=MsAllocate(0x2000ull*0x30);if(!fresh)return false;
  if(v.v[0])MsFree(v.v[0],v.v[2]-v.v[0]);v.v[0]=v.v[1]=fresh;v.v[2]=fresh+0x2000ull*0x30;}
 v.v[1]=v.v[0];float q[6];memcpy(q,box,24);
 reinterpret_cast<void(*)(uint64_t,void*,const float*,int,uint64_t,uint64_t)>(f)(e.region,v.v,q,1,0,0);return true;
}
// Boxes of the records strictly overlapping the query box (by more than 1e-5 on every axis), appended to out.
inline void MsCollect(MsEnv& e,const float* box,std::vector<float>& out){
 if(!MsFetch(e,box))return;const MsShapes& v=*e.shapes;
 for(uint64_t r=v.v[0];r!=v.v[1];r+=0x30){const float* a=(const float*)(r+8);
  if(1e-5f<box[3]-a[0]&&1e-5f<a[3]-box[0]&&1e-5f<box[4]-a[1]&&1e-5f<a[4]-box[1]&&1e-5f<box[5]-a[2]&&1e-5f<a[5]-box[2])out.insert(out.end(),a,a+6);}
}
// Drops invalid boxes (non-finite or empty), keeping the order.
inline void MsCompact(std::vector<float>& v){
 size_t w=0;for(size_t r=0;r+6<=v.size();r+=6){const float* a=&v[r];
  const bool ok=MsFinite(a[0])&&MsFinite(a[3])&&a[0]<a[3]&&MsFinite(a[1])&&MsFinite(a[4])&&a[1]<a[4]&&MsFinite(a[2])&&MsFinite(a[5])&&a[2]<a[5];
  if(ok){if(w!=r)memmove(&v[w],a,24);w+=6;}}
 v.resize(w);
}
inline void MsCollectClean(MsEnv& e,const float* box,std::vector<float>& out){out.clear();MsCollect(e,box,out);MsCompact(out);}
// Shapes of the block at a cell, relative to the cell (records whose block position is the cell).
inline void MsCellShapes(MsEnv& e,const int32_t* cell,std::vector<float>& out){
 out.clear();const float x=(float)cell[0],y=(float)cell[1],z=(float)cell[2];const float box[6]={x,y,z,x+1.f,y+1.f,z+1.f};
 if(!MsFetch(e,box))return;const MsShapes& v=*e.shapes;
 for(uint64_t r=v.v[0];r!=v.v[1];r+=0x30){const int32_t* p=(const int32_t*)(r+0x20);if(p[0]!=cell[0]||p[1]!=cell[1]||p[2]!=cell[2])continue;const float* a=(const float*)(r+8);
  const float rel[6]={a[0]-x,a[1]-y,a[2]-z,a[3]-x,a[4]-y,a[5]-z};out.insert(out.end(),rel,rel+6);}
}
// Liquid at a cell: {lava, height (8 - depth & 7), falling (depth > 7), valid}; layer 0 then the liquid layer.
struct MsLiquid{bool lava=false;int height=0;bool falling=false;bool valid=false;};
inline int MsLiquidDepth(uint64_t block){
 const uint64_t states=NbtNode(block+0xf0,"states");if(!states)return 0;const uint64_t tag=states+0x40;uint8_t type{};if(!Read(tag+0x28,&type)||type!=10)return 0;
 const uint64_t depth=NbtNode(tag,"liquid_depth");if(!depth)return 0;uint8_t t{};int32_t v{};if(!Read(depth+0x40+0x28,&t)||t!=3||!Read(depth+0x40+8,&v))return 0;return v;
}
inline void MsLiquidOf(uint64_t block,MsLiquid* out){
 uint8_t a{},b{};if(!MsLegacy(block,&a,&b)||!(b&2)){out->valid=false;return;}
 const std::string n=MsName(block);const int d=MsLiquidDepth(block);
 out->lava=n.size()>3&&n.find("lava")!=std::string::npos;out->height=8-(d&7);out->falling=7<d;out->valid=true;
}
inline MsLiquid MsLiquidAt(const MsEnv& e,const int32_t* p){MsLiquid l;MsLiquidOf(MsBlock(e,p),&l);if(!l.valid)MsLiquidOf(MsLiquidLayer(e,p),&l);return l;}
inline float MsLiquidHeight(const MsLiquid& l){return l.falling?1.f:(float)(l.height+1)/9.f;}
// Block properties: friction, speed factor, bounce (bed 2, slime 1), powder snow / scaffolding / sweet berry bytes,
// air, climbable (ladder, vines), cobweb, honey, fence / wall, soul sand.
struct MsProps{float friction=0.6f,factor=1.f;uint8_t bounce=0,b9=0,b10=0,air=0,climb=0,web=0,honey=0,fence=0,soul=0;};
inline MsProps MsPropsOf(const std::string& n){
 MsProps p;p.air=n=="minecraft:air";
 static const char* climb[7]={"minecraft:ladder","minecraft:vine","minecraft:cave_vines","minecraft:cave_vines_body_with_berries","minecraft:cave_vines_head_with_berries","minecraft:twisting_vines","minecraft:weeping_vines"};
 for(auto c:climb)if(n==c)p.climb=1;
 auto ends=[&](const char* e){const size_t l=strlen(e);return n.size()>=l&&!n.compare(n.size()-l,l,e);};
 p.web=n=="minecraft:web";p.honey=n=="minecraft:honey_block";
 p.fence=(n.size()>=6&&ends("_fence"))||(n.size()>9&&ends("fence_gate"))||(n.size()>=5&&ends("_wall"));
 static const struct{const char* n;float f;}ice[6]={{"minecraft:ice",0.98f},{"minecraft:frosted_ice",0.98f},{"minecraft:packed_ice",0.98f},{"minecraft:blue_ice",0.989f},{"minecraft:slime",0.8f},{"minecraft:honey_block",0.8f}};
 for(auto& i:ice)if(n==i.n){p.friction=i.f;break;}
 if(n=="minecraft:bed")p.bounce=2;else if(n=="minecraft:slime")p.bounce=1;
 else if(n=="minecraft:soul_sand"){uint32_t bits=0x3f9ccccd;memcpy(&p.factor,&bits,4);p.soul=1;}
 else if(n=="minecraft:powder_snow"){p.b9=2;p.b10=2;}else if(n=="minecraft:scaffolding")p.b10=1;
 else if(n=="minecraft:sweet_berry_bush")p.b9=1;
 return p;
}
inline MsProps MsPropsAt(const MsEnv& e,const int32_t* p){return MsPropsOf(MsName(MsBlock(e,p)));}

// ---- entity reads ----------------------------------------------------------------------------------------------
inline uintptr_t MsComponent(const MsEnv& e,uint32_t hash,uint32_t stride){uintptr_t st{},el{};return Storage(e.c.registry,hash,&st)&&InStorage(st,e.c.entity,stride,&el)?el:0;}
inline bool MsFlag(const MsEnv& e,int bit){const uintptr_t f=MsComponent(e,0xc67426f3u,0x18);if(!f||bit>=0x82)return false;uint64_t w{};return Read(f+(uint64_t)(bit>>6)*8,&w)&&((w>>(bit&63))&1);}
inline bool MsEffect(const MsEnv& e,int which,int* amplifier){
 static const int ids[3]={8,24,27};const int id=which<3?ids[which]:0x21;const int a=EffectLevel(e.c.registry,e.c.entity,id);if(a<0)return false;*amplifier=a;return true;
}
inline int MsEnchantSafe(const MsEnv& e,uint64_t stack,int id){if(!ActionItem(stack))return 0;const int l=EnchantLevel(e.c,stack,(int16_t)id);return l>0?l:0;}
// mode 0 depth strider (boots), 1 soul speed (boots), 2 swift sneak (leggings), other: riptide on the held item.
inline int MsArmorEnchant(const MsEnv& e,int mode){
 if(mode==0||mode==1||mode==2){const uint64_t armor=EquipmentContainer(e.c,e.actor,1);if(!armor)return 0;const int n=ActionSize(e.c,armor);
  const int slot=mode==2?2:3;if(n<=slot)return 0;return MsEnchantSafe(e,ActionStackAt(e.c,armor,slot),mode==0?7:mode==1?36:37);}
 return MsEnchantSafe(e,ActionHeld(e.c,e.actor),30);
}
inline bool MsArmorIs(const MsEnv& e,int slot,const char* name){const uint64_t armor=EquipmentContainer(e.c,e.actor,1);if(!armor||ActionSize(e.c,armor)<=slot)return false;return ActionNameIs(ActionStackAt(e.c,armor,slot),name);}
// Firework rockets (entity type 72) whose synched owner (data item 141, +0x10) is the local player.
inline int MsRockets(const MsEnv& e){
 uintptr_t uidEl{};uint64_t uid{};if(!Element(e.c.registry,e.c.entity,"ActorUniqueIDComponent",8,&uidEl)||!Read(uidEl,&uid))return 0;
 uintptr_t types{},data{};if(!Storage(e.c.registry,0x4f6ba419u,&types)||!Storage(e.c.registry,0xe642016bu,&data))return 0;
 uint64_t tb{},te{},db{},de{};Read(types+0x20,&tb);Read(types+0x28,&te);Read(data+0x20,&db);Read(data+0x28,&de);
 const uintptr_t walk=(de-db)<(te-tb)?data:types;uint64_t pb{},pe{};if(!Read(walk+0x20,&pb)||!Read(walk+0x28,&pe)||pe<pb||(pe-pb)/4>0x40000)return 0;
 int count=0;for(uint64_t at=pe;at>pb;){at-=4;uint32_t ent{};if(!Read(at,&ent))break;uintptr_t te_{},de_{};
  if(!InStorage(types,ent,4,&te_)||!InStorage(data,ent,0x48,&de_))continue;int32_t type{};if(!Read(te_,&type)||type!=0x48)continue;
  uint64_t vb{},ve{},item{},owner{};if(!Read(de_,&vb)||!Read(de_+8,&ve)||ve-vb<=0x468||!Read(vb+0x468,&item)||!item||!Read(item+0x10,&owner))continue;if(owner==uid)++count;}
 return count;
}

// ---- state sanity -------------------------------------------------------------------------------------------------
inline bool MsSane(const uint8_t* s){
 static const int vec[19]={0x00,0x0c,0x18,0x24,0x30,0x3c,0x4c,0x58,0x64,0x70,0x7c,0x88,0x94,0xa0,0x15c,0x184,0x174,0xbc,0xc8};
 for(int o:vec)for(int k=0;k<3;++k)if(!MsFinite(MF(o+4*k)))return false;
 static const int one[14]={0xd4,0xd8,0xdc,0x124,0x128,0x12c,0x130,0x134,0x138,0x13c,0x140,0x144,0x148,0x1bc};
 for(int o:one)if(!MsFinite(MF(o)))return false;
 return MsFinite(MF(0xac))&&MsFinite(MF(0xb0))&&MsFinite(MF(0xb8))&&MsFinite(MF(0xb4));
}
inline bool MsInputSane(const uint8_t* in){
 auto f=[&](int o){float v;memcpy(&v,in+o,4);return v;};
 if(in[0x4c]==1){const float v=f(0x48);if(!MsFinite(v)||v<0.f||1.f<v)return false;}
 if(!MsFinite(f(0))||!MsFinite(f(4)))return false;
 for(int o=0x18;o<=0x2c;o+=4)if(!MsFinite(f(o)))return false;
 return MsFinite(f(0xc))&&MsFinite(f(0x10))&&MsFinite(f(0x14));
}
inline bool MsPositive(float v){return MsFinite(v)&&v>0.f;}
inline void MsPrevVel(uint8_t* s){memcpy(s+0x70,s+0x64,12);}

// ---- boxes --------------------------------------------------------------------------------------------------------
// Standard box of the current position (feet at +0x4c, width +0xbc, height MsHeight, scale +0xc4, eye offset).
inline void MsStdBox(const MsEnv& e,const uint8_t* s,float* o){
 const float hw=0.5f*MF(0xbc)*MF(0xc4),yoff=MsEyeOff(e,s),h=MsHeight(s);
 const float a0=MF(0x4c)+hw,a1=h*MF(0xc4)+MF(0x50)+yoff,a2=hw+MF(0x54),b0=MF(0x4c)-hw,b1=yoff+MF(0x50),b2=MF(0x54)-hw;
 o[0]=b0<=a0?b0:a0;o[1]=b1<=a1?b1:a1;o[2]=b2<=a2?b2:a2;o[3]=a0<=b0?b0:a0;o[4]=a1<=b1?b1:a1;o[5]=a2<=b2?b2:a2;
}
inline void MsStoreBox(const MsEnv& e,uint8_t* s,const float* box){
 memcpy(s+0xf0,box,24);memcpy(s+0x108,s+0x4c,12);MF(0x114)=0.5f*MF(0xbc)*MF(0xc4);MF(0x118)=MsHeight(s)*MF(0xc4);MF(0x11c)=MsEyeOff(e,s);MB(0x120)=1;
}
// Box used by the movement (cached box when position and dimensions are unchanged).
inline void MsBox(const MsEnv& e,const uint8_t* s,float* out){
 if(MB(0x120)==1&&MF(0x108)==MF(0x4c)&&MF(0x10c)==MF(0x50)&&MF(0x110)==MF(0x54)){
  const float hw=MF(0xbc)*0.5f*MF(0xc4);
  if(MF(0x114)==hw&&MF(0x118)==MF(0xc4)*MsHeight(s)){const float y=MF(0x11c);if(e.cfg[0xc]?y==MF(0xb0):y==0.f){memcpy(out,s+0xf0,24);return;}}
 }
 MsStdBox(e,s,out);
}
// Box fitting: keeps the box centred on the position against shapes whose edge mirrors the centre.
inline void MsFit(float* out,const float* b,const float* c,const std::vector<float>& sh){
 float nMinX=b[0],nMaxX=b[3],nMinZ=b[2],nMaxZ=b[5];
 for(size_t i=0;i+6<=sh.size();i+=6){const float* a=&sh[i];
  if(!(1e-5f<a[3]-b[0]&&1e-5f<b[3]-a[0]&&1e-5f<a[4]-b[1]&&1e-5f<b[4]-a[1]&&1e-5f<a[5]-b[2]&&1e-5f<b[5]-a[2]))continue;
  if(b[0]<a[3]&&a[3]<b[3]&&(a[3]+b[3])*0.5f==c[0])nMinX=a[3]<=nMinX?nMinX:a[3];
  if(a[0]<b[3]&&b[0]<a[0]&&(a[0]+b[0])*0.5f==c[0])nMaxX=nMaxX<=a[0]?nMaxX:a[0];
  if(b[2]<a[5]&&a[5]<b[5]&&(a[5]+b[5])*0.5f==c[2])nMinZ=a[5]<=nMinZ?nMinZ:a[5];
  if(a[2]<b[5]&&b[2]<a[2]&&(a[2]+b[2])*0.5f==c[2])nMaxZ=nMaxZ<=a[2]?nMaxZ:a[2];
 }
 if(nMaxX<=nMinX||(nMinX+nMaxX)*0.5f!=c[0]){nMinX=b[0];nMaxX=b[3];}
 if(!(nMinZ<nMaxZ&&(nMinZ+nMaxZ)*0.5f==c[2])){nMinZ=b[2];nMaxZ=b[5];}
 out[0]=nMaxX<nMinX?nMaxX:nMinX;out[1]=b[4]<b[1]?b[4]:b[1];out[2]=nMaxZ<nMinZ?nMaxZ:nMinZ;
 out[3]=nMinX<nMaxX?nMaxX:nMinX;out[4]=b[4]>b[1]?b[4]:b[1];out[5]=nMaxZ>nMinZ?nMaxZ:nMinZ;
}
// Box refresh before a tick: dimensions changed -> plain box; position changed (or no cache) -> fitted box.
inline void MsRefreshBox(MsEnv& e,uint8_t* s){
 const float hw=0.5f*MF(0xbc)*MF(0xc4),h=MsHeight(s),yoff=MsEyeOff(e,s);
 bool plain=false;
 if(MB(0x120)==1){plain=true;
  if(MF(0x114)==hw&&MF(0x118)==h*MF(0xc4)&&MF(0x11c)==yoff){
   if(MF(0x108)==MF(0x4c)&&MF(0x10c)==MF(0x50)&&MF(0x110)==MF(0x54))return;
   plain=false;}}
 float box[6];MsStdBox(e,s,box);
 if(plain){memcpy(s+0xf0,box,24);memcpy(s+0x108,s+0x4c,12);MF(0x114)=hw;MF(0x118)=MF(0xc4)*h;MF(0x11c)=yoff;MB(0x120)=1;return;}
 if(!MsBoxSane(box)||!MsLoaded(e,box))return;
 MsCollectClean(e,box,e.boxes);float fit[6];MsFit(fit,box,(const float*)(s+0x4c),e.boxes);
 memcpy(s+0xf0,fit,24);memcpy(s+0x108,s+0x4c,12);MF(0x114)=MF(0xbc)*0.5f*MF(0xc4);MF(0x118)=h*MF(0xc4);MF(0x11c)=yoff;MB(0x120)=1;
}
inline float MsAirSpeed(const uint8_t* s){return MB(0x1a2)?0.026000000536441803f:0.019999999552965164f;}
// Room for a box of the given height at the position: ok = world readable, empty = no collision box inside.
struct MsRoom{bool empty=false,ok=false;};
inline MsRoom MsBoxFree(MsEnv& e,const uint8_t* s,float height){
 const float x=MF(0x4c),y=MF(0x50),z=MF(0x54),yoff=e.cfg[0xc]?MF(0xb0):0.f,hw=MF(0xbc)*0.5f*MF(0xc4),h=height*MF(0xc4);
 float box[6];
 if(MB(0x120)==1&&MF(0x108)==x&&MF(0x10c)==y&&MF(0x110)==z&&MF(0x114)==hw&&MF(0x118)==h&&MF(0x11c)==yoff)memcpy(box,s+0xf0,24);
 else{const float a0=x+hw,a1=y+h+yoff,b0=x-hw,b1=y+yoff,az=z+hw,bz=z-hw;
  box[0]=a0<b0?a0:b0;box[1]=a1<b1?a1:b1;box[2]=bz<=az?bz:az;box[3]=a0>b0?a0:b0;box[4]=a1>b1?a1:b1;box[5]=az<=bz?bz:az;}
 MsRoom r;if(!MsBoxSane(box)||!MsLoaded(e,box))return r;
 MsCollectClean(e,box,e.boxes);r.empty=e.boxes.empty();r.ok=true;return r;
}
inline void MsSneakOff(uint8_t* s){MB(0x1a6)=0;MB(0x1ee)=0;MF(0xc0)=MF(0xd4);}
// Standing up if possible, else sneaking, else crawling.
inline bool MsTryStand(MsEnv& e,uint8_t* s){
 float box[6];MsBox(e,s,box);if(!MsBoxSane(box)||!MsLoaded(e,box))return false;
 const MsRoom a=MsBoxFree(e,s,MF(0xd4));if(!a.ok)return false;
 if(a.empty){MsSneakOff(s);return true;}
 const MsRoom b=MsBoxFree(e,s,MF(0xd8));if(!b.ok)return false;
 MB(0x1a6)=b.empty;MB(0x1ee)=!b.empty;MF(0xc0)=b.empty?MF(0xd8):MF(0xdc);return true;
}
// Sneaking pose from the input.
inline void MsPose(MsEnv& e,uint8_t* s,const uint8_t* in,bool canFetch,uint8_t* allOk){
 auto sneak=[&]{MB(0x1a6)=1;MF(0xc0)=MF(0xd8);};
 if(in[0x37]==1){MB(0x1a6)=1;if(MB(0x1ee)!=0)return;MF(0xc0)=MF(0xd8);return;}
 if(in[0x38]==1){
  if(MB(0x1ee)==0){
   if(!canFetch){sneak();return;}
   const MsRoom r=MsBoxFree(e,s,MF(0xd4));*allOk=*allOk?r.ok:0;
   if(!r.ok||!r.empty){sneak();return;}
   MB(0x1a6)=0;MF(0xc0)=MF(0xd4);return;}
 }else if(MB(0x1ee)==0){
  if(in[0x39]==1){sneak();return;}
  if(MB(0x1a6)!=1){MB(0x1a6)=0;MF(0xc0)=MF(0xd4);return;}
  if(canFetch){const MsRoom r=MsBoxFree(e,s,MF(0xd4));*allOk=*allOk?r.ok:0;if(r.ok&&r.empty){MB(0x1a6)=0;MF(0xc0)=MF(0xd4);return;}}
  MF(0xc0)=MF(0xd8);return;
 }
 MB(0x1a6)=0;
}
inline void MsSprintInput(uint8_t* s,const uint8_t* in){
 const uint8_t down=in[0x34],up=in[0x35];
 auto reset=[&]{MB(0x158)=0;MF(0x134)=MF(0x138);MF(0x13c)=0.019999999552965164f;};
 if((down&up)==1){MB(0x1a2)=0;reset();return;}
 if(down==0){
  if(up&1){MB(0x1a2)=0;if(MB(0x158)!=0){MF(0x13c)=0.019999999552965164f;return;}reset();return;}
  if(MB(0x1a2)==0){MF(0x13c)=0.019999999552965164f;return;}
 }else{MB(0x1a2)=1;MB(0x158)=0;MF(0x134)=MF(0x138)*1.2999999523162842f;}
 MF(0x13c)=0.026000000536441803f;
}
inline void MsTimers(uint8_t* s,bool tickInput){
 if(MQ(0x1e0)>0)MQ(0x1e0)--;
 if(MQ(0x150)>0&&--MQ(0x150)==0){MQ(0x150)=0;MF(0x148)=1.f;}
 MQ(0x168)++;MB(0x170)=0;
 if(tickInput&&MQ(0x190)!=-1)MQ(0x190)++;
 if(MU(0x1b0)!=0)MU(0x1b0)--;
 if(MI(0x1f4)>0&&--MI(0x1f4)==0){MB(0x1fa)=0;}
 MB(0x200)=0;MB(0x1c0)=0;
}
// Horizontal input vector, limited by sneak / crawl (swift sneak) and the input's own limits.
inline void MsMoveVector(MsEnv& e,float* out,uint8_t* s,const uint8_t* in,uint8_t full){
 float lim=1.f;
 if(full==0||in[8]!=0){
  if(in[0x4c]==1)memcpy(&lim,in+0x48,4);
  else if(in[0x50]!=0||(in[0x51]==1&&in[0x52]==0))lim=0.12250000238418579f;
  if((MB(0x1a6)&(MB(0x1c1)^1))==0&&MB(0x1ee)==0&&MB(0x1dc)==0)MI(0x1f0)=0;
  else{const int n=MI(0x1f0);MI(0x1f0)=n+1;float f=0.30000001192092896f;if(1<n)f=(float)MsArmorEnchant(e,2)*0.15000000596046448f+0.30000001192092896f;
   const float c=0.f<=f?f:0.f;lim*=1.f<f?1.f:c;}
 }
 float v[2];memcpy(v,in,8);
 for(int i=0;i<2;++i){float a=-lim>v[i]?-lim:v[i];out[i]=lim<a?lim:a;}
 if(in[8]==1)for(int i=0;i<2;++i){float a=-1.f>v[i]?-1.f:v[i];a=1.f<a?1.f:a;out[i]=lim*a;}
 if(in[0x53]==1){out[0]=out[1]=0.f;}
}
// Per-tick preparation from the input (pose, sprint, swimming, gliding, jump power, riptide end).
inline bool MsPrepare(MsEnv& e,uint8_t* s,const uint8_t* in,float* move){
 const bool noRiptide=MI(0x1f4)<1;
 if(MF(0xd4)<=0.f){float f=1.7999999523162842f;if(MB(0x1a6)==0&&MB(0x1ee)==0&&0.f<MF(0xc0))f=MF(0xc0);MF(0xd4)=f;if(!(0.f<MF(0xd8)))MF(0xd8)=1.4900000095367432f;}
 else if(MF(0xd8)<=0.f)MF(0xd8)=1.4900000095367432f;
 if(MF(0xdc)<=0.f)MF(0xdc)=0.6000000238418579f;
 const uint8_t wasGlide=MB(0x1dc);
 float box[6];MsBox(e,s,box);const bool canFetch=MsBoxSane(box)&&MsLoaded(e,box);
 uint8_t allOk=1;
 MB(0x48)=in[0x30];MB(0x49)=in[0x31];
 memcpy(s+0xc,s+0,12);memcpy(s+0,in+0x18,12);memcpy(s+0x24,s+0x18,12);memcpy(s+0x18,in+0x24,12);memcpy(s+0x3c,s+0x30,12);
 MF(0x30)=MF(0)-MF(0xc);MF(0x34)=MF(4)-MF(0x10);MF(0x38)=MF(8)-MF(0x14);
 if(in[0x32]==1){MB(0x4a)=1;if(MB(0x1ff)==1)MB(0x1fd)=1;}
 else if(in[0x33]==1){if(MB(0x1fd)==1)MB(0x200)=1;MB(0x1fd)=0;MB(0x4a)=0;}
 memcpy(s+0xa0,s+0x94,12);memcpy(s+0x94,in+0xc,4);memcpy(s+0x98,in+0x14,4);memcpy(s+0x9c,in+0x10,4);
 MB(0x1a7)=in[0x3a];MB(0x1a3)=in[0x36];
 MB(0x1a8)=in[0x3e]==0?in[0x3c]:1;
 MB(0x1a9)=in[0x45]?1:in[0x3a];
 const uint8_t keepSpeed=e.cfg[0xd],down=in[0x34],up=in[0x35];
 auto speedReset=[&](uint8_t c){if(c==0){MB(0x158)=0;MF(0x134)=MF(0x138);}};
 if((up&down)==0){
  if(up==0&&down==0&&MB(0x1a5)==0&&MB(0x1a4)!=MB(0x1a2)){MB(0x1a2)=MB(0x1a4);MB(0x1a5)=1;}
  else if(down==0){
   if(up==0)MB(0x1a5)=1;
   else{MB(0x1a2)=0;const uint8_t c=MB(0x158);MB(0x1a5)=1;if(keepSpeed==0)speedReset(c);}
  }else{MB(0x1a2)=1;MB(0x1a5)=1;if(keepSpeed==0){MB(0x158)=0;MF(0x134)=MF(0x138)*1.2999999523162842f;}}
 }else{MB(0x1a2)=0;MB(0x1a5)=1;speedReset(keepSpeed);}
 MF(0x13c)=MsAirSpeed(s);
 MsPose(e,s,in,canFetch,&allOk);
 const uint8_t standPose=in[0x38]?0:(in[0x39]?1:in[0x37]);
 auto crawl=[&]{MB(0x1ee)=1;MF(0xc0)=MF(0xdc);};
 if(in[0x43]==1){
  if(canFetch){const MsRoom r=MsBoxFree(e,s,MF(0xd4));allOk=allOk?r.ok:0;if(!r.ok||!r.empty){MB(0x1a6)=0;crawl();}}
 }else if(in[0x44]==1){
  if(!canFetch)crawl();
  else{const float h=standPose?MF(0xd8):MF(0xd4);const MsRoom r=MsBoxFree(e,s,h);allOk=allOk?r.ok:0;
   if(!r.ok||!r.empty)crawl();else{MB(0x1ee)=0;MB(0x1a6)=standPose;MF(0xc0)=h;}}
 }
 const uint8_t wasSwim=MB(0x1b8);
 MB(0x1c0)=wasSwim?in[0x40]:0;
 bool swimOk;
 if(in[0x40]==1){
  MB(0x1b8)=0;swimOk=false;
  if(canFetch){const MsRoom a=MsBoxFree(e,s,MF(0xd4));
   if(a.ok){swimOk=true;
    if(a.empty)MsSneakOff(s);
    else{const MsRoom b=MsBoxFree(e,s,MF(0xd8));swimOk=b.ok;if(b.ok){MB(0x1a6)=b.empty;MB(0x1ee)=!b.empty;MF(0xc0)=b.empty?MF(0xd8):MF(0xdc);}}}}
 }else{
  swimOk=true;
  if(in[0x3f]==1){MB(0x1b8)=1;
   if(MB(0x1c1)==0&&MQ(0x1c8)<1){if(canFetch){const MsRoom r=MsBoxFree(e,s,MF(0xd4));allOk=allOk?r.ok:0;if(r.ok&&r.empty)MsSneakOff(s);}}
   else MsSneakOff(s);}
 }
 {float f=(wasSwim?0.10000000149011612f:-0.10000000149011612f)+MF(0x1bc);const float lo=0.f<=f?f:0.f;MF(0x1bc)=1.f<f?1.f:lo;}
 MB(0x1d0)=in[0x3d];MB(0x1d1)=in[0x41];MB(0x1d2)=in[0x42];
 if(in[0x46]==0){if(in[0x47]==1)MB(0x1dc)=1;}else MB(0x1dc)=0;
 MsMoveVector(e,move,s,in,e.cfg[0x20]);
 MB(0x1aa)=in[0x3b];MB(0x1ab)=in[0x3c];
 MB(0x1ac)=((in[0x3c]|in[0x3d])&1)?1:in[0x3e];
 MF(0x128)=0.f<MF(0x12c)?MF(0x12c):0.41999998688697815f;
 {int a=0;if(MsEffect(e,0,&a))MF(0x128)=(float)(a+1)*0.10000000149011612f+MF(0x128);}
 if(MB(0x1ab)==0)MU(0x1b0)=0;
 if(MF(0x124)==0.f)MF(0x124)=0.07999999821186066f;
 {int a=0;MB(0x1ed)=MsEffect(e,2,&a);}
 MB(0x1f8)=in[0x54]?1:(MB(0x1f9)==1?MB(0x1f8):0);
 int riptide=MI(0x1f4);
 uint8_t result=1;
 bool skip=false;
 if((0<riptide)&&(in[0x55]&1)){
  if(MB(0x1fa)!=1)skip=true;
  else{MI(0x1f4)=0;MB(0x1fa)=0;MsPrevVel(s);MF(0x64)*=-0.20000000298023224f;MF(0x68)*=-0.20000000298023224f;MF(0x6c)*=-0.20000000298023224f;riptide=0;}
 }
 if(!skip&&((riptide!=0)|MB(0x1dc)|((wasGlide^1)&(uint8_t)noRiptide))==0&&(MB(0x1b8)!=1||(MB(0x1c1)==0&&MQ(0x1c8)<1))){
  result=0;
  if(canFetch){const MsRoom a=MsBoxFree(e,s,MF(0xd4));
   if(a.ok){result=1;
    if(a.empty)MsSneakOff(s);
    else{const MsRoom b=MsBoxFree(e,s,MF(0xd8));result=b.ok;if(b.ok){MB(0x1a6)=b.empty;MB(0x1ee)=!b.empty;MF(0xc0)=b.empty?MF(0xd8):MF(0xdc);}}}}
 }
 MF(0xb4)=move[0]*0.9800000190734863f;MF(0xb8)=move[1]*0.9800000190734863f;
 if((allOk&(uint8_t)canFetch)==0)result=0;
 if(!swimOk)result=0;
 return result!=0;
}

// ---- collision -------------------------------------------------------------------------------------------------
// One shape against the moving box along the move: the move on a single axis is clipped to the gap; when the box
// already overlaps the shape it is pushed out along the axis of least penetration (pen keeps the largest one).
struct MsClipState{float move[3];float pen[3];};
inline float MsMask6(float v){return std::fabs(v)>9.999999974752427e-07f?v:0.f;}
inline void MsClip(const float* S,const float* B,MsClipState& io,bool penOnly){
 if(!MsFinite(S[0])||!MsFinite(S[3])||S[3]<=S[0]||!MsFinite(S[1])||!MsFinite(S[4])||S[4]<=S[1]||!MsFinite(S[2])||!MsFinite(S[5])||S[5]<=S[2])return;
 float pens[3]={0,0,0},signs[3]={0,0,0};
 const float d1=MsMask6(S[3]-B[0]),d2=MsMask6(B[3]-S[0]);
 const float a=d2>0.f?d2:0.f,b=d1>0.f?d1:0.f;
 bool xo;float fx;
 if(a!=0.f){if(b!=0.f){const float p=b<=a?b:a;signs[0]=a<b?-1.f:1.f;pens[0]=p;xo=true;fx=p;}else{xo=false;signs[0]=1.f;fx=d1;}}
 else{xo=false;signs[0]=-1.f;fx=d2;}
 bool ysep=false;float fy;
 {const float e2=MsMask6(B[4]-S[1]);const float c=e2>0.f?e2:0.f;bool sep=false;
  if(c!=0.f){const float e1=MsMask6(S[4]-B[1]);const float d=e1>0.f?e1:0.f;
   if(d==0.f){signs[1]=1.f;fy=e1;sep=true;}else{const float p=d<=c?d:c;pens[1]=p;signs[1]=c<d?-1.f:1.f;fy=p;}}
  else{signs[1]=-1.f;fy=e2;sep=true;}
  if(sep){if(!xo)return;ysep=true;xo=false;}}
 int axis;float sign,gap;
 const float f2=MsMask6(B[5]-S[2]);const float cz=f2>0.f?f2:0.f;
 if(cz!=0.f){
  const float f1=MsMask6(S[5]-B[2]);const float dz=f1>0.f?f1:0.f;
  if(dz==0.f){if(!xo)return;axis=2;sign=1.f;gap=f1;}
  else{
   const float pz=dz<=cz?dz:cz;pens[2]=pz;signs[2]=cz<dz?-1.f:1.f;
   if(!xo){if(ysep){axis=1;sign=signs[1];gap=fy;}else{axis=0;sign=signs[0];gap=fx;}}
   else{
    const float m=pens[1]<pens[0]?pens[1]:pens[0];
    axis=m<=pz?(pens[1]<pens[0]?1:0):2;
    const float pen=pens[axis];io.pen[axis]=pen<=io.pen[axis]?io.pen[axis]:pen;
    if(penOnly)return;
    const float push=pen*signs[axis],mv=io.move[axis];
    const bool take=push<=0.f?push<mv:mv<push;
    io.move[axis]=(!take&&mv!=push)?mv:push;return;
   }
  }
 }else{if(!xo)return;axis=2;sign=-1.f;gap=f2;}
 if(gap-io.move[axis]*sign<=0.f)return;
 io.move[axis]=gap*sign;
}
// All shapes (last to first) against the box for one move.
inline MsClipState MsSweep(const std::vector<float>& sh,const float* box,const float* move,bool penOnly,const float* pen){
 MsClipState io;memcpy(io.move,move,12);memcpy(io.pen,pen,12);
 for(size_t i=sh.size();i>=6;i-=6)MsClip(&sh[i-6],box,io,penOnly);
 return io;
}
// Step-up attempt: up 0.5625, X, Z, back down; returns the box after and the total move.
struct MsStep{float box[6];float move[3];};
inline MsStep MsStepUp(const float* b,const float* v,const std::vector<float>& sh,bool penOnly){
 const float zero[3]={0,0,0};float box[6];memcpy(box,b,24);
 auto pass=[&](const float* mv){MsClipState io;memcpy(io.move,mv,12);memcpy(io.pen,zero,12);
  for(size_t i=sh.size();i>=6;i-=6)if(sh[i-6+1]<b[4]){MsClipState t=io;memcpy(t.pen,zero,12);MsClip(&sh[i-6],box,t,penOnly);memcpy(io.move,t.move,12);}
  return io;};
 const float upIn[3]={0.f,0.5625f,0.f};
 MsClipState up=pass(upIn);
 if(sh.empty()){up.move[0]=0.f;up.move[1]=0.5625f;up.move[2]=0.f;}
 for(int k=0;k<3;++k){box[k]+=up.move[k];box[k+3]+=up.move[k];}
 const float xIn[3]={v[0],0.f,0.f};MsClipState x=pass(xIn);
 for(int k=0;k<3;++k){box[k]+=x.move[k];box[k+3]+=x.move[k];}
 const float zIn[3]={0.f,0.f,v[2]};MsClipState z=pass(zIn);
 for(int k=0;k<3;++k){box[k]+=z.move[k];box[k+3]+=z.move[k];}
 const float dIn[3]={-up.move[0],-up.move[1],-up.move[2]};MsClipState d=pass(dIn);
 for(int k=0;k<3;++k){box[k]+=d.move[k];box[k+3]+=d.move[k];}
 MsStep r;memcpy(r.box,box,24);for(int k=0;k<3;++k)r.move[k]=up.move[k]+x.move[k]+z.move[k]+d.move[k];return r;
}
// Grown block-aligned box (one block around).
inline void MsGrow(float* o,const float* b){
 const float x0=std::floor(b[0]-1.f),y0=std::floor(b[1]-1.f),z0=std::floor(b[2]-1.f),x1=std::ceil(b[3]+1.f)+1.f,y1=std::ceil(b[4]+1.f)+1.f,z1=std::ceil(b[5]+1.f)+1.f;
 o[0]=x1<x0?x1:x0;o[1]=y1<y0?y1:y0;o[2]=z1<z0?z1:z0;o[3]=x0<x1?x1:x0;o[4]=y1>y0?y1:y0;o[5]=z1>z0?z1:z0;
}
inline void MsExpand(float* o,const float* b,const float* v){memcpy(o,b,24);for(int k=0;k<3;++k){if(v[k]<0.f)o[k]+=v[k];else if(v[k]>0.f)o[k+3]+=v[k];}}
// World readable around the box swept by the velocity.
inline bool MsCanMove(MsEnv& e,const uint8_t* s){
 float box[6],ex[6],g[6];MsBox(e,s,box);MsExpand(ex,box,(const float*)(s+0x64));MsGrow(g,ex);return MsBoxSane(g)&&MsLoaded(e,g);
}
// Sneaking edge protection: shrinks the horizontal velocity by 0.05 steps while no ground remains under the box.
inline bool MsSneakEdge(MsEnv& e,uint8_t* s){
 if(MB(0x1a6)!=1||MB(0x1d6)==0||0.f<MF(0x68))return true;
 float b[6];MsBox(e,s,b);
 const float x0=b[0]+0.02500000037252903f,z0=b[2]+0.02500000037252903f,x1=b[3]-0.02500000037252903f,y1=b[4]+0.f,z1=b[5]-0.02500000037252903f,y0=b[1]-0.5681250095367432f;
 float vx=MF(0x64),vz=MF(0x6c);
 {float q[6]={x0,y0,z0,x1,y1,z1};if(vx<0.f)q[0]+=vx;else if(vx>0.f)q[3]+=vx;if(vz<0.f)q[2]+=vz;else if(vz>0.f)q[5]+=vz;
  if(!MsBoxSane(q)||!MsLoaded(e,q))return false;}
 const float yTop=-0.5681250095367432f+y1;
 auto stepOf=[](float v){return v+(0.f<v?-0.05000000074505806f:0.05000000074505806f);};
 auto empty=[&](float ax,float az){const float q[6]={x0+ax,y0,z0+az,x1+ax,yTop,z1+az};MsCollectClean(e,q,e.boxes);return e.boxes.empty();};
 unsigned n=0;
 if(vx!=0.f){unsigned i=0;
  for(;;){if(!empty(vx,0.f)){n=i;break;}if(vx<0.05000000074505806f&&-0.05000000074505806f<=vx){n=i+1;vx=0.f;break;}vx=stepOf(vx);n=i+1;if(vx==0.f||i>0x3e6)break;i=n;}}
 const float fx=n!=1000?vx:0.f;
 n=0;
 if(vz!=0.f){unsigned i=0;
  for(;;){if(!empty(0.f,vz)){n=i;break;}if(vz<0.05000000074505806f&&-0.05000000074505806f<=vz){n=i+1;vz=0.f;break;}vz=stepOf(vz);n=i+1;if(vz==0.f||i>0x3e6)break;i=n;}}
 float fz=n!=1000?vz:0.f;
 float gx=fx;n=0;
 if(fz!=0.f&&gx!=0.f){unsigned i=0;
  for(;;){if(!empty(gx,fz)){n=i;break;}
   gx=(0.05000000074505806f<=gx||gx<-0.05000000074505806f)?stepOf(gx):0.f;
   if(fz<0.05000000074505806f&&-0.05000000074505806f<=fz){n=i+1;fz=0.f;break;}
   fz=stepOf(fz);n=i+1;if(fz==0.f||gx==0.f||i>0x3e6)break;i=n;}}
 MsPrevVel(s);MF(0x64)=n!=1000?gx:0.f;MF(0x6c)=n!=1000?fz:0.f;return true;
}
// Supporting block: the cell overlapping the box (lowered by 0.001) nearest to the feet cell centre.
inline void MsSupportScan(MsEnv& e,uint8_t* s,const float* box){
 MB(0xec)=0;
 const int fx=MsToInt(std::floor(MF(0x4c))),fy=MsToInt(std::floor(MF(0x50))),fz=MsToInt(std::floor(MF(0x54)));
 const int y0=MsToInt(std::floor(box[1])),y1=MsToInt(std::ceil(box[4])),x0=MsToInt(std::floor(box[0])),x1=MsToInt(std::ceil(box[3])),z0=MsToInt(std::floor(box[2])),z1=MsToInt(std::ceil(box[5]));
 if(y0>y1||z0>z1||x0>x1)return;
 const float rx=(float)fx+0.5f,ry=(float)fy+0.5f,rz=(float)fz+0.5f;float best=3.4028234663852886e+38f;
 for(int y=y0;;++y){for(int x=x0;;++x){for(int z=z0;;++z){
  const int32_t cell[3]={x,y,z};MsCellShapes(e,cell,e.local);const float cx=(float)x,cy=(float)y,cz=(float)z;
  for(size_t i=0;i+6<=e.local.size();i+=6){const float* a=&e.local[i];
   if(!(MsFinite(a[0])&&MsFinite(a[3])&&a[0]<a[3]&&MsFinite(a[1])&&MsFinite(a[4])&&a[1]<a[4]&&MsFinite(a[2])&&MsFinite(a[5])&&a[2]<a[5]))continue;
   if(1e-5f<(a[3]+cx)-box[0]&&!((a[4]+cy)-box[1]<=1e-5f)&&!(box[3]-(a[0]+cx)<=1e-5f)&&!(box[4]-(a[1]+cy)<=1e-5f)&&!((a[5]+cz)-box[2]<=1e-5f)&&1e-5f<box[5]-(a[2]+cz)){
    const float dy=cy-ry,d=(cz-rz)*(cz-rz)+dy*dy+(cx-rx)*(cx-rx);
    if(d<best){memcpy(s+0xe0,cell,12);best=d;MB(0xec)=1;}
    break;}}
  if(z==z1)break;}if(x==x1)break;}if(y==y1)break;}
}
inline bool MsSupport(MsEnv& e,uint8_t* s,const float* v){
 if(MB(0x1d6)==0){MB(0xec)=0;return true;}
 float box[6];MsBox(e,s,box);box[1]+=-0.0010000000474974513f;
 if(!MsBoxSane(box)||!MsLoaded(e,box)){MB(0xec)=0;return false;}
 MsSupportScan(e,s,box);if(MB(0xec))return true;
 box[0]-=v[0];box[2]-=v[2];box[3]-=v[0];box[5]-=v[2];
 if(MsBoxSane(box)&&MsLoaded(e,box)){MsSupportScan(e,s,box);return true;}
 return false;
}
inline bool MsMinorCollision(const float* v,const float* d){
 if(std::fabs(v[0])<=std::fabs(v[2])||4.999999873689376e-05f<=std::fabs(d[0])){
  if(std::fabs(v[2])<=std::fabs(v[0]))return false;return std::fabs(d[2])<4.999999873689376e-05f;}
 return true;
}
// Move with collisions (X, Y, Z sweeps, step-up), position / box update, collision flags, ground state.
inline bool MsCollideMove(MsEnv& e,uint8_t* s){
 MsRefreshBox(e,s);
 const bool eye=e.cfg[0xc]!=0;
 float b[6];MsBox(e,s,b);
 const float v[3]={MF(0x64),MF(0x68),MF(0x6c)};
 float ex[6];MsExpand(ex,b,v);
 static thread_local std::vector<float> moveBoxes;moveBoxes.clear();MsCollect(e,ex,moveBoxes);MsCompact(moveBoxes);
 const bool penOnly=MB(0x1d9)!=0;const float zero[3]={0,0,0};
 const float mx[3]={v[0],0.f,0.f};const MsClipState r1=MsSweep(moveBoxes,b,mx,penOnly,zero);
 float b2[6];for(int k=0;k<3;++k){b2[k]=b[k]+r1.move[k];b2[k+3]=b[k+3]+r1.move[k];}
 const float my[3]={0.f,v[1],0.f};const MsClipState r2=MsSweep(moveBoxes,b2,my,penOnly,r1.pen);
 float b4[6];for(int k=0;k<3;++k){b4[k]=b2[k]+r2.move[k];b4[k+3]=b2[k+3]+r2.move[k];}
 const float mz[3]={0.f,0.f,v[2]};const MsClipState r3=MsSweep(moveBoxes,b4,mz,penOnly,r2.pen);
 for(int k=0;k<3;++k){b4[k]+=r3.move[k];b4[k+3]+=r3.move[k];}
 float dz=r1.move[2]+r2.move[2]+r3.move[2],dx=r2.move[0]+r1.move[0]+r3.move[0],dy=r2.move[1]+r1.move[1]+r3.move[1];
 {const float p=r3.pen[2]*r3.pen[2]+r3.pen[1]*r3.pen[1]+r3.pen[0]*r3.pen[0];const bool pushed=9.999999960041972e-12f<=p;MB(0x1d9)=(uint8_t)((pushed?1:0)&MB(0x1d8));MB(0x1d8)=pushed;}
 float cx=(b4[3]+b4[0])*0.5f,cz=(b4[5]+b4[2])*0.5f;
 bool stepped=false;
 bool tryStep;
 if(MB(0x1d6)==0)tryStep=(v[1]!=dy&&v[1]<0.f&&(v[2]!=dz||v[0]!=dx));
 else tryStep=!(v[0]==dx&&v[2]==dz);
 if(tryStep){
  float q[6];MsExpand(q,b,v);q[4]+=0.5625f;
  if(!MsBoxSane(q)||!MsLoaded(e,q))return false;
  const MsStep st=MsStepUp(b,v,moveBoxes,penOnly);
  MsCollectClean(e,st.box,e.boxes);
  bool accept=e.boxes.empty()&&!(st.move[2]*st.move[2]+st.move[0]*st.move[0]<=dz*dz+dx*dx);
  if(accept&&e.cfg[0x14]==0){
   float limit;memcpy(&limit,e.cfg+4,4);
   const float px=MF(0),py=MF(4),pz=MF(8);
   const float d0=std::sqrt((b4[1]-py)*(b4[1]-py)+(cx-px)*(cx-px)+(cz-pz)*(cz-pz));
   const float sx=(st.box[0]+st.box[3])*0.5f-px,sz=(st.box[2]+st.box[5])*0.5f-pz;
   const float d1=std::sqrt((st.box[1]-py)*(st.box[1]-py)+sx*sx+sz*sz);
   if(d0<=limit&&d0<d1)accept=false;
  }
  if(accept){memcpy(b4,st.box,24);stepped=true;dx=st.move[0];dy=st.move[1];dz=st.move[2];
   if(eye){const float f=0.4000000059604645f*MF(0xb0);MF(0xac)=MF(0xac)*0.4000000059604645f;MF(0xb0)=f+st.move[1];}}
  cx=(b4[0]+b4[3])*0.5f;cz=(b4[2]+b4[5])*0.5f;
 }
 float ny=b4[1];
 if(eye){if(stepped)ny=b4[1]-MF(0xb0);else{MF(0xac)=0.f;MF(0xb0)=0.f;}}
 {const float d[3]={cx-MF(0x4c),ny-MF(0x50),cz-MF(0x54)};MB(0x1d7)=MsMinorCollision(v,d);}
 memcpy(s+0x58,s+0x4c,12);MF(0x4c)=cx;MF(0x50)=ny;MF(0x54)=cz;
 memcpy(s+0xf0,b4,24);memcpy(s+0x108,s+0x4c,12);
 MF(0x114)=MF(0xbc)*0.5f*MF(0xc4);MF(0x118)=MsHeight(s)*MF(0xc4);MF(0x11c)=eye?MF(0xb0):0.f;MB(0x120)=1;
 const bool hx=9.999999747378752e-06f<=std::fabs(v[0]-dx),hy=9.999999747378752e-06f<=std::fabs(v[1]-dy);
 MB(0x1d3)=hx;MB(0x1d4)=hy;MB(0x1d5)=9.999999747378752e-06f<=std::fabs(v[2]-dz);
 uint8_t ground;if(hy&&v[1]<0.f)ground=1;else ground=(!hy&&MB(0x1d6)==1)?(uint8_t)(v[1]==0.f):0;
 MB(0x1d6)=ground;
 if(!MsSupport(e,s,v))return false;
 MsPrevVel(s);MF(0x64)=dx;MF(0x68)=dy;MF(0x6c)=dz;return true;
}

// ---- liquids -------------------------------------------------------------------------------------------------------
// Box shrunk by (sx, sy, sx) on every side; an inverted axis collapses to its centre.
inline void MsShrink(float* o,const float* b,float sx,float sy){
 const float sh[3]={sx,sy,sx};
 for(int k=0;k<3;++k){float lo=b[k]+sh[k],hi=b[k+3]-sh[k];if(hi<lo){lo=hi=(b[k]+b[k+3])*0.5f;}o[k]=lo<hi?lo:hi;o[k+3]=lo<hi?hi:lo;}
}
inline bool MsLiquidReach(const float* box,const int32_t* cell,const MsLiquid& l){return (float)cell[1]<box[4]&&box[1]<MsLiquidHeight(l)+(float)cell[1];}
// Liquid cells touched by the shrunk box (water: 0.001 / 0.401, lava: 0.1 / 0.4).
inline void MsLiquidCells(MsEnv& e,const uint8_t* s,bool lava,std::vector<int32_t>& out){
 out.clear();float b[6],in[6];MsBox(e,s,b);
 if(lava)MsShrink(in,b,0.10000000149011612f,0.4000000059604645f);else MsShrink(in,b,0.0010000000474974513f,0.4009999930858612f);
 const int x0=MsToInt(std::floor(in[0])),y0=MsToInt(std::floor(in[1])),z0=MsToInt(std::floor(in[2]));
 const int x1=MsToInt(std::floor(in[3]+1.f)),y1=MsToInt(std::floor(in[4]+1.f)),z1=MsToInt(std::floor(in[5]+1.f));
 if(x0>=x1||z0>=z1||y0>=y1)return;
 for(int x=x0;x<x1;++x)for(int y=y0;y<y1;++y)for(int z=z0;z<z1;++z){const int32_t c[3]={x,y,z};const MsLiquid l=MsLiquidAt(e,c);
  if(l.valid&&l.lava==lava&&MsLiquidReach(in,c,l))out.insert(out.end(),c,c+3);}
}
inline int MsFace(const int32_t* from,const int32_t* to){
 if(to[1]<from[1])return 0;if(to[1]>from[1])return 1;if(to[2]<from[2])return 2;if(to[2]>from[2])return 3;return to[0]<from[0]?4:5;
}
// Face of the block at pos fully covered by its collision shapes.
inline bool MsFaceSturdy(MsEnv& e,const int32_t* pos,int face){
 static const int dx[6]={0,0,0,0,-1,1},dy[6]={-1,1,0,0,0,0},dz[6]={0,0,-1,1,0,0},axisOf[6]={1,1,2,2,0,0},secondOf[6]={-1,-1,-1,-1,2,2};
 MsCellShapes(e,pos,e.local);
 const int axis=axisOf[face],a1=axis+1==3?0:axis+1,a2=secondOf[face]+axis;
 const int dir=axis==0?dx[face]:axis==1?dy[face]:dz[face];
 auto touches=[&](const float* s){return dir<1?s[axis]<=0.f:1.f<=s[axis+3];};
 auto c01=[](float v){return v<0.f?0.f:(1.f<v?1.f:v);};
 e.axisA.assign({0.f,1.f});e.axisB.assign({0.f,1.f});
 for(size_t i=0;i+6<=e.local.size();i+=6){const float* s=&e.local[i];if(!touches(s))continue;
  e.axisA.push_back(c01(s[a1]));e.axisA.push_back(c01(s[a1+3]));e.axisB.push_back(c01(s[a2]));e.axisB.push_back(c01(s[a2+3]));}
 std::sort(e.axisA.begin(),e.axisA.end());std::sort(e.axisB.begin(),e.axisB.end());
 if(e.axisA.size()<2||e.axisB.size()<2)return true;
 for(size_t i=1;i<e.axisA.size();++i){const float p=e.axisA[i-1],q=e.axisA[i];if(p==q)continue;const float ma=(p+q)*0.5f;
  for(size_t j=1;j<e.axisB.size();++j){const float r=e.axisB[j-1],t=e.axisB[j];if(r==t)continue;const float mb=(r+t)*0.5f;bool found=false;
   for(size_t k=0;k+6<=e.local.size();k+=6){const float* s=&e.local[k];
    if(touches(s)&&s[a1]<=ma&&ma<=s[a1+3]&&s[a2]<=mb&&mb<=s[a2+3]){found=true;break;}}
   if(!found)return false;}}
 return true;
}
inline int MsDepth(const MsLiquid& l){return l.falling?0:8-l.height;}
// Liquid flow vector at a cell (normalised; falling liquid beside a wall adds (0, -6, 0) before normalising).
inline void MsFlow(MsEnv& e,const int32_t* cell,const MsLiquid& own,float* out){
 static const int dirs[4][3]={{-1,0,0},{1,0,0},{0,0,-1},{0,0,1}};
 const int self=MsDepth(own);float fx=0,fy=0,fz=0;
 for(auto& d:dirs){const int32_t n[3]={cell[0]+d[0],cell[1]+d[1],cell[2]+d[2]};const MsLiquid nl=MsLiquidAt(e,n);
  if(MsFaceSturdy(e,cell,MsFace(cell,n))||MsFaceSturdy(e,n,MsFace(n,cell)))continue;
  int diff;bool use=false;
  if(nl.valid&&nl.lava==own.lava){diff=MsDepth(nl)-self;use=true;}
  else{const int32_t b[3]={n[0],n[1]-1,n[2]};const MsLiquid bl=MsLiquidAt(e,b);if(bl.valid&&bl.lava==own.lava){diff=MsDepth(bl)-self+8;use=true;}}
  if(use){const float f=(float)diff;fx+=f*(float)d[0];fy+=f*0.f;fz+=f*(float)d[2];}}
 if(own.falling){
  static const int side[4][3]={{-1,0,0},{1,0,0},{0,0,-1},{0,0,1}};bool any=false;
  for(auto& d:side){for(int up=0;up<2&&!any;++up){const int32_t c[3]={cell[0]+d[0],cell[1]+up,cell[2]+d[2]};MsCellShapes(e,c,e.local);if(!e.local.empty())any=true;}if(any)break;}
  if(any){const float l=std::sqrt(fz*fz+fy*fy+fx*fx),inv=1.f/l;if(9.999999747378752e-05f<l){fz*=inv;fx*=inv;fy*=inv;}fx+=-0.f;fy+=-6.f;}
 }
 const float l=std::sqrt(fx*fx+fy*fy+fz*fz),inv=1.f/l;
 if(9.999999747378752e-05f<l){out[0]=fx*inv;out[1]=fy*inv;out[2]=fz*inv;}else{out[0]=out[1]=out[2]=0.f;}
}
// Liquid current pushing the player (water 0.014, lava 0.0035 per normalised flow).
inline bool MsLiquidPush(MsEnv& e,uint8_t* s,const std::vector<int32_t>& cells,bool lava){
 if(cells.empty())return true;
 float b[6];MsBox(e,s,b);const float g[6]={b[0]-1.f,b[1]-1.f,b[2]-1.f,b[3]+1.f,b[4]+1.f,b[5]+1.f};
 if(!MsBoxSane(g)||!MsLoaded(e,g))return false;
 float sx=0,sy=0,sz=0;
 for(size_t i=0;i+3<=cells.size();i+=3){const MsLiquid l=MsLiquidAt(e,&cells[i]);if(l.valid&&l.lava==lava){float f[3];MsFlow(e,&cells[i],l,f);sx+=f[0];sy+=f[1];sz+=f[2];}}
 const float len=std::sqrt(sx*sx+sy*sy+sz*sz);
 if(9.999999747378752e-05f<=len){const float k=(lava?0.0035000001080334187f:0.014000000432133675f)/len;MsPrevVel(s);MF(0x64)+=sx*k;MF(0x68)+=sy*k;MF(0x6c)=sz*k+MF(0x6c);}
 return true;
}
// Liquid at eye height: valid (world readable), water, lava.
struct MsEyeLiquid{bool water=false,lava=false,valid=false;};
inline MsEyeLiquid MsEyeIn(MsEnv& e,const uint8_t* s){
 MsEyeLiquid r;if(!MsSane(s))return r;
 float f=0.4000000059604645f;if(!MsSwim(s)&&MB(0x1ee)==0&&MB(0x1dc)==0&&MI(0x1f4)<1){f=1.6200000047683716f;if(MB(0x1a6)==1)f=1.2699999809265137f;}
 const float scale=0.f<MF(0xc4)?MF(0xc4):1.f,eyeY=scale*f+MF(0x50);
 const int32_t c[3]={MsToInt(std::floor(MF(0x4c)+0.f)),MsToInt(std::floor(eyeY)),MsToInt(std::floor(MF(0x54)+0.f))};
 const float b[6]={(float)c[0]-0.f,(float)c[1]-0.f,(float)c[2]-0.f,(float)c[0]+1.f,(float)c[1]+1.f,(float)c[2]+1.f};
 if(!MsBoxSane(b)||!MsLoaded(e,b))return r;r.valid=true;
 const MsLiquid l=MsLiquidAt(e,c);if(l.valid&&eyeY<MsLiquidHeight(l)+(float)c[1]){r.water=!l.lava;r.lava=l.lava;}
 return r;
}
// Riptide impulse.
inline void MsRiptideImpulse(const uint8_t* s,int level,bool inWater,bool eyeWater,float* o){
 const float pitch=MF(0x94)*3.1415927410125732f/180.f,yaw=MF(0x9c)*3.1415927410125732f/180.f;
 const float sp=pitch*10430.3779296875f,sy=yaw*10430.3779296875f;
 const float sinYaw=(float)MsTableSinScaled(sy),cosPitch=(float)MsTableSinScaled(sp+16384.f),sinPitch=(float)MsTableSinScaled(sp),cosYaw=(float)MsTableSinScaled(sy+16384.f);
 float x=-sinYaw*cosPitch,y=-sinPitch,z=cosPitch*cosYaw;
 const float l2=z*z+sinPitch*sinPitch+x*x;
 if(0.f<l2){const float k=((float)(level+1)*0.75f)/std::sqrt(l2);x*=k;y*=k;z*=k;}
 if(MB(0x1d6)==1&&MB(0x1ec)!=0){float f=MF(0x124)+y;float g=f;if(inWater)g=(y/0.800000011920929f)*0.9800000190734863f;if(!eyeWater)f=g;y=f;}
 o[0]=x;o[1]=y;o[2]=z;
}
// Riptide launch: low byte = launched, high byte = continue with the normal travel.
inline uint16_t MsRiptide(MsEnv& e,uint8_t* s,bool inWater){
 if(!(MB(0x1fc)==0&&MI(0x1f4)<1&&MB(0x1f9)!=0&&((MB(0x1fb)|(uint8_t)inWater)&1)))return 0x100;
 const int lvl=MsArmorEnchant(e,3);if(lvl<1||MB(0x1f8)==0)return 0x100;
 bool eyeWater=false;
 if(MB(0x1d6)==1&&inWater&&MB(0x1ec)!=0){const MsEyeLiquid r=MsEyeIn(e,s);eyeWater=r.water;if(!r.valid)return 0;}
 float v[3];MsRiptideImpulse(s,lvl,inWater,eyeWater,v);
 MsPrevVel(s);MF(0x64)+=v[0];MF(0x68)+=v[1];MF(0x6c)+=v[2];MI(0x1f4)=20;MB(0x1fa)=0;MB(0x1f8)=0;return 0x101;
}
inline bool MsOverride(uint8_t* s){
 if(MB(0x170)==0){if(MQ(0x168)!=0)return false;if(MF(0x15c)==0.f&&MF(0x160)==0.f&&MF(0x164)==0.f)return false;}
 MsPrevVel(s);memcpy(s+0x64,s+0x15c,12);return true;
}
// Input movement relative to the yaw.
inline void MsMoveRelative(uint8_t* s,float speed){
 const float ix=MF(0xb4),iz=MF(0xb8);float l2=iz*iz+ix*ix;
 if(9.999999747378752e-05f<=l2){const float a=MF(0x9c)*0.01745329238474369f,sn=std::sin(a),cs=std::cos(a);
  l2=std::sqrt(l2);const float d=1.f<=l2?l2:1.f;const float k=speed/d;
  const float vx=MF(0x64),vy=MF(0x68),vz=MF(0x6c);MsPrevVel(s);
  MF(0x64)=(ix*k*cs-sn*MF(0xb8)*k)+vx;MF(0x68)=vy;MF(0x6c)=iz*k*cs+ix*k*sn+vz;}
}
inline bool MsBlockSlow(uint8_t* s){
 const float mx=MF(0xc8),my=MF(0xcc),mz=MF(0xd0);
 if(1.0000000116860974e-07f<mz*mz+my*my+mx*mx){
  if(MB(0x1db)==0){MsPrevVel(s);MF(0x64)*=mx;MF(0x68)*=my;MF(0x6c)*=mz;}
  MF(0xc8)=MF(0xcc)=MF(0xd0)=0.f;return (MB(0x1db)^1)!=0;}
 return false;
}
// Vertical swimming toward the look direction.
inline void MsSwimPitch(MsEnv& e,uint8_t* s){
 if(!(MB(0x1b8)==1&&MB(0x1ac)==0))return;
 const float sp=MsTableSin((MF(0x94)*3.1415927410125732f)/180.f);
 const bool steep=0.20000000298023224f<sp;const float target=-sp;
 if(sp<target&&MB(0x1d2)==0&&MB(0x1a9)==0){
  const int32_t c[3]={MsToInt(std::floor(MF(0x4c)+0.f)),MsToInt(std::floor(MF(0x50)+0.5199999809265137f)),MsToInt(std::floor(MF(0x54)+0.f))};
  if(!MsLiquidAt(e,c).valid&&MsPropsAt(e,c).air==1){
   const int32_t c2[3]={c[0],MsToInt(std::floor(MF(0x50)+0.41999995708465576f)),c[2]};
   if(!MsLiquidAt(e,c2).valid){MsPrevVel(s);MF(0x68)=0.f;return;}}
 }
 const float k=steep?0.08500000089406967f:0.05999999865889549f;MsPrevVel(s);MF(0x68)=(target-MF(0x68))*k+MF(0x68);
}
inline bool MsAnyLiquid(MsEnv& e,const float* b){
 const int x0=MsToInt(std::floor(b[0])),x1=MsToInt(std::ceil(b[3])),y0=MsToInt(std::floor(b[1])),y1=MsToInt(std::ceil(b[4])),z0=MsToInt(std::floor(b[2])),z1=MsToInt(std::ceil(b[5]));
 if(!(x0<x1&&y0<y1&&z0<z1))return false;
 for(int x=x0;x<x1;++x)for(int y=y0;y<y1;++y)for(int z=z0;z<z1;++z){const int32_t c[3]={x,y,z};const MsLiquid l=MsLiquidAt(e,c);if(l.valid&&MsLiquidReach(b,c,l))return true;}
 return false;
}
// Block slowdown multipliers (cobweb-like 0.9 / 1.5 kind 2, sweet berry 0.8 / 0.75 kind 1).
inline void MsSlowBy(uint8_t* s,uint8_t kind){
 float x,y,z;
 if(kind==2){x=0.8999999761581421f;y=1.5f;z=0.8999999761581421f;}else if(kind==1){x=0.800000011920929f;y=0.75f;z=0.800000011920929f;}else return;
 const float mx=MF(0xc8),my=MF(0xcc),mz=MF(0xd0);
 if(mz*mz+my*my+mx*mx<=1.0000000116860974e-07f){MF(0xc8)=x;MF(0xcc)=y;MF(0xd0)=z;return;}
 MF(0xc8)=x<mx?x:mx;MF(0xcc)=y<my?y:my;MF(0xd0)=mz<=z?mz:z;
}
inline bool MsCellOverlap(const float* b,int x,int y,int z){
 return 1e-5f<((float)x+1.f)-b[0]&&1e-5f<b[3]-(float)x&&1e-5f<((float)y+1.f)-b[1]&&1e-5f<b[4]-(float)y&&1e-5f<((float)z+1.f)-b[2]&&1e-5f<b[5]-(float)z;
}
inline void MsCellRange(const float* b,int* r){r[0]=MsToInt(std::floor(b[0]));r[1]=MsToInt(std::ceil(b[3]));r[2]=MsToInt(std::floor(b[1]));r[3]=MsToInt(std::ceil(b[4]));r[4]=MsToInt(std::floor(b[2]));r[5]=MsToInt(std::ceil(b[5]));}
inline void MsInsideBlocks(MsEnv& e,uint8_t* s,const float* b){
 int r[6];MsCellRange(b,r);if(!(r[0]<r[1]&&r[4]<r[5]&&r[2]<r[3]))return;
 for(int x=r[0];x!=r[1];++x)for(int y=r[2];y!=r[3];++y)for(int z=r[4];z!=r[5];++z){
  if(!MsCellOverlap(b,x,y,z))continue;const int32_t c[3]={x,y,z};const MsProps p=MsPropsAt(e,c);if(!(p.air&1))MsSlowBy(s,p.b9);}
}
inline bool MsHoneySlide(const uint8_t* s,const int32_t* c){
 if(MF(0x68)<0.f&&MF(0x50)<=(float)c[1]+0.9375f){
  const float hw=MF(0xbc)*MF(0xc4)*0.5f+0.4312500059604645f;
  if(std::fabs(((float)c[0]+0.5f)-MF(0x4c))<=hw)return hw<std::fabs(((float)c[2]+0.5f)-MF(0x54));return true;}
 return false;
}
inline void MsHoney(MsEnv& e,uint8_t* s,const float* b){
 int r[6];MsCellRange(b,r);if(!(r[0]<r[1]&&r[4]<r[5]&&r[2]<r[3]))return;
 for(int x=r[0];x!=r[1];++x)for(int y=r[2];y!=r[3];++y)for(int z=r[4];z!=r[5];++z){
  if(!MsCellOverlap(b,x,y,z))continue;const int32_t c[3]={x,y,z};if(MsPropsAt(e,c).honey!=1)continue;
  const float vy=MF(0x68)<=-0.11999999731779099f?-0.11999999731779099f:MF(0x68);
  MsPrevVel(s);MF(0x64)*=0.4000000059604645f;MF(0x68)=vy;MF(0x6c)*=0.4000000059604645f;if(MsHoneySlide(s,c))MF(0x130)=0.f;}
}
inline void MsAfterMove(MsEnv& e,uint8_t* s){
 float b[6];MsBox(e,s,b);MsInsideBlocks(e,s,b);
 MsBox(e,s,b);const float h[6]={b[0]-0.0010000000474974513f,b[1]-0.f,b[2]-0.0010000000474974513f,b[3]+0.0010000000474974513f,b[4]+0.f,b[5]+0.0010000000474974513f};MsHoney(e,s,h);
}
inline float MsLiquidGravity(uint8_t swim,bool notLava){const float u=swim?0.f:0.004999999888241291f;return notLava?u:0.019999999552965164f;}
// Bounce (bed 0.75 * -vy, slime -vy) and horizontal collision stop.
inline void MsCollideVel(uint8_t* s,const float* saved,uint8_t wasGround,const MsProps& blk){
 if(((wasGround^1)&MB(0x1d4))==1){
  const float f=saved[1];float v=0.f;
  if(f<0.f&&MB(0x1a7)==0){if(blk.bounce==2)v=f*-0.75f;else if(blk.bounce==1&&9.999999747378752e-05f<=std::fabs(f))v=-f;}
  MF(0x68)=v;
 }else if(MB(0x1d4)==1)MF(0x68)=0.f;
 const float vx=MB(0x1d3)?0.f:MF(0x64),vz=MB(0x1d5)?0.f:MF(0x6c);
 MsPrevVel(s);MF(0x64)=vx;MF(0x6c)=vz;
}
inline void MsFallUpdate(uint8_t* s,float prevY){
 const float d=MF(0x50)-prevY;
 if(0.f<=d||(MB(0x1d6)&1)){if(0.f<d)MF(0x130)=0.f;if(MB(0x1d6)==1&&0.f<MF(0x130))MF(0x130)=0.f;return;}
 MF(0x130)=MF(0x130)-d;
}
inline void MsSanitize(MsProps& p){if(!MsPositive(p.friction))p.friction=0.6000000238418579f;if(!MsPositive(p.factor))p.factor=1.f;}
// Block under the feet used for bounces: supporting block, else 0.2 below, else a fence one block lower.
inline MsProps MsUnderFeet(MsEnv& e,const uint8_t* s){
 if(MB(0xec)==1){MsProps p=MsPropsAt(e,(const int32_t*)(s+0xe0));MsSanitize(p);return p;}
 const int32_t c[3]={MsToInt(std::floor(MF(0x4c))),MsToInt(std::floor(MF(0x50)-0.20000000298023224f)),MsToInt(std::floor(MF(0x54)))};
 MsProps p=MsPropsAt(e,c);MsSanitize(p);if(p.air==0)return p;
 const int32_t c2[3]={MsToInt(std::floor(MF(0x4c))),MsToInt(std::floor(MF(0x50)))-1,MsToInt(std::floor(MF(0x54)))};
 MsProps q=MsPropsAt(e,c2);MsSanitize(q);return q.fence?q:p;
}
inline void MsGroundSlow(uint8_t* s,const MsProps& blk){
 if(MB(0x1d6)==1&&MB(0x1a6)==0){float vx=MF(0x64),vz=MF(0x6c);const float ay=std::fabs(MF(0x68));
  if((blk.bounce==1||blk.honey!=0)&&ay<0.10000000149011612f&&MB(0x1a7)==0){const float m=ay*0.20000000298023224f+0.4000000059604645f;vx*=m;vz*=m;}
  MsPrevVel(s);MF(0x64)=vx;MF(0x6c)=vz;}
}
// Scaffolding (kind 1) / powder snow (kind 2) vertical control. Returns true when descending scaffolding.
inline bool MsClimbKind(uint8_t* s,uint8_t kind,bool boots){
 float y=MF(0x68);bool ret=false;
 if(kind==2){if(MB(0x1a9)!=0)y=-0.15000000596046448f;else if(boots&&MB(0x1a8)!=0)y=0.20000000298023224f;}
 else if(kind==1){if(MB(0x1a9)!=0){ret=true;y=-0.15000000596046448f;}else if(MB(0x1a8)==1)y=0.15000000596046448f;}
 MsPrevVel(s);MF(0x68)=y;return ret;
}
inline bool MsInWeb(MsEnv& e,const uint8_t* s){
 float b[6];MsBox(e,s,b);
 const int x0=MsToInt(std::floor(b[0]-1.f)),y0=MsToInt(std::floor(b[1]-1.f)),z0=MsToInt(std::floor(b[2]-1.f)),x1=MsToInt(std::ceil(b[3]+1.f)),y1=MsToInt(std::ceil(b[4]+1.f)),z1=MsToInt(std::ceil(b[5]+1.f));
 if(y0>y1||z1<z0||x1<x0)return false;
 for(int y=y0;y<=y1;++y)for(int x=x0;x<=x1;++x)for(int z=z0;z<=z1;++z){const int32_t c[3]={x,y,z};const MsProps p=MsPropsAt(e,c);
  if(!(p.air&1)&&MsCellOverlap(b,x,y,z)&&(p.web&1))return true;}
 return false;
}
inline void MsWebSlow(MsEnv& e,uint8_t* s){
 int a=0;const bool weaving=MsEffect(e,3,&a);const float mx=weaving?0.5f:0.25f,my=weaving?0.25f:0.05000000074505806f;
 MsPrevVel(s);MF(0x64)*=mx;MF(0x68)*=my;MF(0x6c)=mx*MF(0x6c);
}
inline void MsRiptideStop(MsEnv& e,uint8_t* s,bool* fail){
 *fail=false;
 if(0<MI(0x1f4)&&(MB(0x1d3)!=0||MB(0x1d5)!=0)){MI(0x1f4)=0;MB(0x1fa)=0;
  if(MB(0x1dc)==0&&!MsSwim(s)&&!MsTryStand(e,s))*fail=true;}
}

// ---- travel ----------------------------------------------------------------------------------------------------------
inline void MsSaveLast(uint8_t* s){memcpy(s+0x88,s+0x7c,12);memcpy(s+0x7c,s+0x64,12);}
// Liquid travel (water: swim speed, depth strider, 0.8 / 0.9 drag; lava: 0.5 drag) with the jump-out-of-liquid step.
inline bool MsLiquidTravel(MsEnv& e,uint8_t* s,bool lava){
 const float startY=MF(0x50);const uint8_t ascend=MB(0x1ac);
 if(!lava){if(MB(0x1d1)||MB(0x1d2)||MB(0x1a9)){MsPrevVel(s);MF(0x68)+=-0.03999999910593033f;}MsSwimPitch(e,s);}
 if(ascend){
  uint8_t blocked=0;
  if(!lava&&MB(0x1b8)){const MsEyeLiquid r=MsEyeIn(e,s);if(!r.valid)return false;blocked=r.water^1;}
  float v=0.f;if((MF(0x1bc)<=0.f||1.f<=MF(0x1bc))&&blocked==0)v=MF(0x68)+0.03999999910593033f;
  MsPrevVel(s);MF(0x68)=v;
 }
 float speed,ds=0.f;bool weak;
 if(lava){speed=MF(0x144)!=0.f?MF(0x144):0.019999999552965164f;weak=true;}
 else{
  const float base=MF(0x140)!=0.f?MF(0x140):0.019999999552965164f;
  float mult=1.f;if(MB(0x1b8)==1&&MF(0x148)!=0.f)mult=MF(0x148);
  int strider=MsArmorEnchant(e,0);if(strider<0)strider=0;ds=(float)strider;if(3.f<=ds)ds=3.f;
  if(1.f<mult)speed=base*((ds/3.f*0.30000001192092896f+0.699999988079071f)*mult);
  else{if(MB(0x1d6)==0)ds*=0.5f;speed=(MF(0x134)-base)*(ds/3.f)+base;}
  weak=mult<=1.f;
 }
 MsMoveRelative(s,speed);
 const bool slowed=MsBlockSlow(s);
 if(!MsCanMove(e,s))return false;
 float saved[3]={MF(0x64),MF(0x68),MF(0x6c)};const uint8_t ground=MB(0x1d6);
 if(!MsCollideMove(e,s))return false;
 bool fail;MsRiptideStop(e,s,&fail);if(fail)return false;
 MsProps air;air.air=1;
 if(!slowed){MsCollideVel(s,saved,ground,air);MsSaveLast(s);}
 else{MsSaveLast(s);MsPrevVel(s);MF(0x64)=MF(0x68)=MF(0x6c)=0.f;saved[0]=saved[1]=saved[2]=0.f;MsCollideVel(s,saved,ground,air);}
 const float vx=MF(0x64),vz=MF(0x6c);
 float hDrag=0.5f,vDrag=0.5f;
 if(!lava){
  const float d=((MB(0x1c0)|MB(0x1a2))&1)?0.8999999761581421f:0.800000011920929f;
  float extra=weak?(ds/3.f)*(0.546000063419342f-d):-0.f;hDrag=d+(0.f<ds?extra:-0.f);vDrag=0.800000011920929f;
 }
 float vy=MF(0x68)*vDrag;
 {int a=0;if(MsEffect(e,1,&a))vy=vy+((float)(a+1)*0.05000000074505806f-vy)*0.20000000298023224f;
  else if(MB(0x1ec)==1)vy=vy-MsLiquidGravity(MB(0x1b8),!lava);}
 const float nx=vx*hDrag,nz=vz*hDrag;
 if(MB(0x1d3)||MB(0x1d5)){
  const float up=(startY+0.6000000238418579f+vy)-MF(0x50);
  float b[6];MsBox(e,s,b);const float q[6]={b[0]+nx,b[1]+up,b[2]+nz,b[3]+nx,b[4]+up,b[5]+nz};
  if(!MsBoxSane(q)||!MsLoaded(e,q))return false;
  MsCollectClean(e,q,e.boxes);const bool liquid=MsAnyLiquid(e,q);
  const float f=liquid?vy:0.30000001192092896f;if(e.boxes.empty())vy=f;
 }
 MsPrevVel(s);MF(0x64)=nx;MF(0x68)=vy;MF(0x6c)=nz;
 MsAfterMove(e,s);MF(0x130)=0.f;return true;
}
// Elytra glide velocity with firework rockets.
inline void MsGlidePhysics(uint8_t* s){
 if(-0.5f<MF(0x68))MF(0x130)=1.f;
 const float a=(-3.1415927410125732f-MF(0x9c)*0.01745329238474369f)*10430.3779296875f;
 const double c5=MsTableSinScaled(a+16384.f),c6=MsTableSinScaled(a);
 const float pr=0.01745329238474369f*MF(0x94),ps=10430.3779296875f*pr;
 const double cp=MsTableSinScaled(ps+16384.f),sp=MsTableSinScaled(ps);
 const float cosP=(float)cp,sinP=(float)sp;
 float vx=MF(0x64),vz=MF(0x6c);const float horiz2=vx*vx+vz*vz;
 const float g=MB(0x1ed)?0.009999999776482582f:MF(0x124);
 const float f12=(-cosP)*(float)c6,f13=(-cosP)*(float)c5;
 float vy=(0.75f*g*cosP*cosP-g)+MF(0x68);
 if(0.00010000000111176631<cp&&vy<0.f){const float f=cosP*cosP*-0.10000000149011612f*vy;vy+=f;vx+=f*f12/cosP;vz+=f*f13/cosP;}
 const float horiz=std::sqrt(horiz2);
 if(pr<0.f&&0.00010000000111176631<cp){const float f=-sinP*horiz*0.03999999910593033f;vy+=3.200000047683716f*f;vx-=f*f12/cosP;vz-=f*f13/cosP;}
 if(0.00010000000111176631<cp){vx+=(horiz*(f12/cosP)-vx)*0.10000000149011612f;vz+=(horiz*(f13/cosP)-vz)*0.10000000149011612f;}
 const int rockets=MI(0x1e8);
 if(0<rockets||0<MQ(0x1e0)){
  const float ax=0.10000000149011612f*f12,ay=0.10000000149011612f*sinP,az=0.10000000149011612f*f13,tx=f12*1.5f,ty=sinP*-1.5f,tz=f13*1.5f;
  const int n=rockets>1?rockets:1;
  for(int i=0;i<n;++i){vx=(tx-vx)*0.5f+ax+vx;vy=((ty-vy)*0.5f-ay)+vy;vz=(tz-vz)*0.5f+az+vz;}
 }
 MsPrevVel(s);MF(0x64)=vx*0.9900000095367432f;MF(0x68)=vy*0.9800000190734863f;MF(0x6c)=vz*0.9900000095367432f;
}
inline bool MsGlideTravel(MsEnv& e,uint8_t* s){
 MB(0x1d6)=0;MsGlidePhysics(s);
 const bool web=MsInWeb(e,s);if(web)MsWebSlow(e,s);
 const bool slowed=MsBlockSlow(s);
 if(!MsCanMove(e,s))return false;
 const float prevY=MF(0x50);
 if(!MsCollideMove(e,s))return false;
 bool fail;MsRiptideStop(e,s,&fail);if(fail)return false;
 MsFallUpdate(s,prevY);MsSaveLast(s);
 if(web||slowed){MsPrevVel(s);MF(0x64)=MF(0x68)=MF(0x6c)=0.f;}
 MsAfterMove(e,s);return true;
}
// Jump from the ground (jump power +0x128, honey 0.6, sprint boost 0.2 along the yaw), 10 ticks delay.
inline bool MsJump(MsEnv& e,uint8_t* s){
 if(!(MB(0x1aa)==1&&MB(0x1d6)!=0)||MU(0x1b0)!=0)return false;
 float power=MF(0x128);
 const int32_t c1[3]={MsToInt(std::floor(MF(0x4c))),MsToInt(std::floor(MF(0x50))),MsToInt(std::floor(MF(0x54)))};
 const int32_t c2[3]={MsToInt(std::floor(MF(0x4c))),MsToInt(std::floor(MF(0x50)-0.10000000149011612f)),MsToInt(std::floor(MF(0x54)))};
 if(MsPropsAt(e,c1).honey|MsPropsAt(e,c2).honey)power*=0.6000000238418579f;
 float v[3]={MF(0x64),MF(0x68),MF(0x6c)};if(v[1]<=power)v[1]=power;
 if(MB(0x1a2)){const float a=MF(0x9c)*0.01745329238474369f*10430.3779296875f;
  v[0]=v[0]-(float)MsTableSinScaled(a)*0.20000000298023224f;v[2]=(float)MsTableSinScaled(a+16384.f)*0.20000000298023224f+v[2];}
 MsPrevVel(s);memcpy(s+0x64,v,12);MU(0x1b0)=10;return true;
}
inline float MsGroundSpeed(float speed,float friction,float factor){const float f=0.546000063419342f/(friction*factor*0.9100000262260437f);return speed*f*f*f;}
// One tick of travel: riptide, liquids, gliding, ground / air movement with collisions.
inline bool MsTravel(MsEnv& e,uint8_t* s,bool inWater){
 bool liquidPath=false,lavaOnly=false;
 bool check=MB(0x1fd)!=0;
 if(!check){const uint16_t r=MsRiptide(e,s,inWater);check=(r>>8)==1;}
 if(check){
  bool b17=true;if(!inWater)b17=(0<MQ(0x1c8))&&(MB(0x1b8)&1)&&e.cellsB.empty();
  if(((!e.cellsB.empty())||b17)&&MB(0x1fd)==0){liquidPath=true;lavaOnly=!b17;}
 }
 if(liquidPath){
  MsOverride(s);
  if(lavaOnly){if(MsLiquidPush(e,s,e.cellsB,true))return MsLiquidTravel(e,s,true);return false;}
  if(MB(0x1dc)==1){MB(0x1dc)=0;if(!MsSwim(s)&&MI(0x1f4)<1&&!MsTryStand(e,s))return false;MQ(0x1e0)=0;}
  return MsLiquidPush(e,s,e.cellsA,false)?MsLiquidTravel(e,s,false):false;
 }
 const int32_t below[3]={MsToInt(std::floor(MF(0x4c))),MsToInt(std::floor(MF(0x50)-0.5f)),MsToInt(std::floor(MF(0x54)))};
 const MsProps bp=MsPropsAt(e,below);
 const float friction=MsPositive(bp.friction)?bp.friction:0.6000000238418579f;float factor=MsPositive(bp.factor)?bp.factor:1.f;
 float drag,speed;
 if(MB(0x1d6)==1){if(bp.soul==1&&MsArmorEnchant(e,1)>0)factor=1.f;drag=0.9100000262260437f*friction;speed=MsGroundSpeed(MF(0x134),friction,factor);}
 else{speed=MF(0x13c);drag=0.9100000262260437f;}
 if(MB(0x1dc)==1){int a=0;if(MsEffect(e,1,&a)){MB(0x1dc)=0;if(!MsSwim(s)&&MI(0x1f4)<1&&!MsTryStand(e,s))return false;}}
 if(MB(0x1dc)==1){
  if(MsArmorIs(e,1,"elytra")&&MB(0x1d6)==0)return MsGlideTravel(e,s);
  MB(0x1dc)=0;if(!MsSwim(s)&&MI(0x1f4)<1&&!MsTryStand(e,s))return false;
 }
 MsOverride(s);MsMoveRelative(s,speed);MsJump(e,s);
 const int32_t feet[3]={MsToInt(std::floor(MF(0x4c))),MsToInt(std::floor(MF(0x50))),MsToInt(std::floor(MF(0x54)))};
 MsProps fp=MsPropsAt(e,feet);uint8_t kind=fp.b10;
 const bool boots=MsArmorIs(e,3,"leather_boots");
 if(kind==0&&MB(0xec)!=0){fp=MsPropsAt(e,(const int32_t*)(s+0xe0));kind=fp.b10==1;}   // the supporting block's record replaces the feet one
 const bool scaffoldDown=MsClimbKind(s,kind,boots);
 if(fp.climb==1){
  float y=0.20000000298023224f;
  if(MB(0x1ac)==0&&MB(0x1d3)==0&&MB(0x1d5)==0){y=-0.20000000298023224f<=MF(0x68)?MF(0x68):-0.20000000298023224f;if(MB(0x1a6)==1&&y<0.f)y=0.f;}
  MsPrevVel(s);MF(0x68)=y;
 }
 const bool web=MsInWeb(e,s);if(web)MsWebSlow(e,s);
 const bool slowed=MsBlockSlow(s);
 if(!MsCanMove(e,s)||!MsSneakEdge(e,s))return false;
 float saved[3]={MF(0x64),MF(0x68),MF(0x6c)};const uint8_t ground=MB(0x1d6);const float prevY=MF(0x50);
 if(!MsCollideMove(e,s))return false;
 bool fail;MsRiptideStop(e,s,&fail);if(fail)return false;
 MsFallUpdate(s,prevY);
 if((fp.climb==1)|scaffoldDown)MF(0x130)=0.f;
 const MsProps blk=MsUnderFeet(e,s);
 if(prevY==MF(0x50))MsGroundSlow(s,blk);
 MsSaveLast(s);
 if(slowed){MsPrevVel(s);MF(0x64)=MF(0x68)=MF(0x6c)=0.f;saved[0]=saved[1]=saved[2]=0.f;}
 MsCollideVel(s,saved,ground,blk);
 if(web){MsPrevVel(s);MF(0x64)=MF(0x68)=MF(0x6c)=0.f;}
 float vx=MF(0x64),vy=MF(0x68),vz=MF(0x6c);
 if(!scaffoldDown){int a=0;
  if(MsEffect(e,1,&a))vy=((float)(a+1)*0.05000000074505806f-vy)*0.20000000298023224f+vy;
  else if(MB(0x1ec)==1){const float g=(MB(0x1ed)!=0&&vy<0.f)?0.009999999776482582f:MF(0x124);vy=(vy-g)*0.9800000190734863f;}}
 vx*=drag;vz*=drag;
 MsPrevVel(s);MF(0x64)=vx;MF(0x68)=vy;MF(0x6c)=vz;
 MsAfterMove(e,s);return true;
}
// Liquids around, swimming pose, then travel.
inline bool MsMoveBody(MsEnv& e,uint8_t* s){
 {const bool zz=std::fabs(MF(0x6c))<9.99999993922529e-09f,kx=9.99999993922529e-09f<=std::fabs(MF(0x64)),ky=9.99999993922529e-09f<=std::fabs(MF(0x68));
  MsPrevVel(s);if(!kx)MF(0x64)=0.f;if(!ky)MF(0x68)=0.f;if(zz)MF(0x6c)=0.f;}
 const bool swim=MsSwim(s);
 int64_t delay;memcpy(&delay,e.cfg+0x18,8);const int64_t d=delay!=0?delay:10;const int64_t lim=-1<delay?d:0;
 if(lim<MQ(0x1c8))MQ(0x1c8)=lim;
 MsLiquidCells(e,s,false,e.cellsA);MsLiquidCells(e,s,true,e.cellsB);
 const bool water=!e.cellsA.empty();MB(0x1c1)=water;
 uint8_t swimFlag=MB(0x1b8);
 if(swim&&(swimFlag==0||(!water&&MQ(0x1c8)<1))&&MB(0x1dc)==0&&MI(0x1f4)==0){if(!MsTryStand(e,s))return false;swimFlag=MB(0x1b8);}
 if(water&&swimFlag){MQ(0x1c8)=lim;MsSneakOff(s);}
 bool r=MsTravel(e,s,water);
 const bool swim2=MsSwim(s);bool delayLeft;
 if(!water){if(MQ(0x1c8)<1)delayLeft=false;else{MQ(0x1c8)=MQ(0x1c8)-1;delayLeft=0<MQ(0x1c8);}}
 else{MQ(0x1c8)=lim;delayLeft=0<lim;}
 if(swim2&&r&&(MB(0x1b8)==0||(MB(0x1c1)==0&&!delayLeft))&&MB(0x1dc)==0&&MI(0x1f4)==0)return MsTryStand(e,s);
 return r;
}
inline void MsAdvanceInput(uint8_t* s){if(MQ(0x198)!=-1){MQ(0x190)=MQ(0x198)+1;return;}MQ(0x198)=0;MQ(0x190)=1;}
// Server-driven position (teleport / interpolation targets at +0x174 / +0x184); idle in the simulation.
inline bool MsInterp(MsEnv& e,uint8_t* s){
 int pending=MI(0x180);bool flag;
 if(pending<1){flag=true;
  if(MB(0x1a1)==0){
   if(MU(0x198)==0){if(MU(0x190)!=0)return false;flag=false;if(MF(0x184)==0.f&&MF(0x188)==0.f&&MF(0x18c)==0.f)return false;}
   else{flag=false;if(MU(0x198)<MU(0x190))return false;}}
 }else{
  if(!(MB(0x1a1)==0&&MF(0x174)==0.f&&MF(0x178)==0.f&&MF(0x17c)==0.f)){memcpy(s+0x184,s+0x174,12);}
  flag=true;
 }
 auto finish=[&]{MB(0x1a1)=0;if(0<pending){pending--;MI(0x180)=pending;}if(pending==0){memset(s+0x174,0,12);}MsAdvanceInput(s);return true;};
 if(MB(0x1a0)==0){
  memcpy(s+0x58,s+0x4c,12);memcpy(s+0x4c,s+0x184,12);
  float b[6];MsStdBox(e,s,b);MsStoreBox(e,s,b);MB(0xec)=0;MsPrevVel(s);MF(0x64)=MF(0x68)=MF(0x6c)=0.f;MQ(0x1b0)=0;return finish();
 }
 int steps=1;const uint64_t from=MU(0x190),to=MU(0x198);
 if(flag||(from<=to&&to!=0)){steps=1;const uint64_t dd=to-from;if(from<=to&&dd!=0){steps=dd<0x7fffffff?(int)dd:0x7fffffff;steps+=(dd<0x7fffffff);}}
 const float k=1.f/(float)steps;
 const float x=MF(0x4c)+(MF(0x184)-MF(0x4c))*k,y=MF(0x50)+(MF(0x188)-MF(0x50))*k,z=MF(0x54)+(MF(0x18c)-MF(0x54))*k;
 memcpy(s+0x58,s+0x4c,12);MF(0x4c)=x;MF(0x50)=y;MF(0x54)=z;
 float b[6];MsStdBox(e,s,b);MsStoreBox(e,s,b);MB(0xec)=0;MQ(0x1b0)=0;
 if(steps!=1)return true;
 return finish();
}
inline void MsClampVel(MsEnv& e,uint8_t* s){
 if(e.cfg[0xe]!=1)return;uint32_t u;memcpy(&u,e.cfg+0x10,4);u&=0x7fffffff;float m;memcpy(&m,&u,4);
 for(int k=0;k<3;++k){float v=MF(0x64+4*k);v=-m>v?-m:v;MF(0x64+4*k)=m<v?m:v;}
}
inline void MsHold(MsEnv& e,uint8_t* s){
 MQ(0x1c8)=0;MB(0x1c1)=0;memset(s+0xc8,0,12);
 memcpy(s+0x58,s+0xc,12);memcpy(s+0x4c,s+0,12);MB(0x120)=0;
 memcpy(s+0x70,s+0x24,12);memcpy(s+0x64,s+0x18,12);memcpy(s+0x88,s+0x3c,12);memcpy(s+0x7c,s+0x30,12);MB(0xec)=0;
 if(MB(0x1fd)||MB(0x1db))MB(0x1d6)=0;
 MsClampVel(e,s);
}
// One movement step. 0 moved, 1 interpolated, 2 not simulated (input position), 3 world not readable / restored,
// 4 frozen, 5 riding.
inline int MsMove(MsEnv& e,uint8_t* s,bool tryStand){
 MB(0x1d7)=0;
 if(MF(0xd4)<=0.f){float f=1.7999999523162842f;if(MB(0x1a6)==0&&MB(0x1ee)==0&&0.f<MF(0xc0))f=MF(0xc0);MF(0xd4)=f;if(!(0.f<MF(0xd8)))MF(0xd8)=1.4900000095367432f;}
 else if(MF(0xd8)<=0.f)MF(0xd8)=1.4900000095367432f;
 if(MF(0xdc)<=0.f)MF(0xdc)=0.6000000238418579f;
 if(MsInterp(e,s)){MQ(0x1c8)=0;MB(0x1c1)=0;memset(s+0xc8,0,12);return 1;}
 if(MB(0x1fc)==1){MsHold(e,s);memset(s+0x1d3,0,4);return 5;}
 bool hold;
 if(MB(0x1a1)==0&&MI(0x180)<1){const uint64_t t=MU(0x198);hold=t<=MU(0x190)||t==0;}
 else hold=MU(0x198)<=MU(0x190);
 if(hold){
  bool simulate=false;
  if((MU(0x21c)&0xfffffffdu)==0){if(MB(0x1fd)==0&&MB(0x200)==0&&MB(0x1db)==0&&MB(0x21a)!=0)simulate=true;}
  if(!simulate){MsHold(e,s);return 2;}
 }
 auto stop=[&]{MsPrevVel(s);MF(0x64)=MF(0x68)=MF(0x6c)=0.f;MQ(0x1c8)=0;MB(0x1c1)=0;memset(s+0xc8,0,12);};
 MsRefreshBox(e,s);
 float box[6];MsBox(e,s,box);
 if(!MsBoxSane(box)||!MsLoaded(e,box)){MsPrevVel(s);MF(0x64)=MF(0x68)=MF(0x6c)=0.f;MQ(0x1c8)=0;MB(0x1c1)=0;memset(s+0xc8,0,12);return 3;}
 if(MB(0x1da)!=0||MB(0x219)==0){stop();return 4;}
 int src=0x64;
 if(MB(0x170)!=0||(MQ(0x168)==0&&(MF(0x15c)!=0.f||MF(0x160)!=0.f||MF(0x164)!=0.f)))src=0x15c;
 float ex[6],g[6];MsExpand(ex,box,(const float*)(s+src));MsGrow(g,ex);
 if(!MsBoxSane(g)||!MsLoaded(e,g)){MsPrevVel(s);MF(0x64)=MF(0x68)=MF(0x6c)=0.f;MQ(0x1c8)=0;MB(0x1c1)=0;memset(s+0xc8,0,12);return 3;}
 MsState backup;memcpy(backup.m,s,0x220);
 bool ok=MsMoveBody(e,s);
 if(tryStand&&ok&&MI(0x1f4)==1&&MB(0x1dc)==0){
  if(MB(0x1b8)!=1)ok=MsTryStand(e,s);
  else if(MB(0x1c1)==0){if(!(0<MQ(0x1c8)))ok=MsTryStand(e,s);}
  else return 0;
 }
 if(ok)return 0;
 memcpy(s,backup.m,0x220);memcpy(s+0x70,backup.m+0x64,12);
 MF(0x64)=MF(0x68)=MF(0x6c)=0.f;MQ(0x1c8)=0;MB(0x1c1)=0;memset(s+0xc8,0,12);return 3;
}
// Output record (0x58 bytes): +0 position (feet), +0xc velocity, +0x18 last velocity, +0x24 input vector, +0x2c minor
// collision, +0x2d on ground, +0x2e / +0x2f / +0x30 collisions x / y / z, +0x34 position delta, +0x40 velocity delta,
// +0x4c rejected, +0x4d result code (non-zero = failed tick), +0x4e landed this tick, +0x50 fall distance at landing.
inline void MsWriteOutput(const MsEnv& e,uint8_t* o,const uint8_t* s,int code){
 memset(o,0,0x58);memcpy(o,s+0x4c,12);memcpy(o+0xc,s+0x64,12);memcpy(o+0x18,s+0x7c,12);
 o[0x2c]=code?0:MB(0x1d7);o[0x2d]=MB(0x1d6);o[0x2e]=MB(0x1d3);o[0x2f]=MB(0x1d4);o[0x30]=MB(0x1d5);
 float d[3],dv[3];for(int k=0;k<3;++k){d[k]=MF(0x4c+4*k)-MF(4*k);dv[k]=MF(0x64+4*k)-MF(0x18+4*k);}
 memcpy(o+0x34,d,12);memcpy(o+0x40,dv,12);o[0x4d]=(uint8_t)code;
 float l1,l2;memcpy(&l1,e.cfg+4,4);memcpy(&l2,e.cfg+8,4);
 bool b1=false,b3=false;if(0.f<l1)b1=l1<std::sqrt(d[2]*d[2]+d[1]*d[1]+d[0]*d[0]);if(0.f<l2)b3=l2<std::sqrt(dv[2]*dv[2]+dv[1]*dv[1]+dv[0]*dv[0]);
 o[0x4c]=e.cfg[0]==2?0:(e.cfg[0]==1?b1:(b1|b3));
}
inline bool MsInputIdle(const uint8_t* s){
 if(MU(0x198)==0){if(MU(0x190)==0){if(MF(0x184)!=0.f||MF(0x188)!=0.f)return true;return MF(0x18c)!=0.f;}return false;}
 return MU(0x190)<=MU(0x198);
}
// One simulated tick.
inline void MsTick(MsEnv& e,MsOutput& out,MsState& st,const MsInput& input){
 uint8_t* s=st.m;uint8_t* o=out.m;const uint8_t* in=input.m;
 auto failed=[&]{memset(o,0,0x58);o[0x4c]=e.cfg[0]!=2;o[0x4d]=6;};
 if(!MsSane(s)){failed();return;}
 if(!MsInputSane(in)){failed();if(MsSane(s)){memcpy(o,s+0x4c,12);memcpy(o+0xc,s+0x64,12);memcpy(o+0x18,s+0x7c,12);
   o[0x2d]=MB(0x1d6);o[0x2e]=MB(0x1d3);o[0x2f]=MB(0x1d4);o[0x30]=MB(0x1d5);
   float d[6];for(int k=0;k<3;++k){d[k]=MF(0x4c+4*k)-MF(4*k);d[3+k]=MF(0x64+4*k)-MF(0x18+4*k);}memcpy(o+0x34,d,24);}return;}
 MsRefreshBox(e,s);
 uint8_t snap1[0xc],snap2[0x34];memcpy(snap1,s+0xbc,0xc);memcpy(snap2,s+0xf0,0x34);
 const uint8_t s1a6=MB(0x1a6),s1b8=MB(0x1b8),s1ee=MB(0x1ee),s1dc=MB(0x1dc),s1fa=MB(0x1fa);const float s1bc=MF(0x1bc);const int32_t s1f4=MI(0x1f4);
 float move[2]={0,0};const bool prepared=MsPrepare(e,s,in,move);
 const uint8_t groundBefore=MB(0x1d6);const float fallBefore=MF(0x130);
 int code;bool skipToFrozen=false;
 if(!prepared&&MB(0x1a1)==0&&MI(0x180)<1&&!MsInputIdle(s)&&MB(0x1fc)==0){
  MsPrevVel(s);MF(0x64)=MF(0x68)=MF(0x6c)=0.f;MQ(0x1c8)=0;MB(0x1c1)=0;memset(s+0xc8,0,12);code=3;skipToFrozen=true;
 }else{code=MsMove(e,s,true);if(code!=3)MB(0x1f9)=0;}
 if(e.cfg[0xd]==1)MsSprintInput(s,in);
 if(code==3){
  memcpy(s+0xbc,snap1,0xc);memcpy(s+0xf0,snap2,0x34);MB(0x1a6)=s1a6;MB(0x1b8)=s1b8;MB(0x1ee)=s1ee;MF(0x1bc)=s1bc;MB(0x1dc)=s1dc;MI(0x1f4)=s1f4;MB(0x1fa)=s1fa;
  MsWriteOutput(e,o,s,3);memcpy(o+0x24,move,8);return;
 }
 (void)skipToFrozen;
 MF(0x13c)=MsAirSpeed(s);
 bool tickInput=true;if(code==1&&MB(0x1a1)==0&&MI(0x180)<1)tickInput=MsInputIdle(s);
 MsTimers(s,tickInput);
 MsWriteOutput(e,o,s,code);memcpy(o+0x24,move,8);
 if(code!=0||groundBefore!=0)return;
 o[0x4e]=MB(0x1d6);const float fall=MB(0x1d6)==1?fallBefore:0.f;memcpy(o+0x50,&fall,4);
}

// ---- initialisation from the local player -----------------------------------------------------------------------
// Rotation: pitch, head yaw (ActorHeadRotationComponent, else body yaw), body yaw.
inline bool MsRotation(const MsEnv& e,float* r){
 uint64_t rot{};float p{},y{};if(!Read(e.actor+0x250,&rot)||!rot||!Read(rot,&p)||!Read(rot+4,&y))return false;
 float head=y;const uintptr_t h=MsComponent(e,0xbabe7211u,8);if(h)Read(h,&head);
 r[0]=p;r[1]=head;r[2]=y;return true;
}
inline void MsDefaults(uint8_t* s){
 memset(s,0,sizeof(MsState));
 MF(0xbc)=0.6000000238418579f;MF(0xc0)=1.7999999523162842f;MF(0xc4)=1.f;
 MF(0x134)=0.10000000149011612f;MF(0x138)=0.10000000149011612f;MF(0x13c)=0.019999999552965164f;MF(0x140)=0.f;
 MQ(0x168)=1;MQ(0x190)=1;MB(0x1ec)=1;MB(0x219)=1;MB(0x21a)=1;
}
// State from the entity: feet position (AABB bottom centre), velocity, rotation, dimensions, flags, speed, abilities.
inline bool MsFromActor(MsEnv& e,uint8_t* s){
 uint64_t bbp{},st{};float bb[6],vel[3],rot[3];
 if(!Read(e.actor+0x248,&bbp)||!bbp||!Floats(bbp,bb,6)||!Read(e.actor+0x240,&st)||!st||!Floats(st+0x18,vel,3)||!MsRotation(e,rot))return false;
 const float pos[3]={(bb[0]+bb[3])*0.5f,bb[1],(bb[2]+bb[5])*0.5f};
 memcpy(s+0x58,s+0x4c,12);memcpy(s+0x4c,pos,12);
 memcpy(s+0x70,s+0x64,12);memcpy(s+0x64,vel,12);
 memcpy(s+0xa0,s+0x94,12);memcpy(s+0x94,rot,12);
 memcpy(s+0,pos,12);memcpy(s+0x18,vel,12);
 MB(0x120)=0;MF(0xd4)=1.7999999523162842f;
 const uintptr_t shape=MsComponent(e,0xbac1b3cfu,0x20);float dims[2]{},abox[6]{};
 if(shape&&Floats(shape+0x18,dims,2)&&Floats(shape,abox,6)){memcpy(s+0xbc,dims,8);MF(0xc4)=1.f;}else if(shape)return false;
 {uintptr_t st{};uint32_t pk{};MB(0x1d6)=Storage(e.c.registry,0xc29078a0u,&st)&&Packed(st,e.c.entity,&pk);}
 MB(0x1d3)=0;MB(0x1d5)=0;MB(0x1d4)=0;
 {const uintptr_t f=MsComponent(e,0xce6b34f6u,8);uint32_t v=0;if(f)Read(f,&v);memcpy(s+0x130,&v,4);}
 MB(0x1a6)=MsFlag(e,1);MB(0x1a2)=MsFlag(e,3);MB(0x1b8)=MsFlag(e,0x39);MB(0x1dc)=MsFlag(e,0x20);
 MI(0x1e8)=MsRockets(e);
 MB(0x1ee)=MsFlag(e,0x72);MB(0x1fc)=MsFlag(e,2);
 {float cur{},top{};if(Attribute(e.c.registry,e.c.entity,9,&cur,&top)){MF(0x134)=cur;MF(0x138)=MB(0x1a2)?cur/1.2999999523162842f:cur;}}
 if(const uintptr_t ab=MsComponent(e,0x6de78dc5u,0x10)){uint8_t b{};Read(ab+4,&b);MB(0x1fd)=b&1;MB(0x1fe)=(b>>1)&1;MB(0x1db)=(b>>4)&1;}
 MB(0x1ff)=1;
 {static const uint32_t table[6]={1,2,0,0,0,6};int32_t mode=-1;if(const uintptr_t gm=MsComponent(e,0x88d3eddfu,4))Read(gm,&mode);
  const uint32_t m=(uint32_t)(mode-1);MI(0x21c)=m<6?(int32_t)table[m]:0;}
 MF(0x13c)=MsAirSpeed(s);
 if(shape){
  memcpy(s+0xf0,abox,24);memcpy(s+0x108,pos,12);
  MF(0x114)=MF(0xbc)*0.5f*MF(0xc4);MF(0x118)=MsHeight(s)*MF(0xc4);MF(0x11c)=0.f;MB(0x120)=1;
 }
 return true;
}
// Input record: current move vector (MoveInputComponent +0x24) and its flags (+0x60 bits 0, 1, 4), rotation.
inline bool MsInputFrom(MsEnv& e,uint8_t* in){
 memset(in,0,sizeof(MsInput));float rot[3];if(!MsRotation(e,rot))return false;
 memcpy(in+0xc,&rot[0],4);memcpy(in+0x10,&rot[2],4);memcpy(in+0x14,&rot[1],4);
 if(const uintptr_t mi=MsComponent(e,0x018b1887u,100)){Read(mi+0x24,(uint64_t*)in);uint8_t b{};Read(mi+0x60,&b);in[0x3a]=b&1;in[0x3c]=(b>>4)&1;in[0x36]=(b>>1)&1;}
 return true;
}
// A ready simulator: environment (configuration mode 2, distance test off), state and input of the local player.
struct MsSim{MsEnv env;MsState state;MsInput input;};
inline bool MsInit(const Context& c,uint64_t region,MsShapes* shapes,MsSim& sim){
 MsEnv& e=sim.env;e.c=c;e.actor=c.actor;e.region=region;e.shapes=shapes;memset(e.cfg,0,sizeof(e.cfg));e.cfg[0]=2;e.cfg[0x14]=1;
 MsDefaults(sim.state.m);
 return region&&shapes&&MsFromActor(e,sim.state.m)&&MsInputFrom(e,sim.input.m);
}
// Output accessors.
inline const float* MsOutPos(const MsOutput& o){return (const float*)o.m;}
inline bool MsOutFailed(const MsOutput& o){return o.m[0x4d]!=0;}
inline bool MsOutGround(const MsOutput& o){return (o.m[0x2d]&1)!=0;}
inline bool MsOutLanded(const MsOutput& o){return o.m[0x4e]!=0;}
inline float MsOutFall(const MsOutput& o){float f;memcpy(&f,o.m+0x50,4);return f;}

#undef MF
#undef MB
#undef MI
#undef MQ
#undef MU
}
