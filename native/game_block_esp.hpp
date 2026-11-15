// Block ESP: highlights listed blocks through walls using the game's own block tessellator.
// - Scan on the tick thread (same skeleton as Chest ESP): range = render distance x 16 + 16, budget = Scan
// Speed ms, changed subchunks (hook #251) first, then round-robin. Selection sorts subchunks by distance
// to the player and accumulates up to Max Blocks (stops at the first that overflows; unlimited when Limit
// Blocks is off).
// - Meshing on the LevelRenderPrep thread (#21): uses the game's 20^3 BlockTessellator cache (LevelRenderer
// + 0x6b0) with only the kept blocks (everything else as air), one mesh per render layer, at most 2 ms of
// building per frame; meshes are kept until the subchunk changes.
// - Drawing in highlight slot 8 + style: world matrix translated by (subchunk origin - camera),
// moving_block* materials and the level's block atlas. Block entities are drawn too; tracers are drawn in
// menu.cpp.
#pragma once
#include "game_highlight.hpp"
#include "game_map.hpp"
#include "game_world.hpp"
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>

namespace game {
struct BeBlock{int8_t x,y,z;uint8_t style;uint64_t block;};   // style: 0 = default, 1..7 = custom style (highlight slot 8 + style)
struct BeSub{int x,y,z;uint64_t version;std::vector<BeBlock> blocks;std::vector<BeBlock> actors;};   // actors: blocks that carry a block entity
struct BeConfig{bool enabled=false;std::vector<std::string> names;std::vector<uint8_t> styleOf;bool limit=true;int maxBlocks=4096;float scanMs=1;};
struct BeShared{SRWLOCK lock=SRWLOCK_INIT;BeConfig config;uint64_t configVersion=0;std::shared_ptr<const std::vector<BeSub>> selected;uint64_t region=0;
 volatile long fault=0,renderFault=0,built=0,drawn=0,items=0,noBits=0,emptyMesh=0;std::mutex pendingGuard;std::vector<uint64_t> pending;void(*log)(const char*)=nullptr;};
inline BeShared& BlockEsp(){static BeShared* s=new BeShared;return *s;}

// ---------------------------------------------------------------- scan (tick thread)
struct BeSubState{uint64_t layer0=0,layer1=0,size0=0,size1=0,version=0;std::vector<BeBlock> found;};
struct BeChunk{uint64_t chunk=0,begin=0;uint32_t count=0;bool read=false;uint64_t seen=0;int x=0,z=0;std::vector<BeSubState> subs;};
struct BeState{uint64_t level=0,region=0,tick=0,configVersion=~0ull,serial=0;int range=-1,lastChunkX=1<<30,lastChunkZ=1<<30,lastMax=-2;size_t robin=0;bool dirty=true;
 std::vector<std::pair<int,int>> offsets;std::unordered_map<uint64_t,BeChunk> tracked;std::unordered_map<uint64_t,uint8_t> named;std::unordered_map<std::string,uint8_t> names;};
// 0: not listed; otherwise 1 + style index.
inline uint8_t BeMatch(BeState& st,uint64_t block){if(!block)return 0;auto f=st.named.find(block);if(f!=st.named.end())return f->second;uint64_t legacy{};char name[96]{};uint8_t m=0;
 if(Read(block+0x68,&legacy)&&legacy&&Text(legacy+0xe8,name,sizeof(name))){auto n=st.names.find(name);if(n!=st.names.end())m=(uint8_t)(1+n->second);}if(st.named.size()<65536)st.named.emplace(block,m);return m;}
inline uint64_t BePalette(const Context& c,uint64_t storage){if(!storage)return 0;const uint64_t get=Virtual(c,storage,0xa8);if(!get)return 0;uint64_t out[2]{};reinterpret_cast<void*(*)(uint64_t,uint64_t*)>(get)(storage,out);return out[0];}
// Layers 0 then 1: local positions + Block*.
inline void BeScanSub(const Context& c,BeState& st,BeSubState& sub,uint64_t address){
 std::vector<BeBlock> before;before.swap(sub.found);sub.layer0=sub.layer1=sub.size0=sub.size1=0;
 if(Read(address+0x30,&sub.layer0)&&Read(address+0x38,&sub.layer1)){sub.size0=BePalette(c,sub.layer0);sub.size1=BePalette(c,sub.layer1);
  for(int layer=0;layer<2;++layer){const uint64_t storage=layer?sub.layer1:sub.layer0;if(!storage)continue;
   const uint64_t palette=Virtual(c,storage,0xa8),bitsOf=Virtual(c,storage,0xa0),wordsOf=Virtual(c,storage,0x90);if(!palette||!bitsOf||!wordsOf)continue;
   uint64_t out[2]{};reinterpret_cast<void*(*)(uint64_t,uint64_t*)>(palette)(storage,out);if(!out[0]||!out[1])continue;const uint32_t entries=(uint32_t)std::min<uint64_t>(out[0],4096);
   static uint64_t blocks[4096];static uint8_t match[4096];bool any=false;for(uint32_t i=0;i<entries;++i){blocks[i]=0;match[i]=Read(out[1]+8ull*i,&blocks[i])&&BeMatch(st,blocks[i]);any=any||match[i];}if(!any)continue;
   auto add=[&](uint32_t at,uint32_t pick){sub.found.push_back({(int8_t)(at>>8),(int8_t)(at&15),(int8_t)((at>>4)&15),(uint8_t)(match[pick]-1),blocks[pick]});};
   const int bits=reinterpret_cast<int(*)(uint64_t)>(bitsOf)(storage);
   if(bits<=0){if(match[0])for(uint32_t at=0;at<4096;++at)add(at,0);continue;}if(bits>32)continue;
   uint64_t words[2]{};reinterpret_cast<void*(*)(uint64_t,uint64_t*)>(wordsOf)(storage,words);if(!words[0]||!words[1])continue;const uint32_t per=32/bits,mask=bits==32?0xffffffffu:((1u<<bits)-1);
   for(uint32_t at=0;at<4096;++at){const uint32_t word=at/per;if(word>=words[0])break;uint32_t value{};if(!Read(words[1]+4ull*word,&value))break;const uint32_t pick=(value>>((at%per)*bits))&mask;if(pick<entries&&match[pick])add(at,pick);}}}
 bool same=before.size()==sub.found.size();for(size_t i=0;same&&i<before.size();++i)same=before[i].x==sub.found[i].x&&before[i].y==sub.found[i].y&&before[i].z==sub.found[i].z&&before[i].block==sub.found[i].block&&before[i].style==sub.found[i].style;
 if(!same){static uint64_t counter=0;sub.version=++counter;st.dirty=true;}}
inline void BeReset(BeState& st){st.tracked.clear();st.named.clear();st.robin=0;st.dirty=true;auto& s=BlockEsp();AcquireSRWLockExclusive(&s.lock);s.selected.reset();ReleaseSRWLockExclusive(&s.lock);}
inline void BeTickWork(void* levelPointer){
 static BeState* kept=new BeState;BeState& st=*kept;auto& s=BlockEsp();BeConfig cfg;uint64_t cfgVersion;
 AcquireSRWLockShared(&s.lock);cfg=s.config;cfgVersion=s.configVersion;ReleaseSRWLockShared(&s.lock);
 if(!cfg.enabled){if(st.level||!st.tracked.empty()){st.level=0;st.region=0;BeReset(st);}return;}
 if(cfgVersion!=st.configVersion){st.configVersion=cfgVersion;st.names.clear();for(size_t i=0;i<cfg.names.size();++i)st.names[cfg.names[i]]=i<cfg.styleOf.size()?cfg.styleOf[i]:0;st.tracked.clear();st.named.clear();st.dirty=true;}   // list changed: rescan everything
 LARGE_INTEGER frequency{},start{};QueryPerformanceFrequency(&frequency);QueryPerformanceCounter(&start);const double budget=std::clamp(cfg.scanMs,.5f,10.f);
 auto late=[&]{LARGE_INTEGER now{};QueryPerformanceCounter(&now);return (now.QuadPart-start.QuadPart)*1000.0/frequency.QuadPart>budget;};
 const uint64_t level=(uint64_t)levelPointer;Context c{};if(!Local(&c)){if(st.level){st.level=0;st.region=0;BeReset(st);}return;}
 const uint64_t guard=Virtual(c,level,0x9e8);if(!guard||!reinterpret_cast<bool(*)(uint64_t)>(guard)(level))return;
 uint64_t owner{},region{},where{};uintptr_t element{};int16_t lowest{};float feet[3],lift{};
 if(!Read(c.actor+0x1f0,&owner)||!owner)return;const uint64_t regionOf=Virtual(c,owner,0x68);if(!regionOf)return;region=reinterpret_cast<uint64_t(*)(uint64_t)>(regionOf)(owner);if(!region||!Read(region+0x3a,&lowest))return;
 if(level!=st.level||region!=st.region){st.level=level;st.region=region;BeReset(st);}
 if(!Read(c.actor+0x240,&where)||!where||!Floats(where,feet,3))return;if(Element(c.registry,c.entity,"OffsetsComponent",0x44,&element)&&Floats(element,&lift,1))feet[1]-=lift;
 const int bx=(int)std::floor(feet[0]),bz=(int)std::floor(feet[2]),minimumY=lowest;
 int distance=8;{const uint64_t options=GameOptions(c);if(options)distance=reinterpret_cast<int(*)(uint64_t)>(c.base+Rva(0x5b6f550))(options);}if(distance<2||distance>96)distance=8;
 const int range=distance*16+16;
 if(range!=st.range){st.range=range;st.offsets.clear();const int n=range/16+1;auto d=[](int v){const int a=v<0?-v:v;return (std::max(a,1)-1)*16;};
  for(int dx=-n;dx<=n;++dx)for(int dz=-n;dz<=n;++dz)if((unsigned)(d(dx)*d(dx)+d(dz)*d(dz))<=(unsigned)(range*range))st.offsets.push_back({dx,dz});
  std::stable_sort(st.offsets.begin(),st.offsets.end(),[](const std::pair<int,int>& a,const std::pair<int,int>& b){return a.first*a.first+a.second*a.second<b.first*b.first+b.second*b.second;});}
 const uint64_t chunkAt=Virtual(c,region,0x148);if(!chunkAt)return;++st.tick;std::vector<uint64_t> visible;visible.reserve(st.offsets.size());
 for(auto& o:st.offsets){int position[2]={(bx>>4)+o.first,(bz>>4)+o.second};const uint64_t chunk=reinterpret_cast<uint64_t(*)(uint64_t,int*)>(chunkAt)(region,position);if(!chunk)continue;
  uint64_t begin{},end{};if(!Read(chunk+0x1c8,&begin)||!Read(chunk+0x1d0,&end)||end<begin)continue;const uint32_t count=(uint32_t)std::min<uint64_t>((end-begin)/0x68,64);const uint64_t key=((uint64_t)(uint32_t)position[0]<<32)|(uint32_t)position[1];BeChunk& r=st.tracked[key];
  if(r.chunk!=chunk||r.begin!=begin||r.count!=count){r.chunk=chunk;r.begin=begin;r.count=count;r.read=false;r.x=position[0];r.z=position[1];r.subs.assign(count,BeSubState{});st.dirty=true;}r.seen=st.tick;visible.push_back(key);}
 for(auto it=st.tracked.begin();it!=st.tracked.end();){if(it->second.seen==st.tick)++it;else{it=st.tracked.erase(it);st.dirty=true;}}
 auto scan=[&](BeChunk& r,uint32_t i){BeScanSub(c,st,r.subs[i],r.begin+0x68ull*i);};
 {std::vector<uint64_t> changed;{std::lock_guard<std::mutex> lock(s.pendingGuard);changed.swap(s.pending);}
  if(!changed.empty()){std::sort(changed.begin(),changed.end());changed.erase(std::unique(changed.begin(),changed.end()),changed.end());
   for(uint64_t key:visible){BeChunk& r=st.tracked[key];if(!r.read)continue;for(uint64_t a:changed)if(a>=r.begin&&a<r.begin+0x68ull*r.count&&(a-r.begin)%0x68==0)scan(r,(uint32_t)((a-r.begin)/0x68));}}}
 for(uint64_t key:visible){if(late())break;BeChunk& r=st.tracked[key];if(r.read)continue;for(uint32_t i=0;i<r.count;++i)scan(r,i);r.read=true;}
 if(!visible.empty())for(size_t turn=0;turn<visible.size()&&!late();++turn){BeChunk& r=st.tracked[visible[st.robin++%visible.size()]];if(!r.read)continue;
  for(uint32_t i=0;i<r.count;++i){const uint64_t a=r.begin+0x68ull*i;uint64_t l0{},l1{};if(!Read(a+0x30,&l0)||!Read(a+0x38,&l1))continue;BeSubState& sub=r.subs[i];
   if(l0!=sub.layer0||l1!=sub.layer1||BePalette(c,l0)!=sub.size0||BePalette(c,l1)!=sub.size1)scan(r,i);}}
 // Selection (only when the list, the player's chunk, Max Blocks or the results changed): sort by centre
 // distance.
 const int maxBlocks=cfg.limit?cfg.maxBlocks:-1;
 if(!st.dirty&&(bx>>4)==st.lastChunkX&&(bz>>4)==st.lastChunkZ&&maxBlocks==st.lastMax)return;
 st.dirty=false;st.lastChunkX=bx>>4;st.lastChunkZ=bz>>4;st.lastMax=maxBlocks;
 struct Cand{float d;const BeChunk* r;uint32_t i;};std::vector<Cand> all;
 for(auto& e:st.tracked)for(uint32_t i=0;i<e.second.subs.size();++i)if(!e.second.subs[i].found.empty()){const float cx=e.second.x*16+8-feet[0],cy=minimumY+16.f*i+8-feet[1],cz=e.second.z*16+8-feet[2];all.push_back({cx*cx+cy*cy+cz*cz,&e.second,i});}
 std::sort(all.begin(),all.end(),[](const Cand& a,const Cand& b){return a.d<b.d;});
 auto out=std::make_shared<std::vector<BeSub>>();size_t total=0;
 const uint64_t entityAt=Virtual(c,region,0x18);
 for(auto& cnd:all){const BeSubState& sub=cnd.r->subs[cnd.i];total+=sub.found.size();if(maxBlocks>=0&&total>(size_t)maxBlocks)break;BeSub out1{cnd.r->x*16,minimumY+16*(int)cnd.i,cnd.r->z*16,sub.version,sub.found,{}};
  if(entityAt)for(const auto& b:sub.found){int pos[3]={out1.x+b.x,out1.y+b.y,out1.z+b.z};if(reinterpret_cast<uint64_t(*)(uint64_t,int*)>(entityAt)(region,pos))out1.actors.push_back(b);}
  out->push_back(std::move(out1));}
 AcquireSRWLockExclusive(&s.lock);s.selected=out;s.region=region;ReleaseSRWLockExclusive(&s.lock);}
inline void BeTick(void* level){if(BlockEsp().fault)return;__try{BeTickWork(level);}__except(EXCEPTION_EXECUTE_HANDLER){InterlockedExchange(&BlockEsp().fault,1);}}
inline void BeSetBlock(uint64_t sub){auto& s=BlockEsp();if(!s.config.enabled||s.fault)return;std::lock_guard<std::mutex> lock(s.pendingGuard);if(s.pending.size()<4096)s.pending.push_back(sub);}

// ---------------------------------------------------------------- meshing and drawing (LevelRenderPrep
// thread)
struct BeMesh{int style;uint8_t layer;uint8_t* mesh;};
struct BeMeshRec{uint64_t version=~0ull;uint64_t serial=0;std::vector<BeMesh> meshes;};
struct BeRenderState{std::unordered_map<uint64_t,BeMeshRec> subs;uint64_t materials[17][2]{};bool materialsReady=false;uint64_t serial=0;};
inline BeRenderState& BeRender(){static BeRenderState* r=new BeRenderState;return *r;}
inline uint64_t BeKey(int x,int y,int z){return ((uint64_t)(uint32_t)(x>>4)&0x1fffff)<<42|((uint64_t)(uint32_t)(y>>4)&0x1fffff)<<21|((uint64_t)(uint32_t)(z>>4)&0x1fffff);}
inline void BeFreeMesh(const Context& c,uint8_t* mesh){if(!mesh)return;reinterpret_cast<void(*)(void*)>(c.base+Rva(0x4dea990))(mesh);::operator delete(mesh);}
// Materials per render layer, HashedString {FNV-1 64, std::string, 0}.
inline void BeMaterials(const Context& c){auto& r=BeRender();if(r.materialsReady)return;r.materialsReady=true;
 static const char* names[17]={"moving_block_double_side","moving_block","moving_block","moving_block_alpha","moving_block_alpha","moving_block","moving_block","moving_block_alpha","moving_block_alpha","moving_block_seasons","moving_block_alpha_seasons","moving_block_alpha_single_side","moving_block_alpha_single_side","moving_block","moving_block","moving_block","moving_block"};
 const uint64_t factory=c.base+Rva(0x129c8110);const uint64_t get=Virtual(c,factory,8);if(!get){r.materialsReady=false;return;}   // the factory object itself lives at MC+0x129c8110
 struct Hashed{uint64_t hash;std::string text;uint64_t last;};
 for(int i=0;i<17;++i){Hashed h{0xcbf29ce484222325ull,names[i],0};for(const char* p=names[i];*p;++p)h.hash=(h.hash*0x100000001b3ull)^(uint8_t)*p;   // FNV-1, not FNV-1a
  uint64_t out[2]{};reinterpret_cast<void(*)(uint64_t,uint64_t*,Hashed*)>(get)(factory,out,&h);r.materials[i][0]=out[0];r.materials[i][1]=out[1];}}
// Builds one subchunk's meshes.
inline void BeBuild(const Context& c,BeMeshRec& rec,const BeSub& sub,uint64_t screen,uint64_t btess,uint64_t region,uint32_t extraFlags=0){
 for(auto& m:rec.meshes)BeFreeMesh(c,m.mesh);rec.meshes.clear();
 uint64_t tess{};if(!Read(screen+0xb0,&tess)||!tess)return;const uint64_t cache=btess+0x6b0;uint64_t air{};if(!Read(cache,&air))return;
 const uint32_t meshFlags=reinterpret_cast<uint32_t(*)()>(c.base+Rva(0x5ad68f0))();
 *(int32_t*)(cache+8)=sub.x-2;*(int32_t*)(cache+0xc)=sub.y-2;*(int32_t*)(cache+0x10)=sub.z-2;memset((void*)(cache+0x20),0xff,16000);
 uint64_t* grid=(uint64_t*)(cache+0x3ea0);uint64_t* grid2=(uint64_t*)(cache+0x138a0);for(int i=0;i<8000;++i){grid[i]=air;grid2[i]=air;}memset((void*)(cache+0x232a0),0,0x1f400);
 for(const auto& b:sub.blocks)grid[b.z+(b.x*20+b.y)*20+0x34a]=b.block;
 std::vector<BeBlock> order(sub.blocks);std::stable_sort(order.begin(),order.end(),[](const BeBlock& a,const BeBlock& b){return a.style<b.style;});
 // Group by style (stable sort), then consecutive runs of the same render layer.
 const auto layerOf=[&](uint64_t block){return reinterpret_cast<uint8_t(*)(uint64_t)>(c.base+Rva(0x6a3a570))(block);};
 float* offset=(float*)(tess+0x1cc);const float saved[3]={offset[0],offset[1],offset[2]};
 for(size_t i=0;i<order.size();){const uint8_t layer=layerOf(order[i].block),style=order[i].style;size_t j=i+1;while(j<order.size()&&order[j].style==style&&layerOf(order[j].block)==layer)++j;
  reinterpret_cast<void(*)(uint64_t,int,int,int,bool)>(c.base+Rva(0x54175a0))(tess,0,1,(int)((j-i)*0x18),false);
  offset[0]=saved[0]-(float)sub.x;offset[1]=saved[1]-(float)sub.y;offset[2]=saved[2]-(float)sub.z;*(uint8_t*)(tess+0x255)=1;*(int32_t*)(btess+0x80)=layer;
  for(size_t k=i;k<j;++k){int32_t pos[3]={sub.x+order[k].x,sub.y+order[k].y,sub.z+order[k].z};reinterpret_cast<void(*)(uint64_t,uint64_t,uint64_t,int32_t*,bool)>(c.base+Rva(0x681b6d0))(btess,tess,order[k].block,pos,false);}
  *(uint8_t*)(tess+0x255)=0;offset[0]=saved[0];offset[1]=saved[1];offset[2]=saved[2];
  uint8_t* mesh=(uint8_t*)::operator new(0x260);memset(mesh,0,0x260);struct View{const char* p;uint64_t n;} name{"Block ESP Mask",14};
  reinterpret_cast<void(*)(uint64_t,void*,int,View*,uint16_t)>(c.base+Rva(0x5418de0))(tess,mesh,0,&name,(uint16_t)(meshFlags|extraFlags));
  rec.meshes.push_back({style,layer,mesh});InterlockedIncrement(&BlockEsp().built);i=j;}
 (void)region;}
inline float* BeTop(uint64_t deque){uint64_t size{},off{},map{},mapSize{};if(!Read(deque+0x20,&size)||!size||!Read(deque+0x18,&off)||!Read(deque+8,&map)||!Read(deque+0x10,&mapSize)||!mapSize)return nullptr;uint64_t p{};return Read(map+8*((off+size-1)&(mapSize-1)),&p)?(float*)p:nullptr;}
// Draws every mesh of one style.
inline void BeDraw(const Context& c,uint64_t screen,uint64_t renderer,const std::vector<BeSub>& subs,const float camera[3],int style){
 auto& r=BeRender();uint64_t stack{};if(!Read(screen+0x18,&stack)||!stack)return;const uint64_t world=stack+0x40;float* top=BeTop(world);
 uint64_t tex[2]{};if(!Read(renderer+0x1d0,&tex[0]))return;const uint64_t getter=Virtual(c,c.client,0x5f0);uint64_t holder{};if(!top||!getter||!CallGetter(getter,c.client,&holder)||!holder||!tex[0]||!Read(holder+0x18,&tex[1])||!tex[1])return;
 const uint8_t saved7b=*(uint8_t*)(screen+0x7b);*(uint8_t*)(screen+0x7b)=1;uint64_t colorObj{};Read(screen+0x30,&colorObj);float savedColor[4]{1,1,1,1};
 if(colorObj){memcpy(savedColor,(void*)colorObj,16);const float white[4]{1,1,1,1};memcpy((void*)colorObj,white,16);*(uint8_t*)(colorObj+0x10)=1;}
 float M[16];memcpy(M,top,64);
 for(const auto& sub:subs){auto f=r.subs.find(BeKey(sub.x,sub.y,sub.z));if(f==r.subs.end())continue;bool placed=false;
  for(const auto& m:f->second.meshes){if(m.style!=style||!m.mesh)continue;
   if(!placed){const float x=(float)sub.x-camera[0],y=(float)sub.y-camera[1],z=(float)sub.z-camera[2];memcpy(top,M,48);
    top[12]=x*M[0]+y*M[4]+z*M[8]+M[12];top[13]=x*M[1]+y*M[5]+z*M[9]+M[13];top[14]=x*M[2]+y*M[6]+z*M[10]+M[14];top[15]=x*M[3]+y*M[7]+z*M[11]+M[15];*(uint8_t*)(world+0x38)=1;placed=true;}
   struct Span{uint64_t n;uint64_t* p;} span{2,tex};uint8_t zero[0x58]{};const uint8_t layer=m.layer>0x10?5:m.layer;
   reinterpret_cast<void(*)(void*,uint64_t,void*,Span*,uint64_t,uint64_t,void*)>(c.base+Rva(0x4deffe0))(m.mesh,screen+0x10,r.materials[layer],&span,0,0,zero);InterlockedIncrement(&BlockEsp().drawn);}}
 memcpy(top,M,64);*(uint8_t*)(world+0x38)=1;*(uint8_t*)(screen+0x7b)=saved7b;
 // Block entities: render context (game RVA 0x57e2560), dispatcher at *(ctx+0x30); each entity is drawn with
 // RVA 0x6a5df90 when the dispatcher has a renderer for be->vt[0x20](); the context ends with RVA 0x57e29e0.
 {uint64_t region{},owner{};if(Read(c.actor+0x1f0,&owner)&&owner){const uint64_t regionOf=Virtual(c,owner,0x68);if(regionOf)region=reinterpret_cast<uint64_t(*)(uint64_t)>(regionOf)(owner);}
  const uint64_t entityAt=region?Virtual(c,region,0x18):0;alignas(16) static uint8_t ctx[0x300];bool open=false;
  uint64_t game{},first{},second{},third{};const bool chain=Read(c.base+Rva(0x128c0f90),&game)&&game&&Read(game+8,&first)&&first&&Read(first+0x20,&second)&&second&&Read(second+0x48,&third)&&third;
  if(entityAt&&chain)for(const auto& sub:subs)for(const auto& b:sub.actors){if(b.style!=style)continue;int pos[3]={sub.x+b.x,sub.y+b.y,sub.z+b.z};uint64_t be=reinterpret_cast<uint64_t(*)(uint64_t,int*)>(entityAt)(region,pos);if(!be)continue;
   const uint64_t inner=Virtual(c,be,0xa8);if(!inner)continue;be=reinterpret_cast<uint64_t(*)(uint64_t)>(inner)(be);if(!be)continue;
   if(!open){memset(ctx,0,sizeof ctx);reinterpret_cast<void(*)(void*,uint64_t,uint64_t,uint64_t)>(c.base+Rva(0x57e2560))(ctx,screen,c.client,third);open=true;}
   const uint64_t dispatcher=*(uint64_t*)(ctx+0x30);const uint64_t typeOf=Virtual(c,be,0x20);if(!dispatcher||!typeOf)continue;const uint8_t type=reinterpret_cast<uint8_t(*)(uint64_t)>(typeOf)(be);uint64_t slot{};if(!Read(dispatcher+8ull*type,&slot)||!slot)continue;
   float rel[3]={(float)pos[0]-camera[0],(float)pos[1]-camera[1],(float)pos[2]-camera[2]};uint8_t zero[0x48]{};
   reinterpret_cast<void(*)(uint64_t,void*,uint64_t,uint64_t,uint64_t,float*,int*,uint8_t,uint64_t,uint64_t,uint32_t,void*)>(c.base+Rva(0x6a5df90))(dispatcher,ctx,region,be,b.block,rel,pos,0,c.base+Rva(0x128d9288),0,0,zero);}
  if(open)reinterpret_cast<void(*)(void*)>(c.base+Rva(0x57e29e0))(ctx);}
 if(colorObj){memcpy((void*)colorObj,savedColor,16);*(uint8_t*)(colorObj+0x10)=1;}}
inline void BeRenderWork(void* screenPtr,void* rendererPtr){
 auto& s=BlockEsp();auto& r=BeRender();Context c{};const bool on=s.config.enabled&&!s.fault;
 if(!on){if(!r.subs.empty()&&Client(&c)){for(auto& e:r.subs)for(auto& m:e.second.meshes)BeFreeMesh(c,m.mesh);r.subs.clear();}return;}
 if(!Local(&c))return;const uint64_t screen=(uint64_t)screenPtr,renderer=(uint64_t)rendererPtr;
 std::shared_ptr<const std::vector<BeSub>> subs;uint64_t region{};AcquireSRWLockShared(&s.lock);subs=s.selected;region=s.region;ReleaseSRWLockShared(&s.lock);if(!subs||subs->empty())return;
 uint64_t owner{},mine{};if(!Read(c.actor+0x1f0,&owner)||!owner)return;{const uint64_t regionOf=Virtual(c,owner,0x68);if(!regionOf)return;mine=reinterpret_cast<uint64_t(*)(uint64_t)>(regionOf)(owner);}if(!mine||mine!=region)return;
 uint64_t holder{};float camera[3];if(!Read(renderer+0x480,&holder)||!holder||!Floats(holder+0x658,camera,3))return;
 BeMaterials(c);const uint64_t btess=reinterpret_cast<uint64_t(*)(uint64_t)>(c.base+Rva(0x5353600))(renderer);if(!btess)return;
 // Meshing: at most 2 ms per frame; the BlockTessellator cache is restored afterwards.
 ++r.serial;LARGE_INTEGER f{},t0{};QueryPerformanceFrequency(&f);QueryPerformanceCounter(&t0);
 const uint64_t cache=btess+0x6b0;const int32_t savedLayer=*(int32_t*)(btess+0x80);const uint64_t savedRegion=*(uint64_t*)(cache+0x18);*(uint64_t*)(cache+0x18)=region;
 for(const auto& sub:*subs){BeMeshRec& rec=r.subs[BeKey(sub.x,sub.y,sub.z)];rec.serial=r.serial;if(rec.version==sub.version)continue;
  LARGE_INTEGER now{};QueryPerformanceCounter(&now);if((now.QuadPart-t0.QuadPart)*1000.0/f.QuadPart>2.0)continue;BeBuild(c,rec,sub,screen,btess,region);rec.version=sub.version;}
 *(uint64_t*)(cache+0x18)=savedRegion;*(int32_t*)(btess+0x80)=savedLayer;
 for(auto it=r.subs.begin();it!=r.subs.end();){if(it->second.serial!=r.serial){for(auto& m:it->second.meshes)BeFreeMesh(c,m.mesh);it=r.subs.erase(it);}else ++it;}
 // Mask of used styles (engine+0x58), then 8 passes: style s draws in highlight slot 8 + s.
 uint64_t mask=0;for(auto& e:r.subs)for(auto& m:e.second.meshes)mask|=1ull<<(m.style&7);for(const auto& sub:*subs)for(const auto& b:sub.actors)mask|=1ull<<(b.style&7);
 auto& h=Highlight();if(!HlActive()||HlOnThread(h.tid178))return;const uintptr_t scene=HlScene();if(!scene)return;
 for(int style=0;style<8;++style){if(!((mask>>style)&1))continue;uint8_t bits=0;uintptr_t list=0;uint64_t before=0;
  {std::lock_guard<std::mutex> lock(h.guard);bits=HlSlotBits(h,scene,8+style);list=HlItems(scene);before=HlCount(list);}if(!bits||!list){InterlockedIncrement(&s.noBits);continue;}
  const long saved17c=InterlockedExchange(&h.tid17c,(long)GetCurrentThreadId());BeDraw(c,screen,renderer,*subs,camera,style);InterlockedExchange(&h.tid17c,saved17c);
  {std::lock_guard<std::mutex> lock(h.guard);HlRecordRange(h,scene,list,before,bits,(uint8_t)(8+style));}
  InterlockedExchangeAdd(&s.items,(long)(HlCount(list)-before));for(uint64_t i=before;i<HlCount(list);++i){uint8_t* it=HlItem(list,i);if(it)*(uint32_t*)(it+4)=0;}}}
inline void BeRenderGuarded(void* screen,void* renderer){if(BlockEsp().renderFault)return;__try{BeRenderWork(screen,renderer);}__except(EXCEPTION_EXECUTE_HANDLER){InterlockedExchange(&BlockEsp().renderFault,1);InterlockedExchange(&BlockEsp().fault,1);if(BlockEsp().log)BlockEsp().log("Block ESP: a fault was caught while building or drawing block meshes; Block ESP is disabled until the game restarts");}}
inline void ConfigureBlockEsp(const BeConfig& cfg,void(*log)(const char*)){auto& s=BlockEsp();s.log=log;AcquireSRWLockExclusive(&s.lock);
 const bool changed=cfg.names!=s.config.names||cfg.styleOf!=s.config.styleOf;s.config=cfg;if(changed)++s.configVersion;ReleaseSRWLockExclusive(&s.lock);
 if(cfg.enabled){static bool wired=false;if(!wired){wired=true;blockEspTick=BeTick;blockEspSetBlock=BeSetBlock;levelPrepListener=BeRenderGuarded;}}}
}
