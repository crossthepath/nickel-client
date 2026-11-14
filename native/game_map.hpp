// MiniMap terrain: reads chunks on the game's tick thread and builds the HUD ring texture and the
// full-screen map. Hooks: #249 level tick (RVA 0x2323c10, rcx = level; guarded by level->vtable[0x9e8]();
// game called afterwards) and #251 subchunk set-block (RVA 0x5cf63d0, rcx = subchunk; game called first).
// Path: player A -> *(A+0x1f0) -> R = ptr->vtable[0x68](ptr) -> min Y int16 at R+0x3a -> chunk =
// R->vtable[0x148](R,&{x,z}) -> subchunks [chunk+0x1c8, +0x1d0) of 0x68 bytes -> storage *(subchunk+0x30):
// palette vtable[0xa8](S,&{size,data}), bits per block vtable[0xa0](S), words vtable[0x90](S,&{count,data})
// -> block+0x68 = BlockLegacy: name at +0xe8, map colour float r,g,b,a at +0x168. Air or alpha <= 0 is
// transparent; water is 0xFFFF4040. Block index = (x<<8)|(z<<4)|y. Subchunk read budget per tick: 4 ms while visible chunks are unread, else 1 ms;
// chunks are re-checked round-robin (layer pointers and palette size). Height Shading: 220, 180 when lower
// than the north neighbour, 255 when higher; water shaded by depth. Outputs, under lock: a toroidal RGBA
// ring of 16W x 16W texels (W = 2n+1, n = ceil(zoom/16)+1) for the HUD element, and a 16 x 16 pixel tile
// per chunk for the full-screen map (at most 16384 chunks, trimmed to 14336). The weak reference at A+0x1f0
// is not locked; hook #251 accepts any thread (queued under lock). Everything runs under SEH: on the first
// fault terrain reading is disabled until the game restarts.
#pragma once
#include "game_render.hpp"
#include "game_visual_entities.hpp"
#include "game_chest.hpp"
#include "game_sus.hpp"
#include <array>
#include <algorithm>
#include <unordered_map>
#include <unordered_set>

namespace game {
struct MapShared{
 SRWLOCK lock=SRWLOCK_INIT;volatile long enabled=0,zoom=64,rotate=0,shading=1;
 std::vector<uint32_t> ring;int cells=0;uint64_t version=0;                                  // toroidal ring for the HUD element
 std::unordered_map<uint64_t,std::array<uint32_t,256>> store;uint64_t storeVersion=0;        // full-screen map: index z*16+x
 volatile long fault=0,ticks=0,chunksRead=0,loadMs=-1;int minimumY=0,dimension=0,subChunks=0;   // loadMs: last time to read every visible chunk
 std::mutex pendingGuard;std::vector<uint64_t> pending;                                      // changed subchunks (hook #251)
};
inline MapShared& Map(){static MapShared* shared=new MapShared;return *shared;}
inline uint64_t ChunkKey(int x,int z){return ((uint64_t)(uint32_t)x<<32)|(uint32_t)z;}

struct MapColumn{uint16_t color;uint8_t y,depth;};
struct MapSub{uint64_t layer0=0,layer1=0,size0=0,size1=0;std::array<MapColumn,256> column{};};
struct MapChunk{uint64_t chunk=0,begin=0;uint32_t count=0;bool read=false;uint64_t seen=0;std::vector<MapSub> subs;};
struct MapColumns{uint32_t color[256];int height[256];uint8_t depth[256];};
struct MapState{ULONGLONG loading=0;uint64_t level=0,tick=0;int n=-1,cells=0,cap=-2,shading=-1;size_t robin=0;std::vector<std::pair<int,int>> offsets;
 std::unordered_map<uint64_t,MapChunk> tracked;std::unordered_map<uint64_t,MapColumns> columns;std::unordered_map<uint64_t,uint16_t> blockIndex;
 std::vector<std::pair<uint32_t,uint8_t>> palette;std::vector<uint64_t> cellKey;std::vector<uint8_t> cellValid;};

inline uint64_t PaletteSize(const Context& context,uint64_t storage){if(!storage)return 0;const uint64_t get=Virtual(context,storage,0xa8);if(!get)return 0;uint64_t out[2]{};reinterpret_cast<void*(*)(uint64_t,uint64_t*)>(get)(storage,out);return out[0];}
// Colour index of a block (0 = transparent), cached by block address.
inline uint16_t BlockColor(MapState& state,uint64_t block){
 auto found=state.blockIndex.find(block);if(found!=state.blockIndex.end())return found->second;uint16_t index=0;uint64_t legacy{};char name[40]{};
 if(block&&Read(block+0x68,&legacy)&&legacy&&state.palette.size()<65535){
  const bool named=Text(legacy+0xe8,name,sizeof(name));
  if(named&&!strcmp(name,"minecraft:air")){}
  else if(named&&(!strcmp(name,"minecraft:water")||!strcmp(name,"minecraft:flowing_water"))){index=(uint16_t)state.palette.size();state.palette.push_back({0xFFFF4040u,(uint8_t)1});}
  else{float c[4];if(Floats(legacy+0x168,c,4)&&c[3]>0){auto channel=[](float v){return (uint32_t)std::clamp((int)(v*255+.5f),0,255);};index=(uint16_t)state.palette.size();state.palette.push_back({0xFF000000u|(channel(c[2])<<16)|(channel(c[1])<<8)|channel(c[0]),(uint8_t)0});}}}
 state.blockIndex.emplace(block,index);return index;
}
inline bool MapCopy(uint64_t from,void* to,size_t bytes){__try{memcpy(to,(const void*)from,bytes);return true;}__except(EXCEPTION_EXECUTE_HANDLER){return false;}}
// One subchunk: palette and words are copied in one read each, then every column is scanned from the top down to its
// first coloured block (water: the contiguous water below it gives the depth), instead of decoding all 4096 blocks.
inline void ReadMapSub(const Context& context,MapState& state,MapSub& sub,uint64_t address){
 sub.column.fill(MapColumn{});sub.layer0=sub.layer1=sub.size0=sub.size1=0;if(!Read(address+0x30,&sub.layer0)||!Read(address+0x38,&sub.layer1))return;
 sub.size1=PaletteSize(context,sub.layer1);const uint64_t storage=sub.layer0;if(!storage)return;const uint64_t palette=Virtual(context,storage,0xa8),bitsOf=Virtual(context,storage,0xa0),wordsOf=Virtual(context,storage,0x90);if(!palette||!bitsOf||!wordsOf)return;
 uint64_t out[2]{};reinterpret_cast<void*(*)(uint64_t,uint64_t*)>(palette)(storage,out);sub.size0=out[0];if(!out[0]||!out[1])return;const uint32_t entries=(uint32_t)std::min<uint64_t>(out[0],4096);
 static uint16_t index[4096];static uint64_t blocks[4096];if(!MapCopy(out[1],blocks,8ull*entries))return;bool any=false;for(uint32_t i=0;i<entries;++i){index[i]=blocks[i]?BlockColor(state,blocks[i]):0;any=any||index[i];}if(!any)return;
 auto water=[&](uint16_t color){return state.palette[color].second!=0;};
 const int bits=reinterpret_cast<int(*)(uint64_t)>(bitsOf)(storage);
 if(bits<=0){const uint16_t color=index[0];if(!color)return;for(auto& column:sub.column)column={color,(uint8_t)15,(uint8_t)(water(color)?16:0)};return;}if(bits>32)return;
 uint64_t words[2]{};reinterpret_cast<void*(*)(uint64_t,uint64_t*)>(wordsOf)(storage,words);if(!words[0]||!words[1])return;const uint32_t per=32/bits,mask=bits==32?0xffffffffu:((1u<<bits)-1);
 static uint32_t packed[4096];const uint32_t count=(uint32_t)std::min<uint64_t>(words[0],4096);if(!MapCopy(words[1],packed,4ull*count))return;
 auto at=[&](uint32_t i)->uint16_t{const uint32_t word=i/per;if(word>=count)return 0;const uint32_t pick=(packed[word]>>((i%per)*bits))&mask;return pick<entries?index[pick]:0;};
 for(uint32_t c=0;c<256;++c){int y=15;uint16_t color=0;for(;y>=0;--y){color=at(c*16+(uint32_t)y);if(color)break;}if(!color)continue;uint8_t depth=0;
  if(water(color)){depth=1;for(int below=y-1;below>=0;--below){const uint16_t under=at(c*16+(uint32_t)below);if(!under||!water(under))break;++depth;}}
  sub.column[c]={color,(uint8_t)y,depth};}
}
inline void ComposeMap(MapState& state,const MapChunk& chunk,MapColumns& out,int minimumY){
 const int top=state.cap>=0?std::min<int>(state.cap+1,(int)chunk.count):(int)chunk.count;
 for(int c=0;c<256;++c){out.color[c]=0;out.height[c]=minimumY;out.depth[c]=0;
  for(int i=top-1;i>=0;--i){const MapColumn& column=chunk.subs[i].column[c];if(!column.color)continue;const auto& entry=state.palette[column.color];out.color[c]=entry.first;out.height[c]=minimumY+16*i+column.y;int depth=column.depth;
   if(entry.second&&depth==column.y+1)for(int j=i-1;j>=0;--j){const MapColumn& below=chunk.subs[j].column[c];if(!below.color||!state.palette[below.color].second||below.y!=15)break;depth+=below.depth;if(below.depth!=16)break;}
   out.depth[c]=(uint8_t)std::min(depth,255);break;}}
}
inline void ShadeMap(MapState& state,uint64_t key,bool shading,std::array<uint32_t,256>& out){
 out.fill(0);auto found=state.columns.find(key);if(found==state.columns.end())return;const MapColumns& mine=found->second;const int X=(int)(key>>32),Z=(int)(uint32_t)key;
 auto north=state.columns.find(ChunkKey(X,Z-1));const MapColumns* above=north==state.columns.end()?nullptr:&north->second;
 for(int z=0;z<16;++z)for(int x=0;x<16;++x){const int c=x*16+z;uint32_t color=mine.color[c];if(color<=0xFFFFFFu){continue;}
  if(shading){uint32_t f=220;if(!mine.depth[c]){const int h=mine.height[c],hn=z?mine.height[c-1]:above?above->height[x*16+15]:h;f=h<hn?180:h>hn?255:220;}else{const float v=((x+z)&1)*.2f+mine.depth[c]*.1f;f=v<.5f?255:v>.9f?180:220;}
   const uint32_t r=(color&0xff)*f/255,g=((color>>8)&0xff)*f/255,b=((color>>16)&0xff)*f/255;color=(color&0xff000000u)|(b<<16)|(g<<8)|r;}
  out[z*16+x]=color;}
}
inline void ResetMap(MapState& state){
 state.tracked.clear();state.columns.clear();state.blockIndex.clear();state.palette.assign(1,{0u,(uint8_t)0});std::fill(state.cellValid.begin(),state.cellValid.end(),(uint8_t)0);state.robin=0;state.cap=-2;
 auto& shared=Map();AcquireSRWLockExclusive(&shared.lock);shared.store.clear();std::fill(shared.ring.begin(),shared.ring.end(),0u);++shared.version;++shared.storeVersion;ReleaseSRWLockExclusive(&shared.lock);
}
inline void MapWork(void* levelPointer){
 static MapState* kept=new MapState;MapState& state=*kept;auto& shared=Map();if(!shared.enabled)return;const uint64_t level=(uint64_t)levelPointer;
 Context context{};if(!Local(&context)){if(state.level){state.level=0;ResetMap(state);}return;}
 const uint64_t guard=Virtual(context,level,0x9e8);if(!guard||!reinterpret_cast<bool(*)(uint64_t)>(guard)(level))return;
 if(level!=state.level){state.level=level;ResetMap(state);}
 uint64_t owner{},region{},where{};uintptr_t storage{},element{};int32_t dimension{};int16_t lowest{};float feet[3],lift{};
 if(!Read(context.actor+0x1f0,&owner)||!owner)return;const uint64_t regionOf=Virtual(context,owner,0x68);if(!regionOf)return;region=reinterpret_cast<uint64_t(*)(uint64_t)>(regionOf)(owner);if(!region)return;
 if(!Storage(context.registry,0xaf993816u,&storage)||!InStorage(storage,context.entity,4,&element)||!Read(element,&dimension)||!Read(region+0x3a,&lowest))return;
 if(!Read(context.actor+0x240,&where)||!where||!Floats(where,feet,3))return;if(Element(context.registry,context.entity,"OffsetsComponent",0x44,&element)&&Floats(element,&lift,1))feet[1]-=lift;
 const int bx=(int)std::floor(feet[0]),by=(int)std::floor(feet[1]),bz=(int)std::floor(feet[2]),minimumY=lowest;const bool shading=shared.shading!=0;
 const float reach=shared.rotate?shared.zoom*1.41421356f:(float)shared.zoom;const int n=std::clamp((int)std::ceil(reach/16)+1,2,60),cells=2*n+1;
 if(n!=state.n){state.n=n;state.cells=cells;state.offsets.clear();for(int dx=-n;dx<=n;++dx)for(int dz=-n;dz<=n;++dz)state.offsets.push_back({dx,dz});
  std::stable_sort(state.offsets.begin(),state.offsets.end(),[](const std::pair<int,int>& a,const std::pair<int,int>& b){return a.first*a.first+a.second*a.second<b.first*b.first+b.second*b.second;});
  state.cellKey.assign((size_t)cells*cells,~0ull);state.cellValid.assign((size_t)cells*cells,0);AcquireSRWLockExclusive(&shared.lock);shared.ring.assign((size_t)cells*16*cells*16,0u);shared.cells=cells;++shared.version;ReleaseSRWLockExclusive(&shared.lock);}
 std::unordered_set<uint64_t> compose,shade;
 const int cap=dimension==1?std::max(0,(by-minimumY)>>4):-1;if(cap!=state.cap){state.cap=cap;for(auto& entry:state.tracked)if(entry.second.read)compose.insert(entry.first);}
 if((int)shading!=state.shading){state.shading=shading;std::fill(state.cellValid.begin(),state.cellValid.end(),(uint8_t)0);}
 double budget=1.0;LARGE_INTEGER frequency{},start{};QueryPerformanceFrequency(&frequency);QueryPerformanceCounter(&start);auto late=[&]{LARGE_INTEGER now{};QueryPerformanceCounter(&now);return (now.QuadPart-start.QuadPart)*1000.0/frequency.QuadPart>budget;};
 const uint64_t chunkAt=Virtual(context,region,0x148);if(!chunkAt)return;++state.tick;std::vector<uint64_t> visible;visible.reserve(state.offsets.size());
 for(auto& offset:state.offsets){int position[2]={(bx>>4)+offset.first,(bz>>4)+offset.second};const uint64_t chunk=reinterpret_cast<uint64_t(*)(uint64_t,int*)>(chunkAt)(region,position);if(!chunk)continue;
  uint64_t begin{},end{};if(!Read(chunk+0x1c8,&begin)||!Read(chunk+0x1d0,&end)||end<begin)continue;const uint32_t count=(uint32_t)std::min<uint64_t>((end-begin)/0x68,64);const uint64_t key=ChunkKey(position[0],position[1]);MapChunk& record=state.tracked[key];
  if(record.chunk!=chunk||record.begin!=begin||record.count!=count){record.chunk=chunk;record.begin=begin;record.count=count;record.read=false;record.subs.assign(count,MapSub{});}record.seen=state.tick;visible.push_back(key);}
 for(auto it=state.tracked.begin();it!=state.tracked.end();){if(it->second.seen==state.tick){++it;continue;}const uint64_t key=it->first;if(state.columns.erase(key)){shade.insert(key);shade.insert(ChunkKey((int)(key>>32),(int)(uint32_t)key+1));}it=state.tracked.erase(it);}
 for(uint64_t key:visible)if(!state.tracked[key].read){budget=4.0;break;}   // 4 ms while visible chunks are still unread (first load, teleport), else 1 ms
 if(budget>1.0&&!state.loading)state.loading=GetTickCount64();
 QueryPerformanceCounter(&start);   // the read budget starts here: walking the visible chunks above must not consume it
 {std::vector<uint64_t> changed;{std::lock_guard<std::mutex> lock(shared.pendingGuard);changed.swap(shared.pending);}
  if(!changed.empty())for(uint64_t key:visible){MapChunk& record=state.tracked[key];if(!record.read)continue;for(uint64_t address:changed)if(address>=record.begin&&address<record.begin+0x68ull*record.count&&(address-record.begin)%0x68==0){ReadMapSub(context,state,record.subs[(address-record.begin)/0x68],address);compose.insert(key);}}}
 for(uint64_t key:visible){MapChunk& record=state.tracked[key];if(record.read)continue;if(late())break;for(uint32_t i=0;i<record.count;++i)ReadMapSub(context,state,record.subs[i],record.begin+0x68ull*i);record.read=true;compose.insert(key);InterlockedIncrement(&shared.chunksRead);}
 // Round-robin: keep re-checking chunks one by one for the rest of the budget, not just one per tick;
 // otherwise a chunk read before its subchunks arrived stays grey or empty for a long time.
 for(size_t turn=0;turn<visible.size()&&!late();++turn){MapChunk& record=state.tracked[visible[state.robin++%visible.size()]];if(record.read)for(uint32_t i=0;i<record.count;++i){const uint64_t address=record.begin+0x68ull*i;uint64_t layer0{},layer1{};if(!Read(address+0x30,&layer0)||!Read(address+0x38,&layer1))continue;MapSub& sub=record.subs[i];
   if(layer0!=sub.layer0||layer1!=sub.layer1||PaletteSize(context,layer0)!=sub.size0||PaletteSize(context,layer1)!=sub.size1){ReadMapSub(context,state,sub,address);compose.insert(visible[(state.robin-1)%visible.size()]);}}}
 for(uint64_t key:compose){auto found=state.tracked.find(key);if(found==state.tracked.end()||!found->second.read)continue;ComposeMap(state,found->second,state.columns[key],minimumY);shade.insert(key);shade.insert(ChunkKey((int)(key>>32),(int)(uint32_t)key+1));}
 bool wrote=false,stored=false;std::array<uint32_t,256> pixels;
 for(auto& offset:state.offsets){const int X=(bx>>4)+offset.first,Z=(bz>>4)+offset.second;const uint64_t key=ChunkKey(X,Z);const int cx=((X%cells)+cells)%cells,cz=((Z%cells)+cells)%cells;const size_t cell=(size_t)cz*cells+cx;
  if(state.cellValid[cell]&&state.cellKey[cell]==key&&!shade.count(key))continue;ShadeMap(state,key,shading,pixels);const bool known=state.columns.count(key)!=0;
  AcquireSRWLockExclusive(&shared.lock);if(known){shared.store[key]=pixels;stored=true;}if(shared.cells==cells&&shared.ring.size()==(size_t)cells*16*cells*16)for(int row=0;row<16;++row)memcpy(&shared.ring[((size_t)cz*16+row)*cells*16+(size_t)cx*16],&pixels[row*16],64);ReleaseSRWLockExclusive(&shared.lock);
  state.cellKey[cell]=key;state.cellValid[cell]=1;wrote=true;}
 if(wrote||stored){AcquireSRWLockExclusive(&shared.lock);if(wrote)++shared.version;if(stored)++shared.storeVersion;
  if(shared.store.size()>0x4000){std::vector<std::pair<int64_t,uint64_t>> order;order.reserve(shared.store.size());for(auto& entry:shared.store){const int64_t dx=(int)(entry.first>>32)-(bx>>4),dz=(int)(uint32_t)entry.first-(bz>>4);order.push_back({dx*dx+dz*dz,entry.first});}std::nth_element(order.begin(),order.begin()+0x3800,order.end());for(size_t i=0x3800;i<order.size();++i)shared.store.erase(order[i].second);}
  shared.minimumY=minimumY;shared.dimension=dimension;ReleaseSRWLockExclusive(&shared.lock);}
 if(state.loading){bool all=true;for(uint64_t key:visible)if(!state.tracked[key].read){all=false;break;}if(all){InterlockedExchange(&shared.loadMs,(long)(GetTickCount64()-state.loading));state.loading=0;}}
 InterlockedIncrement(&shared.ticks);
}
inline void MapGuarded(void* level){__try{MapWork(level);}__except(EXCEPTION_EXECUTE_HANDLER){InterlockedExchange(&Map().fault,1);}}
using TickFunction=uint64_t(*)(void*);using SetBlockFunction=uint64_t(*)(void*,uint8_t,uint16_t,void*);inline TickFunction tickOriginal=nullptr;inline SetBlockFunction setBlockOriginal=nullptr;
// Extra LevelTickEvent subscriber (Freecam); hook #249 is installed once.
inline void(*volatile levelTickListener)()=nullptr;
// Block ESP (game_block_esp.hpp): tick-thread scan and changed subchunks, like Chest ESP.
inline void(*blockEspTick)(void*)=nullptr;inline void(*blockEspSetBlock)(uint64_t)=nullptr;
// Bed Breaker (game_bed_breaker.hpp): subchunk, layer and index of the changed block.
inline void(*volatile bedSetBlock)(uint64_t,uint8_t,uint16_t)=nullptr;
inline void(*volatile regenSetBlock)(uint64_t,uint8_t,uint16_t)=nullptr;   // Regen (game_player_c.hpp)
// Base Finder (game_base_finder.hpp): changed subchunk and new block (Chunk Ages).
inline void(*volatile baseSetBlock)(uint64_t,uint64_t)=nullptr;
inline void(*volatile schematicSetBlock)(uint64_t)=nullptr;
// Utility modules (Timer, ...): level tick subscribers.
inline void(*volatile utilityTick)()=nullptr;
inline thread_local void* currentTickLevel=nullptr;   // level of the current tick (client or local server)
// Singleplayer runs a client level and a local server level, each ticking on its own thread. Subscribers that act on
// the local player (rotation stack, Freecam, utility modules) run on the client level tick only, as on a remote
// server; when the level cannot be told (no player, read failure) they run, as before.
inline bool ClientTickLevel(void* level){Context c{};uint64_t own{};if(!level||!Local(&c)||!Read(c.actor+0x200,&own)||!own)return true;return own==(uint64_t)level;}
// A new client level is a new world for the fault boundaries (game_fault.hpp): stopped sites get one more chance, also
// when the StartGame hook (game_events.hpp) is not installed because no module needs it.
inline void NoteClientLevel(void* level){static void* last=nullptr;Context c{};uint64_t own{};if(level==last||!Local(&c)||!Read(c.actor+0x200,&own)||own!=(uint64_t)level)return;last=level;FeatureNewWorld();}
inline uint64_t TickDetour(void* level){currentTickLevel=level;NoteClientLevel(level);if(ClientTickLevel(level)){auto rp=rotationPush;if(rp)rp(1);if(auto listener=levelTickListener)listener();if(auto u=utilityTick)u();if(rp)if(auto pop=rotationPop)pop();}if(auto esp=blockEspTick)esp(level);if(!Map().fault)MapGuarded(level);if(!Chests().fault)ChestGuarded(level);if(!Sus().fault)SusGuarded(level);return tickOriginal(level);}
inline uint64_t SetBlockDetour(void* sub,uint8_t layer,uint16_t index,void* block){const uint64_t result=setBlockOriginal(sub,layer,index,block);if(auto esp=blockEspSetBlock)esp((uint64_t)sub);if(auto bed=bedSetBlock)bed((uint64_t)sub,layer,index);if(auto rg=regenSetBlock)rg((uint64_t)sub,layer,index);if(auto bf=baseSetBlock)bf((uint64_t)sub,(uint64_t)block);if(auto ss=schematicSetBlock)ss((uint64_t)sub);auto& shared=Map();if(shared.enabled&&!shared.fault){std::lock_guard<std::mutex> lock(shared.pendingGuard);if(shared.pending.size()<4096)shared.pending.push_back((uint64_t)sub);}
 auto& chests=Chests();if(chests.enabled&&!chests.fault){std::lock_guard<std::mutex> lock(chests.pendingGuard);if(chests.pending.size()<4096)chests.pending.push_back((uint64_t)sub);}
 auto& sus=Sus();if(sus.enabled&&!sus.fault){std::lock_guard<std::mutex> lock(sus.pendingGuard);if(sus.pending.size()<4096)sus.pending.push_back((uint64_t)sub);}return result;}
// 1: hooks installed; -1: refused. Attempted once.
inline int InstallMapHooks(void(*log)(const char*)){
 static int state=0;if(state)return state;state=-1;uintptr_t base{};uint32_t size{};if(!Supported(&base,&size)){if(log)log("MiniMap: not the supported Minecraft image, no terrain hook installed");return state;}
 struct Hook{uint32_t rva;uint8_t bytes[16];void* detour;void** original;};
 const Hook hooks[2]={
  {0x2323c10,{0x55,0x56,0x57,0x53,0x48,0x81,0xec,0xb8,0x00,0x00,0x00,0x48,0x8d,0xac,0x24,0x80},(void*)TickDetour,(void**)&tickOriginal},
  {0x5cf63d0,{0x55,0x41,0x57,0x41,0x56,0x41,0x55,0x41,0x54,0x56,0x57,0x53,0x48,0x81,0xec,0x88},(void*)SetBlockDetour,(void**)&setBlockOriginal}};
 for(auto& hook:hooks)for(int i=0;i<16;++i){uint8_t byte{};if(!Read(base+hook.rva+i,&byte)||byte!=hook.bytes[i]){if(log)log("MiniMap: unexpected bytes at a terrain hook target, no terrain hook installed");return state;}}
 // all or nothing: on a failure, hooks never enabled are removed and enabled ones are only disabled (their
 // trampolines stay valid for a thread that may already be inside the detour)
 int created=0,enabled=0;for(auto& hook:hooks){if(MH_CreateHook((void*)(base+hook.rva),hook.detour,hook.original)!=MH_OK)break;++created;}
 if(created==2)for(auto& hook:hooks){if(MH_EnableHook((void*)(base+hook.rva))!=MH_OK)break;++enabled;}
 if(enabled!=2){for(int i=0;i<enabled;++i)MH_DisableHook((void*)(base+hooks[i].rva));for(int i=enabled;i<created;++i)MH_RemoveHook((void*)(base+hooks[i].rva));
  if(log)log("MiniMap: MinHook could not install every hook, none left active");return state;}
 if(log)log("MiniMap: level tick and block change hooks installed");state=1;return state;
}
}
