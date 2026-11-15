// Base Finder: scans loaded chunks for signs of player activity and classifies them as Stash, Base or
// Underground. Level tick: chunk disc of radius render distance + 1, period = offsets/8000 + 1 ticks, 1 ms
// budget (changed subchunks, unscanned chunks, then round-robin), layer 0 only. Palette entries are
// classified (Custom list, placed deepslate, a 108-entry table and suffixes, Out Of Place outside the
// Overworld, natural structures, trail blocks, solidity, flowing liquid), giving per-signal counts, up to
// 64 positions, natural counters, dug corridors / flat rooms (y < 50 / <= 19) and trails (runs >= 12,
// Nether tunnels). Aggregation (0.5 ms), scoring and verdict, 1 s purge, trail merging, chunk ages, and a
// global database (finds + trails + ages per dimension, also read by MiniMap "Show Finds"), Notify and Auto
// Waypoint. Drawing is in menu.cpp. The global database is not saved to disk.
#pragma once
#include "game_sus.hpp"
#include "game_packets.hpp"
#include "game_actions.hpp"
#include "game_view.hpp"
#include <array>
#include <cmath>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

namespace game {
struct BfConfig{bool enabled=false;bool signals[12]={true,true,true,true,true,true,true,true,true,true,true,false};std::vector<std::string> custom;int baseScore=12,stash=8;bool skipNatural=true;
 bool notify=true,autoWaypoint=false,chunkAges=false,trails=true;int trailLength=48;};
struct BfSeg{uint8_t type=0,axis=0;int32_t perp=0,y=0,begin=0,end=0;};
struct BfLine{std::string name;int signal=0;int count=0;int points=0;};
struct BfFindView{uint64_t key=0,id=0;int type=0;int score=0,storage=0,ymin=0,ymax=0,x=0,y=0,z=0,cx=0,cz=0;std::vector<BfLine> lines;std::vector<std::array<int,3>> spots;};
struct BfTrailView{BfSeg seg;uint64_t id=0;};
struct BfDbEntry{uint64_t id=0;int type=0,dim=0,cx=0,cz=0,x=0,y=0,z=0,state=0;int x2=0,z2=0;double time=0;};
struct BfShared{std::mutex guard;BfConfig cfg;std::vector<BfFindView> finds;std::vector<BfTrailView> trails;std::vector<std::pair<uint64_t,uint8_t>> ages;uint64_t region=0;int dimension=0;
 uint64_t analysed=0,tracked=0;volatile long fault=0;std::mutex pendGuard;std::vector<uint64_t> dirty,flowing;std::vector<std::array<int,4>> waypointRequests;std::vector<std::string> waypointNames;float feet[3]{};};
inline BfShared& BaseFinder(){static auto* s=new BfShared;return *s;}
// Global database: finds, trails and ages, also read by the MiniMap.
struct BfDb{std::mutex guard;std::vector<BfDbEntry> entries;std::map<uint64_t,uint8_t> ages[3];uint64_t next=1,version=0;};
inline BfDb& BaseDb(){static auto* d=new BfDb;return *d;}
inline double BfNowS(){return (double)GetTickCount64()/1000.0;}

// ---- classification
struct BfDesc{const char* name;const char* shown;uint8_t signal,weight;uint16_t mask;};
inline const BfDesc* BfGeneral(const std::string& n){
 static const BfDesc table[]={
  {"barrel","barrel",0,2,0x1},{"chest","chest",0,2,0x3ff},{"crafter","crafter",0,2,0x10},{"dispenser","dispenser",0,1,0x3ff},{"dropper","dropper",0,1,0},{"ender_chest","ender_chest",0,4,0},{"hopper","hopper",0,1,0x10},
  {"trapped_chest","trapped_chest",0,2,0x3ff},{"undyed_shulker_box","shulker_box",0,4,0},
  {"amethyst_block","amethyst_block",1,2,0x400},{"amethyst_cluster","amethyst_cluster",1,2,0x400},{"large_amethyst_bud","amethyst_cluster",1,2,0x400},{"medium_amethyst_bud","amethyst_cluster",1,2,0x400},
  {"small_amethyst_bud","amethyst_cluster",1,2,0x400},{"calibrated_sculk_sensor","calibrated_sculk_sensor",1,3,0},{"tinted_glass","tinted_glass",1,2,0},
  {"campfire","campfire",2,1,0x3ff},{"soul_campfire","campfire",2,1,0x3ff},{"candle","candle",2,1,0x3ff},{"copper_bulb","copper_bulb",2,1,0x10},{"end_rod","end_rod",2,1,0x100},{"glowstone","glowstone",2,1,0},
  {"lantern","lantern",2,1,0x3ff},{"soul_lantern","lantern",2,1,0x3ff},{"lit_pumpkin","jack_o_lantern",2,1,0},{"lit_redstone_lamp","redstone_lamp",2,1,0},{"redstone_lamp","redstone_lamp",2,1,0},
  {"ochre_froglight","froglight",2,1,0},{"pearlescent_froglight","froglight",2,1,0},{"verdant_froglight","froglight",2,1,0},{"sea_lantern","sea_lantern",2,1,0x40},{"shroomlight","shroomlight",2,1,0},
  {"torch","torch",2,1,0x3ff},{"soul_torch","torch",2,1,0x3ff},
  {"anvil","anvil",3,2,0},{"chipped_anvil","anvil",3,2,0},{"damaged_anvil","anvil",3,2,0},{"beacon","beacon",3,6,0},{"bed","bed",3,1,0x1},{"beehive","beehive",3,1,0},{"blast_furnace","blast_furnace",3,1,0x1},
  {"lit_blast_furnace","blast_furnace",3,1,0x1},{"brewing_stand","brewing_stand",3,2,0x3ff},{"cartography_table","cartography_table",3,1,0x1},{"cauldron","cauldron",3,1,0x3ff},{"conduit","conduit",3,3,0},
  {"crafting_table","crafting_table",3,1,0x3ff},{"enchanting_table","enchanting_table",3,3,0},{"fletching_table","fletching_table",3,1,0x1},{"grindstone","grindstone",3,1,0x1},{"jukebox","jukebox",3,2,0},
  {"lectern","lectern",3,1,0x3ff},{"lit_furnace","furnace",3,1,0x3ff},{"smoker","smoker",3,1,0x1},{"lit_smoker","smoker",3,1,0x1},{"lodestone","lodestone",3,3,0},{"loom","loom",3,1,0x1},
  {"respawn_anchor","respawn_anchor",3,3,0},{"smithing_table","smithing_table",3,1,0x1},{"stonecutter_block","stonecutter",3,1,0x1},
  {"bookshelf","bookshelf",4,1,0x3ff},{"chiseled_bookshelf","chiseled_bookshelf",4,2,0},{"glass","glass",4,1,0x3ff},{"glass_pane","glass_pane",4,1,0x3ff},
  {"activator_rail","rail",5,1,0x4},{"detector_rail","rail",5,1,0x4},{"golden_rail","rail",5,1,0x4},{"rail","rail",5,1,0x4},{"beetroot","crops",5,1,0x1},{"carrots","crops",5,1,0x1},{"potatoes","crops",5,1,0x1},
  {"wheat","crops",5,1,0x1},{"composter","composter",5,1,0x1},{"daylight_detector","daylight_detector",5,1,0},{"daylight_detector_inverted","daylight_detector",5,1,0},{"farmland","farmland",5,1,0x1},
  {"honey_block","honey_block",5,1,0},{"lever","lever",5,1,0x3ff},{"observer","observer",5,1,0},{"piston","piston",5,1,0x3ff},{"sticky_piston","piston",5,1,0x3ff},{"powered_comparator","comparator",5,1,0},
  {"unpowered_comparator","comparator",5,1,0},{"powered_repeater","repeater",5,1,0x3ff},{"unpowered_repeater","repeater",5,1,0x3ff},{"redstone_wire","redstone_wire",5,1,0x3ff},{"slime","slime_block",5,1,0},
  {"target","target",5,1,0},{"tnt","tnt",5,1,0x3ff},{"tripwire_hook","tripwire_hook",5,1,0x3ff},
  {"diamond_block","diamond_block",6,6,0},{"emerald_block","emerald_block",6,4,0},{"gold_block","gold_block",6,3,0x280},{"iron_block","iron_block",6,2,0},{"lapis_block","lapis_block",6,2,0},{"netherite_block","netherite_block",6,8,0},
  {"decorated_pot","decorated_pot",7,1,0x3ff},{"flower_pot","flower_pot",7,1,0x3ff},{"frame","item_frame",7,2,0x100},{"glow_frame","item_frame",7,2,0},{"player_head","player_head",7,3,0},
  {"standing_banner","banner",7,1,0x3ff},{"wall_banner","banner",7,1,0x3ff},{"standing_sign","sign",7,2,0},{"wall_sign","sign",7,2,0},
  {"end_portal","end_portal",8,3,0},{"obsidian","obsidian",8,1,0x200},{"portal","nether_portal",8,5,0}};
 static std::unordered_map<std::string,const BfDesc*>* index=[]{auto* m=new std::unordered_map<std::string,const BfDesc*>;for(auto& d:table)(*m)[std::string("minecraft:")+d.name]=&d;return m;}();
 auto f=index->find(n);return f==index->end()?nullptr:f->second;}
inline bool BfEnds(const std::string& n,const char* s){const size_t l=strlen(s);return n.size()>l&&n.compare(n.size()-l,l,s)==0;}
inline bool BfSuffix(const std::string& n,BfDesc* out){
 struct S{const char* suf;const char* shown;uint8_t sig,w;uint16_t mask;};
 static const S list[]={{"_stained_glass_pane","stained_glass",4,1,0x3ff},{"_glazed_terracotta","glazed_terracotta",4,2,0x3ff},{"_concrete_powder","concrete_powder",4,2,0},{"_stained_glass","stained_glass",4,1,0x3ff},
  {"_shulker_box","shulker_box",0,4,0},{"_copper_bulb","copper_bulb",2,1,0x10},{"_concrete","concrete",4,2,0},{"_trapdoor","trapdoor",4,1,0x3ff},{"_carpet","carpet",4,1,0x3ff},{"_candle","candle",2,1,0x3ff},
  {"_wool","wool",4,1,0x3ff},{"_door","door",4,1,0x3ff},{"_sign","sign",7,2,0}};
 for(auto& s:list)if(BfEnds(n,s.suf)){if(!strcmp(s.suf,"_carpet")&&(n=="minecraft:moss_carpet"||n=="minecraft:pale_moss_carpet"))return false;*out={nullptr,s.shown,s.sig,s.w,s.mask};return true;}
 return false;}
// Natural structures: category and weight.
inline bool BfNatural(const std::string& n,uint8_t* cat,uint8_t* weight){
 struct N{const char* name;uint8_t c,w;};static const N exact[]={{"bell",0,16},{"hay_block",0,2},{"mob_spawner",1,16},{"mossy_cobblestone",1,1},{"oak_fence",2,1},{"dark_oak_fence",2,1},{"web",2,1},
  {"chiseled_stone_bricks",3,1},{"cracked_stone_bricks",3,1},{"mossy_stone_bricks",3,1},{"stone_bricks",3,1},{"iron_bars",3,1},{"end_portal_frame",3,16},{"chiseled_tuff",4,1},{"polished_tuff",4,1},
  {"trial_spawner",4,16},{"vault",4,16},{"reinforced_deepslate",5,16},{"sponge",6,16},{"gilded_blackstone",7,16},{"end_rod",8,1},{"crying_obsidian",9,8},{"budding_amethyst",10,16}};
 for(auto& e:exact)if(n.size()==10+strlen(e.name)&&n.compare(10,std::string::npos,e.name)==0&&n.compare(0,10,"minecraft:")==0){*cat=e.c;*weight=e.w;return true;}
 struct P{const char* part;uint8_t c;};static const P parts[]={{"tuff_brick",4},{"minecraft:waxed_",4},{"deepslate_tiles",5},{"deepslate_bricks",5},{"sculk",5},{"prismarine",6},{"polished_blackstone_bricks",7},{"purpur",8},{"minecraft:infested_",3}};
 for(auto& p:parts)if(n.find(p.part)!=std::string::npos){*cat=p.c;*weight=1;return true;}return false;}
struct BfEntry{uint16_t id=0;bool sig=false;uint8_t nat=0,natW=0;bool natOk=false;uint8_t trail=0;bool isTrail=false;uint8_t solid=2;bool flowing=false;};
struct BfSignal{std::string name;uint8_t signal=0,weight=0;uint16_t mask=0;};
struct BfPos{int32_t x,y,z;uint16_t id;};
struct BfSubRec{uint64_t l0=0,l1=0,s0=0,s1=0;std::vector<std::pair<uint16_t,int>> counts;std::vector<BfPos> pos;int nat[11]{};std::vector<BfSeg> segs;int flowing=0;int ymin=0x7fffffff,ymax=(int)0x80000000;};
struct BfChunk{uint64_t chunk=0,begin=0;uint32_t count=0;bool read=false;uint64_t gen=0;int x=0,z=0;std::vector<BfSubRec> subs;};
struct BfResult{std::map<uint16_t,int> counts;std::vector<BfPos> pos;int nat[11]{};uint16_t flags=0;bool liquid=false;int ymin=0x7fffffff,ymax=(int)0x80000000;};
struct BfFind{uint64_t id=0;int type=0,state=0,score=0,storage=0,ymin=0,ymax=0,x=0,y=0,z=0;std::vector<BfLine> lines;std::vector<std::array<int,3>> spots;};
struct BfTrail{BfSeg seg;uint64_t id=0;bool recorded=false,hidden=false;};
struct BfState{uint64_t level=0,region=0;int dim=0,radius=-1;std::vector<std::string> custom;bool agesOn=false,trailsOn=false;std::unordered_map<uint64_t,BfEntry> entries;std::vector<BfSignal> signals;
 std::vector<std::pair<int,int>> offsets;uint64_t gen=0;std::unordered_map<uint64_t,BfChunk> chunks;std::vector<uint64_t> inRange;size_t robin=0;int period=1,countdown=0;
 std::unordered_map<uint64_t,BfResult> results;std::set<uint64_t> toAggregate,toScore;double purge=0,resync=0;bool scoringSet=false;bool sig[12]{};bool skip=true;int baseScore=12,stash=8;
 std::unordered_map<uint64_t,BfFind> finds;std::unordered_map<uint64_t,uint8_t> ages;std::vector<BfSeg> pass;std::vector<BfTrail> trails;uint64_t dbVersion=0;};
inline BfState& BfData(){static auto* s=new BfState;return *s;}
inline uint16_t BfSignalId(BfState& st,const std::string& name,uint8_t signal,uint8_t weight,uint16_t mask){for(size_t i=0;i<st.signals.size();++i)if(st.signals[i].name==name&&st.signals[i].signal==signal)return (uint16_t)i;
 st.signals.push_back({name,signal,weight,mask});return (uint16_t)(st.signals.size()-1);}
inline int BfIntState(const Context& c,uint64_t block,const char* key){const uint64_t states=SusTag(c,block+0xf0,"states",10);const uint64_t t=states?SusTag(c,states,key,3):0;int32_t v=0;if(t)Read(t+8,&v);return v;}
inline BfEntry BfClassify(const Context& c,BfState& st,uint64_t block){BfEntry e{};uint64_t legacy{};char raw[96]{};if(!block||!Read(block+0x68,&legacy)||!legacy||!Text(legacy+0xe8,raw,sizeof(raw)))return e;const std::string n=raw;
 uint8_t material{},bits{};Read(legacy+0x28,&material);Read(legacy+0x29,&bits);e.solid=material==0?0:((bits&0x17)==0x14?1:2);if((bits&2)&&BfIntState(c,block,"liquid_depth")!=0)e.flowing=true;
 // signal
 if(std::binary_search(st.custom.begin(),st.custom.end(),n)){e.id=BfSignalId(st,n,11,2,0);e.sig=true;}
 else if(n=="minecraft:deepslate"){const uint64_t states=SusTag(c,block+0xf0,"states",10);const uint64_t axis=states?SusTag(c,states,"pillar_axis",8):0;std::string v;
  if(axis&&SusString(axis+8,&v)&&v!="y"){e.id=BfSignalId(st,"rotated_deepslate",9,2,0);e.sig=true;}}
 else{const bool endObs=st.dim==2&&(n=="minecraft:obsidian"||n=="minecraft:end_rod"),netherGlow=st.dim==1&&(n=="minecraft:shroomlight"||n=="minecraft:glowstone");
  if(!endObs&&!netherGlow){bool done=false;
   if(st.dim!=0){struct O{const char* n;const char* s;uint8_t w;};static const O out[]={{"minecraft:blue_ice","blue_ice",2},{"minecraft:cobblestone","cobblestone",1},{"minecraft:dirt","dirt",1},{"minecraft:flowing_water","water",2},
     {"minecraft:grass_block","grass_block",1},{"minecraft:ice","ice",1},{"minecraft:packed_ice","packed_ice",2},{"minecraft:snow","snow",1},{"minecraft:snow_layer","snow",1},{"minecraft:stone","stone",1},{"minecraft:water","water",2}};
    for(auto& o:out)if(n==o.n){e.id=BfSignalId(st,o.s,10,o.w,0);e.sig=done=true;break;}
    if(!done&&BfEnds(n,"_log")){e.id=BfSignalId(st,"log",10,1,0);e.sig=done=true;}if(!done&&BfEnds(n,"_planks")){e.id=BfSignalId(st,"planks",10,1,0);e.sig=done=true;}}
   if(!done){if(const BfDesc* d=BfGeneral(n)){e.id=BfSignalId(st,d->shown,d->signal,d->weight,d->mask);e.sig=true;}else{BfDesc s{};if(BfSuffix(n,&s)){e.id=BfSignalId(st,s.shown,s.signal,s.weight,s.mask);e.sig=true;}}}}}
 e.natOk=BfNatural(n,&e.nat,&e.natW);
 if(n=="minecraft:dirt_path"||n=="minecraft:grass_path"){e.trail=0;e.isTrail=true;}else if(n=="minecraft:rail"||n=="minecraft:golden_rail"||n=="minecraft:detector_rail"||n=="minecraft:activator_rail"){e.trail=1;e.isTrail=true;}
 else if(n=="minecraft:blue_ice"||n=="minecraft:packed_ice"){e.trail=2;e.isTrail=true;}
 return e;}
inline const BfEntry& BfEntryOf(const Context& c,BfState& st,uint64_t block){auto f=st.entries.find(block);if(f!=st.entries.end())return f->second;return st.entries.emplace(block,BfClassify(c,st,block)).first->second;}
inline uint64_t BfPalette(const Context& c,uint64_t storage){return SusPalette(c,storage);}
// Layer 0: palette entries and indices.
inline bool BfLayer(const Context& c,BfState& st,uint64_t storage,std::vector<BfEntry>& entries,uint16_t* index){entries.clear();if(!storage)return false;
 const uint64_t palette=Virtual(c,storage,0xa8),bitsOf=Virtual(c,storage,0xa0),wordsOf=Virtual(c,storage,0x90);if(!palette||!bitsOf||!wordsOf)return false;
 uint64_t p[2]{};reinterpret_cast<void*(*)(uint64_t,uint64_t*)>(palette)(storage,p);if(!p[0]||!p[1])return false;const uint32_t n=(uint32_t)std::min<uint64_t>(p[0],4096);entries.resize(n);
 for(uint32_t i=0;i<n;++i){uint64_t b{};entries[i]=Read(p[1]+8ull*i,&b)?BfEntryOf(c,st,b):BfEntry{};}
 const int bits=reinterpret_cast<int(*)(uint64_t)>(bitsOf)(storage);if(bits<=0){memset(index,0,4096*2);return true;}if(bits>32)return false;
 uint64_t words[2]{};reinterpret_cast<void*(*)(uint64_t,uint64_t*)>(wordsOf)(storage,words);if(!words[0]||!words[1])return false;const uint32_t per=32/bits,mask=bits==32?0xffffffffu:((1u<<bits)-1);
 for(uint32_t at=0;at<4096;++at){const uint32_t w=at/per;uint32_t v{};index[at]=0xffff;if(w>=words[0]||!Read(words[1]+4ull*w,&v))continue;const uint32_t pick=(v>>((at%per)*bits))&mask;index[at]=pick<n?(uint16_t)pick:0xffff;}return true;}
inline void BfFillGrid(const Context& c,BfState& st,uint8_t* grid,uint64_t sub,int first,int last,int shift,uint8_t fill){uint64_t l0{};
 if(!sub||!Read(sub+0x30,&l0)||!l0){memset(grid+(shift+first)*256+256,sub?0:fill,(size_t)(last-first)*256+256);return;}std::vector<BfEntry> e;static uint16_t idx[4096];
 if(!BfLayer(c,st,l0,e,idx)){memset(grid+(shift+first)*256+256,0,(size_t)(last-first)*256+256);return;}
 for(uint32_t at=0;at<4096;++at){const int y=at&15;if(y<first||y>last)continue;grid[(y+shift)*256+(at>>4)+256]=idx[at]<e.size()?e[idx[at]].solid:0;}}
// Scans one subchunk.
inline void BfScanSub(const Context& c,BfState& st,BfChunk& ch,uint32_t i,int minY){BfSubRec& r=ch.subs[i];r=BfSubRec{};const uint64_t address=ch.begin+0x68ull*i;
 if(!Read(address+0x30,&r.l0)||!Read(address+0x38,&r.l1))return;r.s0=BfPalette(c,r.l0);r.s1=BfPalette(c,r.l1);st.toAggregate.insert(((uint64_t)(uint32_t)ch.x<<32)|(uint32_t)ch.z);if(!r.l0)return;
 std::vector<BfEntry> e;static uint16_t idx[4096];if(!BfLayer(c,st,r.l0,e,idx))return;const int baseY=minY+16*(int)i,x0=ch.x*16,z0=ch.z*16;
 bool anySig=false,anyNat=false,anyFlow=false,air=false,solid=false;bool trailType[3]{};for(auto& v:e){anySig|=v.sig;anyNat|=v.natOk;anyFlow|=v.flowing;air|=v.solid==0;solid|=v.solid==1;if(v.isTrail&&v.trail<3)trailType[v.trail]=true;}
 // trails
 if(st.trailsOn){static uint8_t g[4096],s[4096];for(uint32_t at=0;at<4096;++at){const int x=at>>8,z=(at>>4)&15,y=at&15;const uint16_t k=idx[at];const BfEntry* v=k<e.size()?&e[k]:nullptr;
   g[y*256+z*16+x]=v&&v->isTrail?(uint8_t)(1+v->trail):0;s[y*256+z*16+x]=v?v->solid:0;}
  for(int type=0;type<3;++type){if(!trailType[type])continue;for(int axis=0;axis<2;++axis){int best=0,bl=0,by=0,bo=0;
    for(int y=0;y<16;++y)for(int line=0;line<16;++line){int run=0;for(int k=0;k<16;++k){const int x=axis==0?k:line,z=axis==0?line:k;if(g[y*256+z*16+x]==1+type){++run;if(run>best){best=run;bl=line;by=y;bo=k-run+1;}}else run=0;}}
    if(best>11){BfSeg sg;sg.type=(uint8_t)type;sg.axis=(uint8_t)axis;sg.perp=(axis==0?z0:x0)+bl;sg.y=baseY+by;sg.begin=(axis==0?x0:z0)+bo;sg.end=sg.begin+best-1;r.segs.push_back(sg);}}}
  if(st.dim==1&&air&&solid)for(int axis=0;axis<2;++axis){bool found=false;
   auto S=[&](int x,int y,int z){return x>=0&&x<16&&z>=0&&z<16&&y>=0&&y<16&&s[y*256+z*16+x]==1;};auto A=[&](int x,int y,int z){return x>=0&&x<16&&z>=0&&z<16&&y>=0&&y<16&&s[y*256+z*16+x]==0;};
   for(int y=1;y<=11&&!found;++y)for(int line=0;line<16&&!found;++line){bool ok=true;for(int k=0;k<16&&ok;++k){const int x=axis==0?k:line,z=axis==0?line:k;
     if(!S(x,y-1,z)||!A(x,y,z)||!A(x,y+1,z)||!(S(x,y+2,z)||S(x,y+3,z)||S(x,y+4,z))){ok=false;break;}bool l=false,rr=false;
     for(int d=1;d<=4;++d){if(axis==0){l|=S(x,y,z-d);rr|=S(x,y,z+d);}else{l|=S(x-d,y,z);rr|=S(x+d,y,z);}}ok=l&&rr;}
    if(ok){BfSeg sg;sg.type=3;sg.axis=(uint8_t)axis;sg.perp=(axis==0?z0:x0)+line;sg.y=baseY+y;sg.begin=axis==0?x0:z0;sg.end=sg.begin+15;r.segs.push_back(sg);found=true;}}}}
 // counting
 if(anySig||anyNat||anyFlow){std::vector<int> per(e.size(),0);
  for(uint32_t at=0;at<4096;++at){const uint16_t k=idx[at];if(k>=e.size())continue;const BfEntry& v=e[k];if(v.natOk)r.nat[v.nat]+=v.natW;if(v.flowing)++r.flowing;
   if(v.sig){++per[k];const int y=baseY+(int)(at&15);r.ymin=std::min(r.ymin,y);r.ymax=std::max(r.ymax,y);if(r.pos.size()<64)r.pos.push_back({x0+(int32_t)(at>>8),y,z0+(int32_t)((at>>4)&15),v.id});}}
  std::map<uint16_t,int> sum;for(size_t k=0;k<e.size();++k)if(per[k])sum[e[k].id]+=per[k];for(auto& p:sum)r.counts.push_back(p);}
 // underground
 if(baseY<50&&air&&solid){static uint8_t grid[0x1300];memset(grid,0,sizeof(grid));
  BfFillGrid(c,st,grid,i?address-0x68:0,15,15,-16,1);BfFillGrid(c,st,grid,i+1<ch.count?address+0x68:0,0,1,16,0);BfFillGrid(c,st,grid,address,0,15,0,0);
  const auto tunnel=SusTunnel(grid);if(baseY+tunnel.second<50&&tunnel.first>7){const uint16_t id=BfSignalId(st,"dug_corridor",9,2,0x3e);r.counts.push_back({id,1});const int y=baseY+tunnel.second;r.ymin=std::min(r.ymin,y);r.ymax=std::max(r.ymax,y);}
  if(baseY<=19){const auto room=SusRoom(grid);if(baseY+room.second<=19&&room.first>=1){const uint16_t id=BfSignalId(st,"flat_room",9,2,0x3e);r.counts.push_back({id,1});}}}}
inline uint16_t BfFlags(const int* nat){uint16_t f=0;for(int c=0;c<11;++c)if(nat[c]>15)f|=(uint16_t)(1u<<c);return f;}
inline std::string BfPretty(std::string s){if(s.compare(0,10,"minecraft:")==0)s=s.substr(10);for(auto& ch:s)if(ch=='_')ch=' ';bool up=true;for(auto& ch:s){if(up&&ch>='a'&&ch<='z')ch=(char)(ch-32);up=ch==' '||ch=='-'||ch==':';}return s;}
inline int BfBitLen(int n){int b=0;while(n>0){++b;n>>=1;}return b;}
// Scoring; returns the verdict 0/1/2.
inline int BfScore(BfState& st,const BfResult& r,uint16_t N,std::vector<BfLine>* lines,int* score,int* storage){*score=0;*storage=0;std::map<std::string,BfLine> grouped;
 for(auto& [id,count]:r.counts){if(count<=0||id>=st.signals.size())continue;const BfSignal& s=st.signals[id];if(!st.sig[s.signal])continue;if(st.skip&&(s.mask&N))continue;
  const int pts=BfBitLen(count)*s.weight;*score+=pts;if(s.signal==0)*storage+=count;BfLine& l=grouped[s.name];l.name=s.name;l.signal=s.signal;l.count+=count;l.points=std::max(l.points,pts);}
 if(st.sig[1]&&r.nat[10]>47&&!grouped.count("amethyst_cluster")){BfLine l{"harvested_geode",1,r.nat[10]/16,4};grouped[l.name]=l;*score+=4;}
 lines->clear();for(auto& g:grouped)lines->push_back(g.second);std::sort(lines->begin(),lines->end(),[](const BfLine& a,const BfLine& b){if(a.points!=b.points)return a.points>b.points;if(a.count!=b.count)return a.count>b.count;return a.name<b.name;});
 if(*storage>=st.stash)return 2;if(*score>=st.baseScore)return 1;return 0;}
inline std::string BfSummary(const std::vector<BfLine>& lines){std::string out;for(size_t i=0;i<lines.size()&&i<3;++i){if(i)out+=", ";out+=BfPretty(lines[i].name)+" \xc3\x97"+std::to_string(lines[i].count);}
 if(lines.size()>3)out+=", +"+std::to_string(lines.size()-3);return out;}
inline const char* BfKindName(int type){return type==1?"Stash":type==2?"Underground":type==3?"Trail":"Base";}
// Global database submit; returns true if the entry is new.
inline bool BfDbSubmit(BfDbEntry e,uint64_t* id){auto& d=BaseDb();std::lock_guard<std::mutex> l(d.guard);
 for(auto& x:d.entries){if(x.dim!=e.dim)continue;bool same=false;if(e.type==3)same=x.type==3&&x.cx==e.cx&&x.cz==e.cz&&x.x==e.x&&x.z==e.z;else same=x.type!=3&&x.cx==e.cx&&x.cz==e.cz&&((x.type==1)==(e.type==1));
  if(same){x.time=BfNowS();x.x2=e.x2;x.z2=e.z2;if(e.type!=3){x.type=e.type;x.x=e.x;x.y=e.y;x.z=e.z;}*id=x.id;return false;}}
 e.id=d.next++;e.time=BfNowS();d.entries.push_back(e);++d.version;*id=e.id;return true;}
inline uint8_t BfDbAge(int dim,uint64_t key,uint8_t age){auto& d=BaseDb();std::lock_guard<std::mutex> l(d.guard);auto& m=d.ages[std::clamp(dim,0,2)];auto f=m.find(key);
 if(f==m.end()){m[key]=age;++d.version;return age;}if(f->second==0&&age){f->second=age;++d.version;}return f->second;}
inline void BfResetAnalysis(BfState& st){for(auto& e:st.chunks){e.second.read=false;e.second.subs.clear();}st.results.clear();st.toAggregate.clear();st.toScore.clear();st.finds.clear();}
inline void BfResetAll(BfState& st){st.chunks.clear();st.inRange.clear();st.robin=0;st.results.clear();st.toAggregate.clear();st.toScore.clear();st.finds.clear();st.ages.clear();st.pass.clear();st.trails.clear();
 st.entries.clear();st.signals.clear();st.radius=-1;st.countdown=0;}
inline void BfPublish(BfState& st){auto& s=BaseFinder();std::vector<BfFindView> fv;for(auto& [key,f]:st.finds){if(f.state==2)continue;BfFindView v;v.key=key;v.id=f.id;v.type=f.type;v.score=f.score;v.storage=f.storage;v.ymin=f.ymin;v.ymax=f.ymax;
  v.x=f.x;v.y=f.y;v.z=f.z;v.cx=(int)(int32_t)(key>>32);v.cz=(int)(int32_t)(uint32_t)key;v.lines=f.lines;v.spots=f.spots;fv.push_back(std::move(v));}
 std::vector<BfTrailView> tv;for(auto& t:st.trails)if(t.recorded&&!t.hidden)tv.push_back({t.seg,t.id});std::vector<std::pair<uint64_t,uint8_t>> ages(st.ages.begin(),st.ages.end());
 uint64_t analysed=0;for(auto& c:st.chunks)analysed+=c.second.read;std::lock_guard<std::mutex> l(s.guard);s.finds.swap(fv);s.trails.swap(tv);s.ages.swap(ages);s.region=st.region;s.dimension=st.dim;s.analysed=analysed;s.tracked=st.chunks.size();}
// ---- main loop
inline void BaseFinderWork(){auto& s=BaseFinder();auto& st=BfData();BfConfig cfg;{std::lock_guard<std::mutex> l(s.guard);cfg=s.cfg;}
 static bool was=false;if(!cfg.enabled){if(was){BfResetAll(st);st.level=0;st.region=0;BfPublish(st);}was=false;return;}if(!was){BfResetAll(st);st.level=0;}was=true;   // enable / disable: reset
 Context c{};if(!Local(&c)){BfResetAll(st);return;}const uint64_t region=ActorRegion(c);if(!region){BfResetAll(st);return;}uint64_t level{};Read(c.actor+0x200,&level);int dim=0;ReadDimension(&dim);
 if(level!=st.level||region!=st.region||dim!=st.dim){BfResetAll(st);st.level=level;st.region=region;st.dim=dim;}
 std::vector<std::string> custom=cfg.custom;std::sort(custom.begin(),custom.end());
 if(custom!=st.custom||cfg.chunkAges!=st.agesOn||cfg.trails!=st.trailsOn){st.custom=custom;st.agesOn=cfg.chunkAges;st.trailsOn=cfg.trails;st.entries.clear();st.signals.clear();BfResetAnalysis(st);}
 {bool changed=!st.scoringSet||cfg.skipNatural!=st.skip||cfg.baseScore!=st.baseScore||cfg.stash!=st.stash;for(int i=0;i<12;++i)changed|=cfg.signals[i]!=st.sig[i];
  if(changed){st.scoringSet=true;memcpy(st.sig,cfg.signals,12);st.skip=cfg.skipNatural;st.baseScore=cfg.baseScore;st.stash=cfg.stash;for(auto& r:st.results)st.toScore.insert(r.first);}}
 int distance=8;{const uint64_t options=GameOptions(c);if(options)distance=reinterpret_cast<int(*)(uint64_t)>(c.base+Rva(0x5b6f550))(options);}if(distance<2||distance>96)distance=8;const int R=distance+1;
 if(R!=st.radius){st.radius=R;st.offsets.clear();for(int dx=-R;dx<=R;++dx)for(int dz=-R;dz<=R;++dz)if(dx*dx+dz*dz<=R*R)st.offsets.push_back({dx,dz});
  std::stable_sort(st.offsets.begin(),st.offsets.end(),[](auto& a,auto& b){return a.first*a.first+a.second*a.second<b.first*b.first+b.second*b.second;});st.period=(int)st.offsets.size()/8000+1;st.countdown=0;}
 if(--st.countdown>0)return;st.countdown=st.period;
 uint64_t where{};float feet[3]{},lift=0;if(!Read(c.actor+0x240,&where)||!where||!Floats(where,feet,3))return;uintptr_t off{};if(Element(c.registry,c.entity,"OffsetsComponent",0x44,&off))Floats(off,&lift,1);feet[1]-=lift;
 {std::lock_guard<std::mutex> l(s.guard);memcpy(s.feet,feet,12);}
 int16_t minY{};if(!Read(region+0x3a,&minY))return;const int px=(int)std::floor(feet[0]),pz=(int)std::floor(feet[2]);const uint64_t chunkAt=Virtual(c,region,0x148);if(!chunkAt)return;
 const int64_t t0=ActionNs();auto late=[&](double ms){return (double)(ActionNs()-t0)/1e6>ms;};++st.gen;st.inRange.clear();const size_t before=st.chunks.size();
 for(auto& o:st.offsets){int cp[2]={(px>>4)+o.first,(pz>>4)+o.second};const uint64_t chunk=reinterpret_cast<uint64_t(*)(uint64_t,int*)>(chunkAt)(region,cp);if(!chunk)continue;uint64_t b{},e{};
  if(!Read(chunk+0x1c8,&b)||!Read(chunk+0x1d0,&e)||e<b)continue;const uint32_t count=(uint32_t)std::min<uint64_t>((e-b)/0x68,64);const uint64_t key=((uint64_t)(uint32_t)cp[0]<<32)|(uint32_t)cp[1];BfChunk& r=st.chunks[key];
  if(r.chunk!=chunk||r.begin!=b||(r.read&&r.count!=count)){r.chunk=chunk;r.begin=b;r.subs.clear();r.read=false;}r.count=count;r.x=cp[0];r.z=cp[1];r.gen=st.gen;st.inRange.push_back(key);}
 for(auto it=st.chunks.begin();it!=st.chunks.end();)it=it->second.gen==st.gen?std::next(it):st.chunks.erase(it);if(st.chunks.size()!=before&&st.robin>=st.inRange.size())st.robin=0;
 std::vector<uint64_t> dirty,flowing;{std::lock_guard<std::mutex> l(s.pendGuard);dirty.swap(s.dirty);flowing.swap(s.flowing);}
 if(!dirty.empty()){std::sort(dirty.begin(),dirty.end());dirty.erase(std::unique(dirty.begin(),dirty.end()),dirty.end());
  for(uint64_t k:st.inRange){BfChunk& r=st.chunks[k];if(!r.read)continue;for(uint64_t a:dirty)if(a>=r.begin&&a<r.begin+0x68ull*r.count&&(a-r.begin)%0x68==0)BfScanSub(c,st,r,(uint32_t)((a-r.begin)/0x68),minY);}}
 for(uint64_t k:st.inRange){if(late(1.0))break;BfChunk& r=st.chunks[k];if(r.read)continue;r.subs.assign(r.count,BfSubRec{});for(uint32_t i=0;i<r.count;++i)BfScanSub(c,st,r,i,minY);r.read=true;}
 for(size_t n=0;n<st.inRange.size()&&!late(1.0);++n){st.robin=(st.robin+1)%st.inRange.size();BfChunk& r=st.chunks[st.inRange[st.robin]];if(!r.read)continue;
  for(uint32_t i=0;i<r.count&&i<r.subs.size();++i){const uint64_t a=r.begin+0x68ull*i;uint64_t l0{},l1{};if(!Read(a+0x30,&l0)||!Read(a+0x38,&l1))continue;BfSubRec& sub=r.subs[i];
   if(l0!=sub.l0||l1!=sub.l1||BfPalette(c,l0)!=sub.s0||BfPalette(c,l1)!=sub.s1)BfScanSub(c,st,r,i,minY);}}
 // purge (1 s)
 const double now=BfNowS();if(now-st.purge>=1.0){st.purge=now;for(auto it=st.results.begin();it!=st.results.end();)it=st.chunks.count(it->first)?std::next(it):st.results.erase(it);
  for(auto it=st.finds.begin();it!=st.finds.end();)it=st.chunks.count(it->first)?std::next(it):st.finds.erase(it);for(auto it=st.ages.begin();it!=st.ages.end();)it=st.chunks.count(it->first)?std::next(it):st.ages.erase(it);}
 // aggregation (0.5 ms)
 const int64_t t1=ActionNs();auto late1=[&](double ms){return (double)(ActionNs()-t1)/1e6>ms;};
 while(!st.toAggregate.empty()&&!late1(0.5)){const uint64_t key=*st.toAggregate.begin();st.toAggregate.erase(st.toAggregate.begin());auto ch=st.chunks.find(key);if(ch==st.chunks.end()||!ch->second.read)continue;
  BfResult r{};std::vector<BfSeg> segs;for(auto& sub:ch->second.subs){for(auto& p:sub.counts)r.counts[p.first]+=p.second;r.pos.insert(r.pos.end(),sub.pos.begin(),sub.pos.end());for(int k=0;k<11;++k)r.nat[k]+=sub.nat[k];
   if(sub.flowing>0)r.liquid=true;if(!sub.counts.empty()){r.ymin=std::min(r.ymin,sub.ymin);r.ymax=std::max(r.ymax,sub.ymax);}segs.insert(segs.end(),sub.segs.begin(),sub.segs.end());}
  r.flags=BfFlags(r.nat);auto old=st.results.find(key);const bool fresh=old==st.results.end();const uint16_t oldFlags=fresh?0:old->second.flags;st.results[key]=r;st.toScore.insert(key);
  if(fresh&&cfg.chunkAges)st.ages[key]=BfDbAge(st.dim,key,r.liquid?2:0);
  if(!fresh&&oldFlags!=r.flags){static const int nb[4][2]={{1,0},{-1,0},{0,1},{0,-1}};const int cx=(int)(int32_t)(key>>32),cz=(int)(int32_t)(uint32_t)key;
   for(auto& d:nb){const uint64_t k2=((uint64_t)(uint32_t)(cx+d[0])<<32)|(uint32_t)(cz+d[1]);if(st.results.count(k2))st.toScore.insert(k2);}}
  for(auto& sg:segs){if(st.skip&&((sg.type==1&&(r.flags&4))||(sg.type==0&&(r.flags&1))))continue;st.pass.push_back(sg);}}
 // ages: subchunks where a liquid started flowing
 if(cfg.chunkAges&&!flowing.empty()){std::sort(flowing.begin(),flowing.end());flowing.erase(std::unique(flowing.begin(),flowing.end()),flowing.end());
  for(auto& [key,ch]:st.chunks){if(!ch.read)continue;for(uint64_t a:flowing)if(a>=ch.begin&&a<ch.begin+0x68ull*ch.count){st.ages[key]=BfDbAge(st.dim,key,1);break;}}}
 // scoring (0.5 ms)
 while(!st.toScore.empty()&&!late1(1.0)){const uint64_t key=*st.toScore.begin();st.toScore.erase(st.toScore.begin());auto rr=st.results.find(key);if(rr==st.results.end())continue;const BfResult& r=rr->second;
  const int cx=(int)(int32_t)(key>>32),cz=(int)(int32_t)(uint32_t)key;uint16_t N=r.flags;static const int nb[4][2]={{1,0},{-1,0},{0,1},{0,-1}};
  for(auto& d:nb){auto f=st.results.find(((uint64_t)(uint32_t)(cx+d[0])<<32)|(uint32_t)(cz+d[1]));if(f!=st.results.end())N|=f->second.flags;}
  std::vector<BfLine> lines;int score{},storage{};const int verdict=BfScore(st,r,N,&lines,&score,&storage);if(!verdict){st.finds.erase(key);continue;}
  const int type=verdict==2?1:(r.ymax<50&&st.dim==0?2:0);int best=-1,bx=cx*16+8,by=r.ymin==0x7fffffff?0:r.ymin,bz=cz*16+8;std::vector<std::array<int,3>> spots;
  for(auto& p:r.pos){if(p.id>=st.signals.size())continue;const BfSignal& s=st.signals[p.id];if(!st.sig[s.signal]||(st.skip&&(s.mask&N)))continue;spots.push_back({p.x,p.y,p.z});
   const int pr=s.weight+(s.signal==0?100:0);if(pr>best){best=pr;bx=p.x;by=p.y;bz=p.z;}}
  BfDbEntry e{};e.type=type;e.dim=st.dim;e.cx=cx;e.cz=cz;e.x=bx;e.y=by;e.z=bz;uint64_t id{};const bool isNew=BfDbSubmit(e,&id);
  BfFind& f=st.finds[key];f.id=id;f.type=type;f.score=score;f.storage=storage;f.ymin=r.ymin;f.ymax=r.ymax;f.x=bx;f.y=by;f.z=bz;f.lines=lines;f.spots=spots;
  if(isNew){if(cfg.notify)ClientMessage(c,std::string("\xc2\xa7""7")+BfKindName(type)+" found at "+std::to_string(bx)+" "+std::to_string(by)+" "+std::to_string(bz)+": "+BfSummary(lines));
   if(cfg.autoWaypoint){std::lock_guard<std::mutex> l(s.guard);s.waypointRequests.push_back({bx,by,bz,st.dim});s.waypointNames.push_back(std::string(BfKindName(type))+" "+std::to_string(bx)+" "+std::to_string(bz));}}}
 // trails: merge and record
 for(auto& sg:st.pass){BfTrail* hit=nullptr;for(auto& t:st.trails){const BfSeg& o=t.seg;if(o.type==sg.type&&o.axis==sg.axis&&std::abs(o.perp-sg.perp)<3&&std::abs(o.y-sg.y)<4&&sg.begin<=o.end+32&&o.begin<=sg.end+32){hit=&t;break;}}
  if(hit){hit->seg.begin=std::min(hit->seg.begin,sg.begin);hit->seg.end=std::max(hit->seg.end,sg.end);}else{st.trails.push_back({sg});hit=&st.trails.back();}
  const BfSeg& t=hit->seg;if(t.end-t.begin+1>=cfg.trailLength&&!hit->recorded){const int x1=t.axis==0?t.begin:t.perp,z1=t.axis==0?t.perp:t.begin,x2=t.axis==0?t.end:t.perp,z2=t.axis==0?t.perp:t.end;
   BfDbEntry e{};e.type=3;e.dim=st.dim;e.cx=x1>>4;e.cz=z1>>4;e.x=x1;e.y=t.y;e.z=z1;e.x2=x2;e.z2=z2;uint64_t id{};const bool isNew=BfDbSubmit(e,&id);hit->id=id;
   if(isNew){hit->recorded=true;hit->hidden=false;static const char* names[4]={"dirt_path","rail_line","ice_road","nether_tunnel"};
    if(cfg.notify)ClientMessage(c,std::string("\xc2\xa7""7")+BfPretty(names[t.type&3])+" found, "+std::to_string(t.end-t.begin+1)+" blocks from "+std::to_string(x1)+" "+std::to_string(z1)+" to "+std::to_string(x2)+" "+std::to_string(z2));}
   else hit->recorded=true;}}
 st.pass.clear();BfPublish(st);}
inline void BaseFinderTick(){auto& s=BaseFinder();if(s.fault)return;__try{BaseFinderWork();}__except(EXCEPTION_EXECUTE_HANDLER){InterlockedExchange(&s.fault,1);}}
inline void BaseFinderSetBlock(uint64_t sub,uint64_t block){auto& s=BaseFinder();if(s.fault)return;bool ages;{std::lock_guard<std::mutex> l(s.guard);if(!s.cfg.enabled)return;ages=s.cfg.chunkAges;}
 std::lock_guard<std::mutex> l(s.pendGuard);if(s.dirty.size()<8192)s.dirty.push_back(sub);
 if(ages&&block){uint64_t legacy{};uint8_t bits{};Context c{};if(Read(block+0x68,&legacy)&&legacy&&Read(legacy+0x29,&bits)&&(bits&2)&&Client(&c)&&BfIntState(c,block,"liquid_depth")!=0&&s.flowing.size()<8192)s.flowing.push_back(sub);}}
}
