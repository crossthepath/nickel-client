// Chest ESP: finds chests, ender chests and trapped chests on the game's tick thread (hook #249, shared
// with MiniMap), with a 1 ms budget measured from the start of the tick.
// 1. Chunks in range: offsets (dx,dz) in [-n,n], n = Range/16+1, kept if d(dx)^2+d(dz)^2 <= Range^2 with
// d(v) = (max(|v|,1)-1)*16; chunk = region->vtable[0x148](region,&{x,z}); subchunks
// [chunk+0x1c8,+0x1d0) of 0x68 bytes.
// 2. Changed subchunks (hook #251) are re-read first, then never-read chunks, then round-robin (layer
// pointers and palette size) while time remains.
// 3. Per subchunk: layers 0 and 1, palette (<= 4096); a block matches when its BlockLegacy name
// (*(block+0x68))+0xe8 is "minecraft:chest", "minecraft:ender_chest" or "minecraft:trapped_chest";
// index (x<<8)|(z<<4)|y.
// 4. Boxes: block entity = region->vtable[0x18](region,&pos); if type (+0x14) == 2 and paired *(be+0x308),
// the box spans both positions (pair position at +8); otherwise the single block. Sorted, deduplicated,
// singles inside a double box removed; output as floats [min, max+1].
// Drawing (menu.cpp): 6 Fill faces, then 12 Outline edges at Thickness if Draw Outline, name_tag material
// (no depth test, so visible through walls).
#pragma once
#include "game_render.hpp"
#include "game_visual_entities.hpp"
#include <array>
#include <algorithm>
#include <unordered_map>
#include <unordered_set>

namespace game {
struct ChestShared{
 SRWLOCK lock=SRWLOCK_INIT;volatile long enabled=0,range=128,fault=0,ticks=0;
 std::vector<std::array<float,6>> boxes;uint64_t version=0;
 std::mutex pendingGuard;std::vector<uint64_t> pending;
};
inline ChestShared& Chests(){static ChestShared* shared=new ChestShared;return *shared;}

struct ChestPos{int x,y,z;bool operator==(const ChestPos& o)const{return x==o.x&&y==o.y&&z==o.z;}};
struct ChestSub{uint64_t layer0=0,layer1=0,size0=0,size1=0;std::vector<ChestPos> found;};
struct ChestChunk{uint64_t chunk=0,begin=0;uint32_t count=0;bool read=false;uint64_t seen=0;int x=0,z=0;std::vector<ChestSub> subs;};
struct ChestState{uint64_t level=0,region=0,tick=0;int range=-1;size_t robin=0;std::vector<std::pair<int,int>> offsets;
 std::unordered_map<uint64_t,ChestChunk> tracked;std::unordered_map<uint64_t,bool> named;};

inline bool ChestBlock(ChestState& state,uint64_t block){
 if(!block)return false;auto found=state.named.find(block);if(found!=state.named.end())return found->second;uint64_t legacy{};char name[40]{};bool match=false;
 if(Read(block+0x68,&legacy)&&legacy&&Text(legacy+0xe8,name,sizeof(name)))match=!strcmp(name,"minecraft:chest")||!strcmp(name,"minecraft:ender_chest")||!strcmp(name,"minecraft:trapped_chest");
 if(state.named.size()<65536)state.named.emplace(block,match);return match;
}
inline uint64_t ChestPalette(const Context& context,uint64_t storage){if(!storage)return 0;const uint64_t get=Virtual(context,storage,0xa8);if(!get)return 0;uint64_t out[2]{};reinterpret_cast<void*(*)(uint64_t,uint64_t*)>(get)(storage,out);return out[0];}
// Layers 0 then 1.
inline void ScanChestSub(const Context& context,ChestState& state,ChestSub& sub,uint64_t address,int x0,int y0,int z0){
 sub.found.clear();sub.layer0=sub.layer1=sub.size0=sub.size1=0;if(!Read(address+0x30,&sub.layer0)||!Read(address+0x38,&sub.layer1))return;
 sub.size0=ChestPalette(context,sub.layer0);sub.size1=ChestPalette(context,sub.layer1);
 for(int layer=0;layer<2;++layer){const uint64_t storage=layer?sub.layer1:sub.layer0;if(!storage)continue;
  const uint64_t palette=Virtual(context,storage,0xa8),bitsOf=Virtual(context,storage,0xa0),wordsOf=Virtual(context,storage,0x90);if(!palette||!bitsOf||!wordsOf)continue;
  uint64_t out[2]{};reinterpret_cast<void*(*)(uint64_t,uint64_t*)>(palette)(storage,out);if(!out[0]||!out[1])continue;const uint32_t entries=(uint32_t)std::min<uint64_t>(out[0],4096);
  static bool match[4096];bool any=false;for(uint32_t i=0;i<entries;++i){uint64_t block{};match[i]=Read(out[1]+8ull*i,&block)&&ChestBlock(state,block);any=any||match[i];}if(!any)continue;
  auto add=[&](uint32_t at){sub.found.push_back({x0+(int)(at>>8),y0+(int)(at&15),z0+(int)((at>>4)&15)});};
  const int bits=reinterpret_cast<int(*)(uint64_t)>(bitsOf)(storage);
  if(bits<=0){if(match[0])for(uint32_t at=0;at<4096;++at)add(at);continue;}if(bits>32)continue;
  uint64_t words[2]{};reinterpret_cast<void*(*)(uint64_t,uint64_t*)>(wordsOf)(storage,words);if(!words[0]||!words[1])continue;const uint32_t per=32/bits,mask=bits==32?0xffffffffu:((1u<<bits)-1);
  for(uint32_t at=0;at<4096;++at){const uint32_t word=at/per;if(word>=words[0])break;uint32_t value{};if(!Read(words[1]+4ull*word,&value))break;const uint32_t pick=(value>>((at%per)*bits))&mask;if(pick<entries&&match[pick])add(at);}
 }
}
inline void ChestReset(ChestState& state){state.tracked.clear();state.named.clear();state.robin=0;auto& shared=Chests();AcquireSRWLockExclusive(&shared.lock);shared.boxes.clear();++shared.version;ReleaseSRWLockExclusive(&shared.lock);}
inline void ChestWork(void* levelPointer){
 static ChestState* kept=new ChestState;ChestState& state=*kept;auto& shared=Chests();
 if(!shared.enabled){if(state.level||!state.tracked.empty()){state.level=0;state.region=0;ChestReset(state);}return;}
 LARGE_INTEGER frequency{},start{};QueryPerformanceFrequency(&frequency);QueryPerformanceCounter(&start);auto late=[&]{LARGE_INTEGER now{};QueryPerformanceCounter(&now);return (now.QuadPart-start.QuadPart)*1000.0/frequency.QuadPart>1.0;};
 const uint64_t level=(uint64_t)levelPointer;Context context{};if(!Local(&context)){if(state.level){state.level=0;state.region=0;ChestReset(state);}return;}
 const uint64_t guard=Virtual(context,level,0x9e8);if(!guard||!reinterpret_cast<bool(*)(uint64_t)>(guard)(level))return;
 uint64_t owner{},region{},where{};uintptr_t element{};int16_t lowest{};float feet[3],lift{};
 if(!Read(context.actor+0x1f0,&owner)||!owner)return;const uint64_t regionOf=Virtual(context,owner,0x68);if(!regionOf)return;region=reinterpret_cast<uint64_t(*)(uint64_t)>(regionOf)(owner);if(!region||!Read(region+0x3a,&lowest))return;
 if(level!=state.level||region!=state.region){state.level=level;state.region=region;ChestReset(state);}
 if(!Read(context.actor+0x240,&where)||!where||!Floats(where,feet,3))return;if(Element(context.registry,context.entity,"OffsetsComponent",0x44,&element)&&Floats(element,&lift,1))feet[1]-=lift;
 const int bx=(int)std::floor(feet[0]),bz=(int)std::floor(feet[2]),minimumY=lowest;
 const int range=(int)shared.range;
 if(range!=state.range){state.range=range;state.offsets.clear();const int n=range/16+1;auto d=[](int v){const int a=v<0?-v:v;return (std::max(a,1)-1)*16;};
  for(int dx=-n;dx<=n;++dx)for(int dz=-n;dz<=n;++dz)if((unsigned)(d(dx)*d(dx)+d(dz)*d(dz))<=(unsigned)(range*range))state.offsets.push_back({dx,dz});
  std::stable_sort(state.offsets.begin(),state.offsets.end(),[](const std::pair<int,int>& a,const std::pair<int,int>& b){return a.first*a.first+a.second*a.second<b.first*b.first+b.second*b.second;});}
 const uint64_t chunkAt=Virtual(context,region,0x148);if(!chunkAt)return;++state.tick;std::vector<uint64_t> visible;visible.reserve(state.offsets.size());
 for(auto& offset:state.offsets){int position[2]={(bx>>4)+offset.first,(bz>>4)+offset.second};const uint64_t chunk=reinterpret_cast<uint64_t(*)(uint64_t,int*)>(chunkAt)(region,position);if(!chunk)continue;
  uint64_t begin{},end{};if(!Read(chunk+0x1c8,&begin)||!Read(chunk+0x1d0,&end)||end<begin)continue;const uint32_t count=(uint32_t)std::min<uint64_t>((end-begin)/0x68,64);const uint64_t key=((uint64_t)(uint32_t)position[0]<<32)|(uint32_t)position[1];ChestChunk& record=state.tracked[key];
  if(record.chunk!=chunk||record.begin!=begin||record.count!=count){record.chunk=chunk;record.begin=begin;record.count=count;record.read=false;record.x=position[0];record.z=position[1];record.subs.assign(count,ChestSub{});}record.seen=state.tick;visible.push_back(key);}
 for(auto it=state.tracked.begin();it!=state.tracked.end();){if(it->second.seen==state.tick)++it;else it=state.tracked.erase(it);}
 auto scan=[&](ChestChunk& record,uint32_t i){ScanChestSub(context,state,record.subs[i],record.begin+0x68ull*i,record.x*16,minimumY+16*(int)i,record.z*16);};
 {std::vector<uint64_t> changed;{std::lock_guard<std::mutex> lock(shared.pendingGuard);changed.swap(shared.pending);}
  if(!changed.empty()){std::sort(changed.begin(),changed.end());changed.erase(std::unique(changed.begin(),changed.end()),changed.end());
   for(uint64_t key:visible){ChestChunk& record=state.tracked[key];if(!record.read)continue;for(uint64_t address:changed)if(address>=record.begin&&address<record.begin+0x68ull*record.count&&(address-record.begin)%0x68==0)scan(record,(uint32_t)((address-record.begin)/0x68));}}}
 for(uint64_t key:visible){if(late())break;ChestChunk& record=state.tracked[key];if(record.read)continue;for(uint32_t i=0;i<record.count;++i)scan(record,i);record.read=true;}
 if(!visible.empty())for(size_t turn=0;turn<visible.size()&&!late();++turn){ChestChunk& record=state.tracked[visible[state.robin++%visible.size()]];if(!record.read)continue;
  for(uint32_t i=0;i<record.count;++i){const uint64_t address=record.begin+0x68ull*i;uint64_t layer0{},layer1{};if(!Read(address+0x30,&layer0)||!Read(address+0x38,&layer1))continue;ChestSub& sub=record.subs[i];
   if(layer0!=sub.layer0||layer1!=sub.layer1||ChestPalette(context,layer0)!=sub.size0||ChestPalette(context,layer1)!=sub.size1)scan(record,i);}}
 // boxes
 const uint64_t entityAt=Virtual(context,region,0x18);struct Box{int lo[3],hi[3];};std::vector<Box> boxes;
 for(auto& entry:state.tracked)for(auto& sub:entry.second.subs)for(const ChestPos& p:sub.found){Box box{{p.x,p.y,p.z},{p.x,p.y,p.z}};
  if(entityAt){int pos[3]={p.x,p.y,p.z};const uint64_t be=reinterpret_cast<uint64_t(*)(uint64_t,int*)>(entityAt)(region,pos);uint8_t type{};uint64_t pair{};int q[3]{};
   if(be&&Read(be+0x14,&type)&&type==2&&Read(be+0x308,&pair)&&pair&&Read(pair+8,&q[0])&&Read(pair+12,&q[1])&&Read(pair+16,&q[2]))for(int k=0;k<3;++k){box.lo[k]=std::min(pos[k],q[k]);box.hi[k]=std::max(pos[k],q[k]);}}
  boxes.push_back(box);}
 auto key6=[](const Box& b){return std::array<int,6>{b.lo[0],b.lo[1],b.lo[2],b.hi[0],b.hi[1],b.hi[2]};};
 std::sort(boxes.begin(),boxes.end(),[&](const Box& a,const Box& b){return key6(a)<key6(b);});boxes.erase(std::unique(boxes.begin(),boxes.end(),[&](const Box& a,const Box& b){return key6(a)==key6(b);}),boxes.end());
 auto pack=[](const int* v){return ((uint64_t)(uint32_t)v[0]<<38)|((uint64_t)(v[2]&0x3ffffff)<<12)|(uint64_t)(v[1]&0xfff);};
 std::unordered_set<uint64_t> doubled;for(const Box& b:boxes)if(b.lo[0]!=b.hi[0]||b.lo[1]!=b.hi[1]||b.lo[2]!=b.hi[2]){doubled.insert(pack(b.lo));doubled.insert(pack(b.hi));}
 boxes.erase(std::remove_if(boxes.begin(),boxes.end(),[&](const Box& b){return b.lo[0]==b.hi[0]&&b.lo[1]==b.hi[1]&&b.lo[2]==b.hi[2]&&doubled.count(pack(b.lo));}),boxes.end());
 std::vector<std::array<float,6>> out;out.reserve(boxes.size());for(const Box& b:boxes)out.push_back({(float)b.lo[0],(float)b.lo[1],(float)b.lo[2],(float)b.hi[0]+1,(float)b.hi[1]+1,(float)b.hi[2]+1});
 AcquireSRWLockExclusive(&shared.lock);shared.boxes.swap(out);++shared.version;ReleaseSRWLockExclusive(&shared.lock);InterlockedIncrement(&shared.ticks);
}
inline void ChestGuarded(void* level){__try{ChestWork(level);}__except(EXCEPTION_EXECUTE_HANDLER){InterlockedExchange(&Chests().fault,1);}}
}
