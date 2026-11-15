// Sus Chunk Finder: scans loaded chunks on the tick thread (hook #249), 2 ms per tick; results drawn by menu.cpp.
// - Chunks: disk dx^2+dz^2 <= Scan Radius^2 around the player's chunk, sorted by distance.
// - Palette classification (layer 0, cached per block) -> 32-bit code:
// byte 0 flags: 1 storage (storage block list or "shulker_box" suffix), 2 placed (placed block list),
// 4 sideways deepslate (minecraft:deepslate with NBT "states"/"pillar_axis" != y), 8 Types list, 0x10 structure;
// byte 1 solidity: 0 air (material *(BlockLegacy+0x28) == 0), 1 full ((byte +1 & 0x17) == 0x14), 2 other;
// bytes 2/3: category (0 trial chamber, 1 ancient city, 2 mineshaft, 3 stronghold, 4 dungeon) and weight
// (0x10 decisive, 1 otherwise).
// - Sub-chunk: mask 0x17 (Score) / 8 (Types) / 0x10 (Tunnels); blocks counted when y < 50 (Types: everywhere);
// 19x256 solidity grid (sub-chunk, layer y=15 below - missing = full -, layers y=0..1 above);
// tunnels (y < 50), flat rooms (sub-chunk and y < 20).
// - Chunk: sum; natural structure (one category >= 16) in the chunk OR one of its 4 neighbours.
// - Score; a newly flagged chunk prints "Sus chunk at X: {0} Z: {1} ({2})" through MC 0x6da9820,
// prefixed with "§7[§dNickel§7]".
#pragma once
#include "game_render.hpp"
#include "game_visual_entities.hpp"
#include <array>
#include <algorithm>
#include <optional>
#include <set>
#include <string>
#include <unordered_map>
#include <unordered_set>

namespace game {
struct SusConfig{int mode=0,radius=8,score=3,minCount=3,tunnelLength=8;bool storage=true,placed=true,rotated=true,flat=true,tunnels=true,skipNatural=true,notify=true;std::vector<std::string> types;
 bool operator==(const SusConfig& o)const{return mode==o.mode&&radius==o.radius&&score==o.score&&minCount==o.minCount&&tunnelLength==o.tunnelLength&&storage==o.storage&&placed==o.placed&&rotated==o.rotated&&flat==o.flat&&tunnels==o.tunnels&&skipNatural==o.skipNatural&&types==o.types;}};
struct SusStats{int v[15]{};SusStats(){v[13]=0x7fffffff;v[14]=(int)0x80000000;}};   // 0 storage 1 placed 2 sideways 3 types 4/5 room (size,y) 6/7 tunnel (length,y) 8..12 categories 13/14 min/max y
struct SusFlag{int cx,cz,reasons,score;SusStats stats;int lowY,highY;};
struct SusShared{SRWLOCK lock=SRWLOCK_INIT;volatile long enabled=0,fault=0,ticks=0,chunksRead=0,stage=0;SusConfig config;std::vector<SusFlag> flagged;std::mutex pendingGuard;std::vector<uint64_t> pending;};
inline SusShared& Sus(){static SusShared* shared=new SusShared;return *shared;}

struct SusSub{uint64_t layer0=0,layer1=0,size0=0,size1=0;SusStats stats;};
struct SusChunk{uint64_t chunk=0,begin=0;uint32_t count=0;bool read=false;uint64_t seen=0;int x=0,z=0;std::vector<SusSub> subs;};
struct SusState{uint64_t level=0,region=0,tick=0,player=0;SusConfig config;bool configured=false;size_t robin=0;int radius=-1;std::vector<std::pair<int,int>> offsets;
 std::unordered_map<uint64_t,SusChunk> tracked;std::unordered_map<uint64_t,uint32_t> codes;std::unordered_set<uint64_t> dirty;
 struct Agg{SusStats stats;bool natural=false;};std::unordered_map<uint64_t,Agg> agg;std::unordered_map<uint64_t,SusFlag> flagged;};
inline uint64_t SusKey(int x,int z){return ((uint64_t)(uint32_t)x<<32)|(uint32_t)z;}

// MSVC std::string read from game memory.
inline bool SusString(uint64_t at,std::string* out){uint64_t size{},cap{},data{};if(!Read(at+0x10,&size)||!Read(at+0x18,&cap)||size>256)return false;data=cap<16?at:0;if(cap>=16&&!Read(at,&data))return false;
 out->resize((size_t)size);for(size_t i=0;i<size;++i){char c{};if(!Read(data+i,&c))return false;(*out)[i]=c;}return true;}
// MSVC std::map<std::string, variant>: head *(compound+8), node: key +0x20, nil +0x19, value +0x40.
inline uint64_t SusTag(const Context& context,uint64_t compound,const char* key,uint8_t want){
 uint64_t head{},node{};if(!Read(compound+8,&head)||!head||!Read(head+8,&node))return 0;uint64_t found=head;std::string name;const std::string k=key;
 for(int guard=0;guard<64;++guard){uint8_t nil{};if(!Read(node+0x19,&nil)||nil)break;if(!SusString(node+0x20,&name))return 0;const bool less=name<k;if(!less)found=node;uint64_t next{};if(!Read(node+(less?0x10:0),&next))return 0;node=next;}
 if(found==head||!SusString(found+0x20,&name)||name!=k)return 0;const uint64_t tag=found+0x40;const uint64_t idOf=Virtual(context,tag,0x28);if(!idOf)return 0;
 return reinterpret_cast<uint8_t(*)(uint64_t)>(idOf)(tag)==want?tag:0;
}
// Classification of one block.
inline uint32_t SusClassify(const Context& context,const SusConfig& config,uint64_t block){
 static const char* storage[]={"minecraft:anvil","minecraft:beacon","minecraft:blast_furnace","minecraft:cartography_table","minecraft:chipped_anvil","minecraft:crafter","minecraft:damaged_anvil","minecraft:dropper","minecraft:enchanting_table","minecraft:ender_chest","minecraft:fletching_table","minecraft:grindstone","minecraft:jukebox","minecraft:lit_blast_furnace","minecraft:lit_smoker","minecraft:lodestone","minecraft:loom","minecraft:respawn_anchor","minecraft:smithing_table","minecraft:smoker","minecraft:stonecutter_block"};
 static const char* placed[]={"minecraft:barrel","minecraft:bed","minecraft:brewing_stand","minecraft:cauldron","minecraft:chest","minecraft:crafting_table","minecraft:dispenser","minecraft:furnace","minecraft:hopper","minecraft:lectern","minecraft:lit_furnace","minecraft:mob_spawner","minecraft:trapped_chest"};
 struct Natural{const char* name;uint8_t category;bool strong;};
 static const Natural exact[]={{"minecraft:trial_spawner",0,true},{"minecraft:vault",0,true},{"minecraft:polished_tuff",0,false},{"minecraft:chiseled_tuff",0,false},{"minecraft:reinforced_deepslate",1,true},
  {"minecraft:web",2,false},{"minecraft:rail",2,false},{"minecraft:oak_fence",2,false},{"minecraft:dark_oak_fence",2,false},{"minecraft:end_portal_frame",3,true},{"minecraft:iron_bars",3,false},
  {"minecraft:mossy_cobblestone",4,false},{"minecraft:stonebrick",3,false},{"minecraft:stone_bricks",3,false},{"minecraft:mossy_stone_bricks",3,false},{"minecraft:cracked_stone_bricks",3,false},{"minecraft:chiseled_stone_bricks",3,false}};
 uint64_t legacy{};char raw[96]{};if(!block||!Read(block+0x68,&legacy)||!legacy||!Text(legacy+0xe8,raw,sizeof(raw)))return 0;const std::string name=raw;
 uint8_t material{},bits{};Read(legacy+0x28,&material);Read(legacy+0x29,&bits);const uint32_t solidity=material==0?0:((bits&0x17)==0x14?1:2);
 uint32_t flags=0;for(auto s:storage)if(name==s)flags|=1;if(name.size()>10&&name.size()>=11&&name.compare(name.size()-11,11,"shulker_box")==0)flags|=1;
 for(auto p:placed)if(name==p)flags|=2;
 if(name=="minecraft:deepslate"){const uint64_t states=SusTag(context,block+0xf0,"states",10);const uint64_t axis=states?SusTag(context,states,"pillar_axis",8):0;std::string value;
  if(axis&&SusString(axis+8,&value)){if(value.size()==1){if(value[0]!='y')flags+=4;}else flags|=4;}}
 if(std::binary_search(config.types.begin(),config.types.end(),name))flags|=8;
 uint32_t code=0;bool natural=false;uint8_t category=0;bool strong=false;
 for(auto& e:exact)if(name==e.name){natural=true;category=e.category;strong=e.strong;break;}
 if(!natural){const size_t n=name.size();
  if(n>=10&&name.find("tuff_brick")!=std::string::npos){natural=true;category=0;}
  else if(n>=16&&name.find("minecraft:waxed_")!=std::string::npos){natural=true;category=0;}
  else if(n>=15&&name.find("deepslate_tiles")!=std::string::npos){natural=true;category=1;}
  else if(n>=16&&name.find("deepslate_bricks")!=std::string::npos){natural=true;category=1;}
  else if(n>=5&&name.find("sculk")!=std::string::npos){natural=true;category=1;}
  else if(n>=19&&name.compare(0,19,"minecraft:infested_")==0){natural=true;category=3;}}
 if(natural){flags|=0x10;code=((uint32_t)category<<16)|(strong?0x10000000u:0x01000000u);}
 return code|flags|(solidity<<8);
}
inline uint32_t SusCode(const Context& context,SusState& state,uint64_t block){auto f=state.codes.find(block);if(f!=state.codes.end())return f->second;const uint32_t c=SusClassify(context,state.config,block);if(state.codes.size()<65536)state.codes.emplace(block,c);return c;}
inline uint64_t SusPalette(const Context& context,uint64_t storage){if(!storage)return 0;const uint64_t get=Virtual(context,storage,0xa8);if(!get)return 0;uint64_t out[2]{};reinterpret_cast<void*(*)(uint64_t,uint64_t*)>(get)(storage,out);return out[0];}
// Palette codes and the 4096 indices of layer 0.
struct SusLayer{std::vector<uint32_t> codes;uint16_t index[4096];bool ok=false;};
inline bool SusReadLayer(const Context& context,SusState& state,uint64_t storage,SusLayer& out){
 out.ok=false;out.codes.clear();if(!storage)return false;const uint64_t palette=Virtual(context,storage,0xa8),bitsOf=Virtual(context,storage,0xa0),wordsOf=Virtual(context,storage,0x90);if(!palette||!bitsOf||!wordsOf)return false;
 uint64_t p[2]{};reinterpret_cast<void*(*)(uint64_t,uint64_t*)>(palette)(storage,p);if(!p[0]||!p[1])return false;const uint32_t entries=(uint32_t)std::min<uint64_t>(p[0],4096);out.codes.resize(entries);
 for(uint32_t i=0;i<entries;++i){uint64_t block{};out.codes[i]=Read(p[1]+8ull*i,&block)?SusCode(context,state,block):0;}
 const int bits=reinterpret_cast<int(*)(uint64_t)>(bitsOf)(storage);if(bits<=0){memset(out.index,0,sizeof(out.index));out.ok=true;return true;}if(bits>32)return false;
 uint64_t words[2]{};reinterpret_cast<void*(*)(uint64_t,uint64_t*)>(wordsOf)(storage,words);if(!words[0]||!words[1])return false;const uint32_t per=32/bits,mask=bits==32?0xffffffffu:((1u<<bits)-1);
 for(uint32_t at=0;at<4096;++at){const uint32_t word=at/per;uint32_t value{};out.index[at]=0xffff;if(word>=words[0]||!Read(words[1]+4ull*word,&value))continue;const uint32_t pick=(value>>((at%per)*bits))&mask;out.index[at]=pick<entries?(uint16_t)pick:0xffff;}
 out.ok=true;return true;
}
inline uint32_t SusAt(const SusLayer& layer,uint32_t at){const uint16_t i=layer.index[at];return i<layer.codes.size()?layer.codes[i]:0;}
// Solidity of a sub-chunk in the grid (layers first..last, offset shift); missing -> fill.
inline void SusFill(const Context& context,SusState& state,uint8_t* grid,uint64_t sub,int first,int last,int shift,uint8_t fill){
 uint64_t layer0{};if(!sub||!Read(sub+0x30,&layer0)||!layer0){memset(grid+(shift+first)*256+256,sub?0:fill,(size_t)(last-first)*256+256);return;}
 SusLayer layer;if(!SusReadLayer(context,state,layer0,layer)){memset(grid+(shift+first)*256+256,0,(size_t)(last-first)*256+256);return;}
 for(uint32_t at=0;at<4096;++at){const int y=at&15;if(y<first||y>last)continue;grid[(y+shift)*256+(at>>4)+256]=(uint8_t)((SusAt(layer,at)>>8)&0xff);}
}
// Tunnels: (length, layer).
inline std::pair<int,int> SusTunnel(const uint8_t* g){
 int best=0,bestLayer=0;
 for(int o=0;o<16;++o)for(int k=0;k<13;++k){int runA=0,runB=0;for(int j=0;j<16;++j){
   auto at=[&](int layer,int x,int z){return g[layer*256+x*16+z];};
   int a=0;{const int x=j,z=k+1;if(at(o,x,z)==1&&at(o+1,x,z)==0&&at(o+2,x,z)==0&&at(o+3,x,z)==1&&at(o+1,x,z-1)==1&&at(o+2,x,z-1)==1&&at(o+1,x,z+1)==1){a=runA+1;if(at(o+2,x,z+1)!=1)a=0;}}
   int b=0;{const int x=k+1,z=j;if(at(o,x,z)==1&&at(o+1,x,z)==0&&at(o+2,x,z)==0&&at(o+3,x,z)==1&&at(o+1,x-1,z)==1&&at(o+2,x-1,z)==1&&at(o+1,x+1,z)==1){b=runB+1;if(at(o+2,x+1,z)!=1)b=0;}}
   const int m=std::max(a,b);if(best<m){best=m;bestLayer=o;}runA=a;runB=b;}}
 return {best,bestLayer};
}
// (size, layer) of the largest flat rectangular room.
inline std::pair<int,int> SusRoom(const uint8_t* g){
 int best=0,bestLayer=0;
 for(int o=0;o<16;++o){bool cell[256];int count=0;for(int i=0;i<256;++i){cell[i]=g[o*256+i]==1&&g[(o+1)*256+i]==0&&g[(o+2)*256+i]==0;count+=cell[i];}if(count<=0x17)continue;
  for(int s=0;s<256;++s){if(!cell[s])continue;cell[s]=false;std::vector<int> stack{s};int size=0,minX=16,maxX=-1,minZ=16,maxZ=-1;
   while(!stack.empty()){const int c=stack.back();stack.pop_back();++size;const int x=c>>4,z=c&15;minX=std::min(minX,x);maxX=std::max(maxX,x);minZ=std::min(minZ,z);maxZ=std::max(maxZ,z);
    const int n[4][2]={{x-1,z},{x+1,z},{x,z-1},{x,z+1}};for(auto& v:n){if(v[0]<0||v[0]>15||v[1]<0||v[1]>15)continue;const int i=v[0]*16+v[1];if(cell[i]){cell[i]=false;stack.push_back(i);}}}
   if(size>0x17&&maxX-minX>1&&maxZ-minZ>1){if((maxX-minX)*75+75>0&&((maxX-minX)*75+75)*(maxZ-minZ+1)<=size*100&&best<size){best=size;bestLayer=o;}}}}
 return {best,bestLayer};
}
inline void SusRecordY(SusStats& s,int y){s.v[13]=std::min(s.v[13],y);s.v[14]=std::max(s.v[14],y);}
// Sub-chunk scan.
inline void SusScan(const Context& context,SusState& state,SusChunk& chunk,uint32_t i,int minimumY){
 SusSub& sub=chunk.subs[i];sub=SusSub{};const uint64_t address=chunk.begin+0x68ull*i;if(!Read(address+0x30,&sub.layer0)||!Read(address+0x38,&sub.layer1))return;
 sub.size0=SusPalette(context,sub.layer0);sub.size1=SusPalette(context,sub.layer1);if(!sub.layer0)return;
 SusLayer layer;if(!SusReadLayer(context,state,sub.layer0,layer))return;const int mode=state.config.mode;const uint8_t mask=mode==1?8:mode==2?0x10:0x17;const int subY=minimumY+16*(int)i;
 bool any=false,air=false,solid=false;for(uint32_t c:layer.codes){any=any||((c&mask)!=0);air=air||((c>>8&0xff)==0);solid=solid||((c>>8&0xff)==1);}
 if(any&&(subY<50||mode==1))for(uint32_t at=0;at<4096;++at){const uint32_t c=SusAt(layer,at);const uint8_t f=(uint8_t)(c&mask);if(!f)continue;const int y=(int)(at&15)+subY;
  if(f&8){++sub.stats.v[3];SusRecordY(sub.stats,y);}
  if(y<50){if(f>0xf)sub.stats.v[8+((c>>16)&0xff)%5]+=(int)(c>>24);if(f&7){sub.stats.v[0]+=f&1;sub.stats.v[1]+=(f>>1)&1;sub.stats.v[2]+=(f&4)?1:0;SusRecordY(sub.stats,y);}}}
 if(air&&solid&&mode!=1&&subY<50){static uint8_t grid[0x1300];memset(grid,0,sizeof(grid));
  SusFill(context,state,grid,i?address-0x68:0,15,15,-16,1);SusFill(context,state,grid,i+1<chunk.count?address+0x68:0,0,1,16,0);SusFill(context,state,grid,address,0,15,0,0);
  const auto tunnel=SusTunnel(grid);if(tunnel.second+subY<50){sub.stats.v[6]=tunnel.first;sub.stats.v[7]=tunnel.second+subY;}
  if(subY<20){const auto room=SusRoom(grid);if(room.second+subY<20){sub.stats.v[4]=room.first;sub.stats.v[5]=room.second+subY;}}}
}
inline void SusAdd(SusStats& a,const SusStats& b){for(int i=0;i<4;++i)a.v[i]+=b.v[i];if(a.v[4]<b.v[4]){a.v[4]=b.v[4];a.v[5]=b.v[5];}if(a.v[6]<b.v[6]){a.v[6]=b.v[6];a.v[7]=b.v[7];}for(int i=8;i<13;++i)a.v[i]+=b.v[i];a.v[13]=std::min(a.v[13],b.v[13]);a.v[14]=std::max(a.v[14],b.v[14]);}
inline bool SusNatural(const SusStats& s){for(int i=8;i<13;++i)if(s.v[i]>=0x10)return true;return false;}
// Chunk score.
inline bool SusEvaluate(const SusConfig& c,const SusStats& s,bool natural,int* reasons,int* score){
 natural=natural&&c.skipNatural;const int len=s.v[6];const bool tunnel=c.tunnelLength<=len&&0<len;*reasons=0;*score=0;
 if(c.mode==2){if(!tunnel||natural)return false;*reasons=0x10;*score=len;return true;}
 if(c.mode==1){if(s.v[3]<c.minCount)return false;*reasons=0x20;*score=s.v[3];return true;}
 int r=0,points=0;bool any=false;if(s.v[0]>0&&c.storage){r=1;points=3;any=true;}
 if(!natural){if(s.v[1]>0&&c.placed){r|=2;points+=1;any=true;}if(s.v[2]>1&&c.rotated){r|=4;points+=2;any=true;}if(s.v[4]>0&&c.flat){r|=8;points+=2;any=true;}if(tunnel&&c.tunnels){r|=0x10;points+=2;any=true;}}
 *reasons=r;*score=points;return c.score<=points&&any;
}
inline std::string SusReasons(const SusFlag& f){
 std::string out;auto add=[&](const char* fmt,int v){char b[64];snprintf(b,sizeof(b),fmt,v);if(!out.empty())out+=", ";out+=b;};
 if(f.reasons&1)add("player storage \xc3\x97%d",f.stats.v[0]);if(f.reasons&2)add("placed storage \xc3\x97%d",f.stats.v[1]);if(f.reasons&4)add("rotated deepslate \xc3\x97%d",f.stats.v[2]);
 if(f.reasons&8)add("flat room %d",f.stats.v[4]);if(f.reasons&0x10)add("tunnel %d",f.stats.v[6]);if(f.reasons&0x20)add("blocks \xc3\x97%d",f.stats.v[3]);return out;
}
inline void SusReset(SusState& state){state.tracked.clear();state.codes.clear();state.dirty.clear();state.agg.clear();state.flagged.clear();state.robin=0;auto& shared=Sus();AcquireSRWLockExclusive(&shared.lock);shared.flagged.clear();ReleaseSRWLockExclusive(&shared.lock);}
inline void SusWork(void* levelPointer){
 static SusState* kept=new SusState;SusState& state=*kept;auto& shared=Sus();
 if(!shared.enabled){if(state.level||!state.tracked.empty()){state.level=0;state.region=0;SusReset(state);}return;}
 LARGE_INTEGER frequency{},start{};QueryPerformanceFrequency(&frequency);QueryPerformanceCounter(&start);auto late=[&]{LARGE_INTEGER now{};QueryPerformanceCounter(&now);return (now.QuadPart-start.QuadPart)*1000.0/frequency.QuadPart>2.0;};
 const uint64_t level=(uint64_t)levelPointer;Context context{};if(!Local(&context)){if(state.level){state.level=0;state.region=0;SusReset(state);}return;}
 const uint64_t guard=Virtual(context,level,0x9e8);if(!guard||!reinterpret_cast<bool(*)(uint64_t)>(guard)(level))return;
 SusConfig config;AcquireSRWLockShared(&shared.lock);config=shared.config;ReleaseSRWLockShared(&shared.lock);std::sort(config.types.begin(),config.types.end());
 uint64_t owner{},region{},where{};uintptr_t element{};int16_t lowest{};float feet[3],lift{};
 if(!Read(context.actor+0x1f0,&owner)||!owner)return;const uint64_t regionOf=Virtual(context,owner,0x68);if(!regionOf)return;region=reinterpret_cast<uint64_t(*)(uint64_t)>(regionOf)(owner);if(!region||!Read(region+0x3a,&lowest))return;
 if(level!=state.level||region!=state.region||!state.configured||!(config==state.config)){state.level=level;state.region=region;state.config=config;state.configured=true;SusReset(state);}
 if(!Read(context.actor+0x240,&where)||!where||!Floats(where,feet,3))return;if(Element(context.registry,context.entity,"OffsetsComponent",0x44,&element)&&Floats(element,&lift,1))feet[1]-=lift;
 const int bx=(int)std::floor(feet[0]),by=(int)std::floor(feet[1]),bz=(int)std::floor(feet[2]),minimumY=lowest;
 if(config.radius!=state.radius){state.radius=config.radius;state.offsets.clear();const int r=config.radius;for(int dx=-r;dx<=r;++dx)for(int dz=-r;dz<=r;++dz)if((unsigned)(dx*dx+dz*dz)<=(unsigned)(r*r))state.offsets.push_back({dx,dz});
  std::stable_sort(state.offsets.begin(),state.offsets.end(),[](const std::pair<int,int>& a,const std::pair<int,int>& b){return a.first*a.first+a.second*a.second<b.first*b.first+b.second*b.second;});}
 const uint64_t chunkAt=Virtual(context,region,0x148);if(!chunkAt)return;++state.tick;std::vector<uint64_t> visible;
 for(auto& offset:state.offsets){int position[2]={(bx>>4)+offset.first,(bz>>4)+offset.second};const uint64_t chunk=reinterpret_cast<uint64_t(*)(uint64_t,int*)>(chunkAt)(region,position);if(!chunk)continue;
  uint64_t begin{},end{};if(!Read(chunk+0x1c8,&begin)||!Read(chunk+0x1d0,&end)||end<begin)continue;const uint32_t count=(uint32_t)std::min<uint64_t>((end-begin)/0x68,64);const uint64_t key=SusKey(position[0],position[1]);SusChunk& record=state.tracked[key];
  if(record.chunk!=chunk||record.begin!=begin||record.count!=count){record.chunk=chunk;record.begin=begin;record.count=count;record.read=false;record.x=position[0];record.z=position[1];record.subs.assign(count,SusSub{});}record.seen=state.tick;visible.push_back(key);}
 for(auto it=state.tracked.begin();it!=state.tracked.end();){if(it->second.seen==state.tick)++it;else{state.agg.erase(it->first);it=state.tracked.erase(it);}}
 {std::vector<uint64_t> changed;{std::lock_guard<std::mutex> lock(shared.pendingGuard);changed.swap(shared.pending);}
  if(!changed.empty()){std::sort(changed.begin(),changed.end());changed.erase(std::unique(changed.begin(),changed.end()),changed.end());
   for(uint64_t key:visible){SusChunk& record=state.tracked[key];if(!record.read)continue;for(uint64_t address:changed)if(address>=record.begin&&address<record.begin+0x68ull*record.count&&(address-record.begin)%0x68==0){SusScan(context,state,record,(uint32_t)((address-record.begin)/0x68),minimumY);state.dirty.insert(key);}}}}
 for(uint64_t key:visible){if(late())break;SusChunk& record=state.tracked[key];if(record.read)continue;for(uint32_t i=0;i<record.count;++i)SusScan(context,state,record,i,minimumY);record.read=true;state.dirty.insert(key);InterlockedIncrement(&shared.chunksRead);}
 if(!visible.empty())for(size_t turn=0;turn<visible.size()&&!late();++turn){const uint64_t key=visible[state.robin++%visible.size()];SusChunk& record=state.tracked[key];if(!record.read)continue;
  for(uint32_t i=0;i<record.count;++i){const uint64_t address=record.begin+0x68ull*i;uint64_t l0{},l1{};if(!Read(address+0x30,&l0)||!Read(address+0x38,&l1))continue;SusSub& sub=record.subs[i];
   if(l0!=sub.layer0||l1!=sub.layer1||SusPalette(context,l0)!=sub.size0||SusPalette(context,l1)!=sub.size1){SusScan(context,state,record,i,minimumY);state.dirty.insert(key);}}}
 // Aggregation, natural structures (chunk and 4 neighbours), score.
 static const int around[4][2]={{-1,0},{1,0},{0,-1},{0,1}};std::unordered_set<uint64_t> evaluate;
 for(uint64_t key:state.dirty){auto found=state.tracked.find(key);if(found==state.tracked.end()||!found->second.read)continue;SusState::Agg a;for(auto& s:found->second.subs)SusAdd(a.stats,s.stats);a.natural=SusNatural(a.stats);
  auto old=state.agg.find(key);const bool changed=old==state.agg.end()||old->second.natural!=a.natural;state.agg[key]=a;evaluate.insert(key);
  if(changed){const int x=(int)(key>>32),z=(int)(uint32_t)key;for(auto& n:around){const uint64_t k=SusKey(x+n[0],z+n[1]);if(state.agg.count(k))evaluate.insert(k);}}}
 state.dirty.clear();
 bool published=false;
 for(uint64_t key:evaluate){auto found=state.agg.find(key);if(found==state.agg.end())continue;const int x=(int)(key>>32),z=(int)(uint32_t)key;bool natural=found->second.natural;
  for(auto& n:around){auto other=state.agg.find(SusKey(x+n[0],z+n[1]));if(other!=state.agg.end()&&other->second.natural)natural=true;}
  int reasons=0,score=0;const SusStats& s=found->second.stats;if(!SusEvaluate(config,s,natural,&reasons,&score)){if(state.flagged.erase(key))published=true;continue;}
  SusFlag flag{x,z,reasons,score,s,s.v[13],s.v[14]};
  if(reasons&8){flag.lowY=std::min(flag.lowY,s.v[5]);flag.highY=std::max(flag.highY,s.v[5]+1);}if(reasons&0x10){flag.lowY=std::min(flag.lowY,s.v[7]);flag.highY=std::max(flag.highY,s.v[7]+1);}
  if(flag.highY<flag.lowY){flag.lowY=by;flag.highY=by;}
  const bool fresh=state.flagged.find(key)==state.flagged.end();state.flagged[key]=flag;published=true;
  if(fresh&&config.notify){std::string text="\xc2\xa7""7[\xc2\xa7""dNickel\xc2\xa7""7] \xc2\xa7""7Sus chunk at X: "+std::to_string(x*16)+" Z: "+std::to_string(z*16)+" ("+SusReasons(flag)+")";std::optional<std::string> none;
   reinterpret_cast<void(*)(uint64_t,const std::string*,std::optional<std::string>*)>(context.base+Rva(0x6da9820))(context.actor,&text,&none);}}
 if(published){std::vector<SusFlag> out;out.reserve(state.flagged.size());for(auto& e:state.flagged)out.push_back(e.second);AcquireSRWLockExclusive(&shared.lock);shared.flagged.swap(out);ReleaseSRWLockExclusive(&shared.lock);}
 InterlockedIncrement(&shared.ticks);
}
inline void SusGuarded(void* level){__try{SusWork(level);}__except(EXCEPTION_EXECUTE_HANDLER){InterlockedExchange(&Sus().fault,1);}}
}
