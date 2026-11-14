// Schematics: a single manager running on the game thread (tick hook #249).
// - `.schem` command: create <name> <x y z> <x y z> / load <name> / unload [name] / list / folder;
// actions that touch the world go through a queue run in the tick; integer coordinates, `~`, `~n`;
// - capture: checks (name 1..48 [A-Za-z0-9_-], busy, exists, world, heights, 4,194,304 blocks), missing
// chunks, sub-chunk layers 0 and 1, palette {name, states, version}, 3 ms per tick,
// uncompressed little-endian .mcstructure file in %APPDATA%\Nickel\schematics;
// - load: 64 MiB max, format detected by content (game_nbt.hpp), anchored at the saved origin when it is
// within 256 blocks in X and Z of the player, otherwise at the player; several schematics;
// loading one already loaded selects it;
// - resolution on every level change: name -> block family (item registry, item+0x178), variant whose states match best;
// unresolved types are counted ("They show in red");
// - move (step 1), Move to me, rotation 0/90/180/270, X / Z mirrors, oriented states transformed (pillar_axis,
// portal_axis, cardinal_direction, facing_direction, block_face, torch_facing_direction, weirdo_direction);
// - ghost classification (5 ms per tick): unresolved -> red, Hide placed + identical -> hidden, wrong block (world block
// not replaceable) -> red, otherwise blue ghost; layers All / Only one / Up to;
// - auto place: Auto place + right button held (the game's own placement is cancelled, BuildBlock #28),
// Place delay, reach min(game mode, Place range), replaceable candidates sorted by distance, Placements per tick,
// hotbar slot holding the block (switched only for the first candidate), place + arm swing;
// Ghost place uses the action queue.
// Ghosts are drawn as world-space edges (menu.cpp) limited to nearby blocks; unresolved blocks (no model) are drawn as
// red edges; only block families that have an item in the item registry can be resolved; the 0.35 tint is applied
// through the shader colour.
#pragma once
#include "game_nbt.hpp"
#include "game_block_esp.hpp"
#include "game_actions.hpp"
#include "game_sus.hpp"
#include <array>
#include <set>
#include <ShlObj.h>
#include <shellapi.h>
#include <deque>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <memory>

namespace game {
struct ScGhost{int8_t x,y,z;uint8_t style;uint64_t block;};   // style 0 ghost, 1 error
struct ScSchem{int id=0;std::string name,file;nbt::Data data;int px=0,py=0,pz=0;int rot=0;bool mx=false,mz=false;bool show=true;int layerMode=0,layer=0;
 std::vector<uint64_t> resolved;std::vector<uint8_t> isAir;int unresolvedTypes=0;uint64_t resolvedLevel=0;bool warn=false;
 std::map<uint64_t,std::vector<std::pair<uint16_t,int32_t>>> buckets;bool bucketsReady=false;   // World sub-chunk -> (12-bit local position, palette index)
 uint64_t transformVersion=1;};
struct ScFile{std::string name;std::wstring path;uint64_t size=0;std::string ext;};
struct ScCapture{bool active=false;std::string name;int lo[3]{},hi[3]{};std::vector<std::array<int,3>> subs;size_t next=0;bool checked=false;int16_t minY=0;
 nbt::Data data;std::unordered_map<uint64_t,int32_t> palette;int64_t version=-1;};
struct ScRenderSub{int x,y,z;uint64_t version;std::vector<BeBlock> blocks;};
struct ScShared{std::recursive_mutex guard;std::vector<std::unique_ptr<ScSchem>> list;int selected=0,nextId=1;std::deque<std::function<void(const Context&)>> queue;std::deque<std::string> messages;
 ScCapture cap;float capProgress=0;bool hidePlaced=true,outline=true,info=true,autoPlace=false,ghost=false;float range=4.5f;int delay=75,perTick=1;std::atomic<bool> rightHeld{false};
 bool held=false;int64_t lastPlace=0;bool hasPlace=false;std::atomic<bool> cancelBuild{false};
 std::shared_ptr<std::vector<ScRenderSub>> render;std::vector<std::array<int,3>> red,frames;std::vector<std::array<int,6>> boxes;std::string infoName,infoWrong;int infoPos[3]{};bool infoValid=false;
 std::map<uint64_t,ScRenderSub> classified;std::vector<uint64_t> dirty;std::mutex dirtyGuard;size_t robin=0;uint64_t serial=0;volatile long fault=0;
 std::vector<ScFile> files;double filesAt=0;
 bool wantMaterials=false;int materialsFor=0;std::map<std::string,int> materialNeeded,materialHave;size_t materialLeft=0;};
inline ScShared& Schematics(){static auto* s=new ScShared;return *s;}
inline std::wstring ScFolder(){PWSTR base=nullptr;std::wstring out;if(SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData,0,nullptr,&base))&&base){out=std::wstring(base)+L"\\Nickel\\schematics";}if(base)CoTaskMemFree(base);return out;}
inline std::string ScNarrow(const std::wstring& w){std::string s;for(wchar_t c:w)s.push_back(c<128?(char)c:'?');return s;}
inline std::string ScLower(std::string s){for(auto& c:s)c=(char)tolower((unsigned char)c);return s;}
inline void ScSay(const std::string& text){auto& s=Schematics();std::lock_guard<std::recursive_mutex> l(s.guard);if(s.messages.size()<64)s.messages.push_back(text);}
inline std::vector<ScFile> ScListFiles(){std::vector<ScFile> out;const std::wstring dir=ScFolder();if(dir.empty())return out;std::error_code ec;
 for(auto& e:std::filesystem::directory_iterator(dir,ec)){if(!e.is_regular_file(ec))continue;std::string ext=ScLower(ScNarrow(e.path().extension().wstring()));if(!ext.empty()&&ext[0]=='.')ext=ext.substr(1);
  if(ext!="nbt"&&ext!="schem"&&ext!="litematic"&&ext!="mcstructure")continue;ScFile f;f.name=ScNarrow(e.path().stem().wstring());f.path=e.path().wstring();f.size=(uint64_t)e.file_size(ec);f.ext=ext;out.push_back(f);}
 std::sort(out.begin(),out.end(),[](const ScFile& a,const ScFile& b){return ScLower(a.name)<ScLower(b.name);});return out;}
inline bool ScValidName(const std::string& n){if(n.empty()||n.size()>48)return false;for(char c:n)if(!isalnum((unsigned char)c)&&c!='_'&&c!='-')return false;return true;}
// ---- Game states -> NBT (compound *(block+0xf0): "name", "states", "version")
inline bool ScGameString(uint64_t at,std::string* out){return SusString(at,out);}
inline void ScGameCompound(const Context& c,uint64_t compound,nbt::Tag& out,int depth=0){out=nbt::MakeCompound();if(depth>8)return;uint64_t head{},root{};if(!Read(compound+8,&head)||!head||!Read(head+8,&root))return;
 std::vector<uint64_t> stack;uint64_t node=root;int guard=0;
 while((node&&node!=head)||!stack.empty()){if(++guard>4096)break;while(node&&node!=head){uint8_t nil{};if(!Read(node+0x19,&nil)||nil)break;stack.push_back(node);uint64_t left{};if(!Read(node,&left))break;node=left;}
  if(stack.empty())break;node=stack.back();stack.pop_back();std::string key;ScGameString(node+0x20,&key);const uint64_t tag=node+0x40;const uint64_t idOf=Virtual(c,tag,0x28);uint8_t type=idOf?reinterpret_cast<uint8_t(*)(uint64_t)>(idOf)(tag):0;nbt::Tag v;v.type=type;
  switch(type){case nbt::Byte:{int8_t b{};Read(tag+8,&b);v.i=b;break;}case nbt::Short:{int16_t b{};Read(tag+8,&b);v.i=b;break;}case nbt::Int:{int32_t b{};Read(tag+8,&b);v.i=b;break;}case nbt::Long:{int64_t b{};Read(tag+8,&b);v.i=b;break;}
   case nbt::Float:{float f{};Read(tag+8,&f);v.d=f;break;}case nbt::String:ScGameString(tag+8,&v.s);break;case nbt::Compound:ScGameCompound(c,tag,v,depth+1);break;default:v.type=nbt::End;}
  if(v.type!=nbt::End)out.comp.push_back({key,v});uint64_t right{};if(!Read(node+0x10,&right))break;node=right;}}
inline nbt::BlockEntry ScEntryOf(const Context& c,uint64_t block){nbt::BlockEntry e;e.states=nbt::MakeCompound();uint64_t legacy{};char name[96]{};if(Read(block+0x68,&legacy)&&legacy&&Text(legacy+0xe8,name,sizeof(name)))e.name=name;
 nbt::Tag all;ScGameCompound(c,block+0xf0,all);if(auto s=all.get("states"))e.states=*s;if(auto v=all.get("version"))e.version=(int32_t)v->i;if(e.name.empty())if(auto n=all.get("name"))e.name=n->s;return e;}
// ---- Resolution: block families from the item registry, variants by states
inline int scPermOffset=-1;
inline bool ScPermutations(uint64_t legacy,uint64_t* begin,uint64_t* end){if(scPermOffset<0){for(int off=0x8;off<0x600;off+=8){uint64_t b{},e{},p{},back{};
   if(!Read(legacy+off,&b)||!Read(legacy+off+8,&e)||!b||e<=b||(e-b)%8||(e-b)>8*65536||!Read(b,&p)||!p||!Read(p+0x68,&back)||back!=legacy)continue;scPermOffset=off;break;}if(scPermOffset<0)return false;}
 return Read(legacy+scPermOffset,begin)&&Read(legacy+scPermOffset+8,end)&&*begin&&*end>=*begin;}
inline bool ScSameValue(const nbt::Tag& a,const nbt::Tag& b){if(a.type==nbt::String||b.type==nbt::String)return a.s==b.s||(a.type!=nbt::String&&std::to_string(a.i)==b.s)||(b.type!=nbt::String&&std::to_string(b.i)==a.s);return a.i==b.i;}
inline void ScResolve(const Context& c,ScSchem& sc){std::unordered_map<std::string,uint64_t> families;const uint64_t registry=ItemRegistry(c);uint64_t head{},node{};
 if(registry&&Read(registry+0x98,&head)&&head&&Read(head,&node))for(int v=0;node&&node!=head&&v<16384;++v){uint64_t holder{},item{},block{};char name[96]{};
  if(Read(node+0x40,&holder)&&holder&&Read(holder,&item)&&item&&Read(item+0x178,&block)&&block&&Text(block+0xe8,name,sizeof(name))&&name[0])families.emplace(name,block);if(!Read(node,&node))break;}
 sc.resolved.assign(sc.data.palette.size(),0);sc.isAir.assign(sc.data.palette.size(),0);sc.unresolvedTypes=0;
 for(size_t i=0;i<sc.data.palette.size();++i){const auto& want=sc.data.palette[i];if(want.name=="minecraft:air"||want.name=="minecraft:structure_void"){sc.isAir[i]=1;continue;}
  auto f=families.find(want.name);uint64_t b{},e{};if(f==families.end()||!ScPermutations(f->second,&b,&e)){++sc.unresolvedTypes;continue;}int best=-1;uint64_t pick=0;
  for(uint64_t at=b;at<e&&at<b+8*4096;at+=8){uint64_t block{};if(!Read(at,&block)||!block)continue;nbt::Tag all;ScGameCompound(c,block+0xf0,all);const nbt::Tag* st=all.get("states");int score=0;
   for(auto& [k,v]:want.states.comp){if(!st)break;const nbt::Tag* g=st->get(k);if(g&&ScSameValue(*g,v))++score;}if(score>best){best=score;pick=block;}}
  if(pick)sc.resolved[i]=pick;else ++sc.unresolvedTypes;}}
// ---- Transformations
inline void ScDims(const ScSchem& sc,int* w,int* h,int* d){*h=sc.data.sy;if(sc.rot&1){*w=sc.data.sz;*d=sc.data.sx;}else{*w=sc.data.sx;*d=sc.data.sz;}}
inline void ScToWorld(const ScSchem& sc,int x,int y,int z,int* out){const int sx=sc.data.sx,sz=sc.data.sz;if(sc.mx)x=sx-1-x;if(sc.mz)z=sz-1-z;int tx=x,tz=z;
 switch(sc.rot&3){case 1:tx=sz-1-z;tz=x;break;case 2:tx=sx-1-x;tz=sz-1-z;break;case 3:tx=z;tz=sx-1-x;break;default:break;}out[0]=sc.px+tx;out[1]=sc.py+y;out[2]=sc.pz+tz;}
inline bool ScToLocal(const ScSchem& sc,const int* w,int* out){const int sx=sc.data.sx,sz=sc.data.sz;const int tx=w[0]-sc.px,y=w[1]-sc.py,tz=w[2]-sc.pz;int x=tx,z=tz;
 switch(sc.rot&3){case 1:x=tz;z=sz-1-tx;break;case 2:x=sx-1-tx;z=sz-1-tz;break;case 3:x=sx-1-tz;z=tx;break;default:break;}if(sc.mx)x=sx-1-x;if(sc.mz)z=sz-1-z;
 if(x<0||y<0||z<0||x>=sx||y>=sc.data.sy||z>=sz)return false;out[0]=x;out[1]=y;out[2]=z;return true;}
inline bool ScLayerOk(const ScSchem& sc,int y){return sc.layerMode==0||(sc.layerMode==1&&y==sc.layer)||(sc.layerMode==2&&y<=sc.layer);}
// Oriented states (clockwise rotation seen from above, then mirrors)
inline std::string ScTurnDir(const std::string& v,int rot,bool mx,bool mz){static const char* ring[4]={"north","east","south","west"};std::string r=v;
 if(mx){if(r=="east")r="west";else if(r=="west")r="east";}if(mz){if(r=="north")r="south";else if(r=="south")r="north";}
 for(int i=0;i<4;++i)if(r==ring[i]){r=ring[(i+rot)&3];break;}return r;}
inline nbt::Tag ScTurnStates(const nbt::Tag& in,int rot,bool mx,bool mz){if(!rot&&!mx&&!mz)return in;nbt::Tag out=in;
 for(auto& [k,v]:out.comp){if((k=="pillar_axis"||k=="portal_axis")&&v.type==nbt::String&&(rot&1)){if(v.s=="x")v.s="z";else if(v.s=="z")v.s="x";}
  else if((k=="minecraft:cardinal_direction"||k=="minecraft:facing_direction"||k=="minecraft:block_face"||k=="torch_facing_direction")&&v.type==nbt::String)v.s=ScTurnDir(v.s,rot,mx,mz);
  else if(k=="facing_direction"&&v.type==nbt::Int){static const char* n[6]={"down","up","north","south","west","east"};if(v.i>=2&&v.i<=5){const std::string t=ScTurnDir(n[v.i],rot,mx,mz);for(int i=2;i<6;++i)if(t==n[i])v.i=i;}}
  else if(k=="weirdo_direction"&&v.type==nbt::Int){static const char* n[4]={"east","west","south","north"};if(v.i>=0&&v.i<4){const std::string t=ScTurnDir(n[v.i],rot,mx,mz);for(int i=0;i<4;++i)if(t==n[i])v.i=i;}}
  else if(k=="direction"&&v.type==nbt::Int){static const char* n[4]={"south","west","north","east"};if(v.i>=0&&v.i<4){const std::string t=ScTurnDir(n[v.i],rot,mx,mz);for(int i=0;i<4;++i)if(t==n[i])v.i=i;}}}
 return out;}
// Effective palette: rotated states, resolved again (rotated variant)
inline void ScApplyTransform(const Context& c,ScSchem& sc,const nbt::Data& original){sc.data.palette=original.palette;for(auto& p:sc.data.palette)p.states=ScTurnStates(p.states,sc.rot,sc.mx,sc.mz);ScResolve(c,sc);sc.bucketsReady=false;++sc.transformVersion;}
inline uint64_t ScSubKey(int x,int y,int z){return BeKey(x,y,z);}
inline void ScBuildBuckets(ScSchem& sc){sc.buckets.clear();const auto& d=sc.data;for(int x=0;x<d.sx;++x)for(int y=0;y<d.sy;++y)for(int z=0;z<d.sz;++z){const int32_t idx=d.l0[nbt::Index(d,x,y,z)];
  if(idx<0||idx>=(int32_t)sc.isAir.size()||sc.isAir[idx])continue;int w[3];ScToWorld(sc,x,y,z,w);const uint16_t local=(uint16_t)(((w[0]&15)<<8)|((w[2]&15)<<4)|(w[1]&15));
  sc.buckets[ScSubKey(w[0],w[1],w[2])].push_back({local,idx|(y<<16)});}   // palette index (low 16 bits) and schematic layer y
 sc.bucketsReady=true;}
inline std::map<int,nbt::Data>& ScOriginals(){static auto* m=new std::map<int,nbt::Data>;return *m;}
// ---- Commands (chat thread -> tick queue)
inline bool ScParseCoord(const std::string& w,int* value,bool* relative){if(w.empty())return false;if(w[0]=='~'){*relative=true;if(w.size()==1){*value=0;return true;}char* e{};const long v=strtol(w.c_str()+1,&e,10);if(*e)return false;*value=(int)v;return true;}
 *relative=false;char* e{};const long v=strtol(w.c_str(),&e,10);if(*e)return false;*value=(int)v;return true;}
inline void ScLoad(const std::string& name);inline void ScUnload(int id);inline void ScUnloadAll();inline void ScOpenFolder();
inline void ScCreate(const std::string& name,const int* a,const bool* ra,const int* b,const bool* rb);
inline bool SchematicCommand(const std::string& line){if(line.size()<6||line.compare(0,6,".schem")!=0||(line.size()>6&&line[6]!=' '&&line[6]!='\t'))return false;
 std::vector<std::string> w;{size_t at=6;while(at<line.size()){while(at<line.size()&&isspace((unsigned char)line[at]))++at;size_t e=at;while(e<line.size()&&!isspace((unsigned char)line[e]))++e;if(e>at)w.push_back(line.substr(at,e-at));at=e;}}
 if(w.empty()){ScSay("Incomplete command");return true;}const std::string sub=ScLower(w[0]);
 if(sub=="create"){if(w.size()<8){ScSay(w.size()<2?"Incomplete command":"Expected three coordinates");return true;}if(w.size()>8){ScSay("Unexpected argument");return true;}
  if(!ScValidName(w[1])){ScSay("Schematic names can only use letters, numbers, '-' and '_' (up to 48 characters).");return true;}int a[3],b[3];bool ra[3],rb[3];
  for(int i=0;i<3;++i)if(!ScParseCoord(w[2+i],&a[i],&ra[i])||!ScParseCoord(w[5+i],&b[i],&rb[i])){ScSay("Expected a coordinate such as 12 or ~-3");return true;}ScCreate(w[1],a,ra,b,rb);return true;}
 if(sub=="load"){if(w.size()<2){ScSay("Incomplete command");return true;}if(w.size()>2){ScSay("Unexpected argument");return true;}bool found=false;for(auto& f:ScListFiles())if(ScLower(f.name)==ScLower(w[1]))found=true;
  if(!found){ScSay("No schematic named '"+w[1]+"'. Use .schem list to see them.");return true;}ScLoad(w[1]);return true;}
 if(sub=="unload"){auto& s=Schematics();std::lock_guard<std::recursive_mutex> l(s.guard);if(w.size()==1){if(s.list.empty()){ScSay("No schematic is loaded.");return true;}ScUnloadAll();return true;}
  for(auto& sc:s.list)if(ScLower(sc->name)==ScLower(w[1])){ScUnload(sc->id);return true;}ScSay("'"+w[1]+"' isn't loaded.");return true;}
 if(sub=="list"){const auto files=ScListFiles();if(files.empty()){ScSay("No schematics yet. Save one with .schem create <name> <x y z> <x y z>.");return true;}
  ScSay("- \xc2\xa7""lSCHEMATICS ("+std::to_string(files.size())+") \xc2\xa7""r\xc2\xa7""7-");for(auto& f:files)ScSay(f.name+" \xc2\xa7""8("+std::to_string((f.size+1023)>>10)+" KB)");return true;}
 if(sub=="folder"){ScOpenFolder();return true;}
 ScSay("Unexpected argument");return true;}
inline void ScOpenFolder(){const std::wstring dir=ScFolder();if(dir.empty()){ScSay("Couldn't open the schematics folder.");return;}std::error_code ec;std::filesystem::create_directories(dir,ec);
 if((INT_PTR)ShellExecuteW(nullptr,L"open",L"explorer.exe",(L"\""+dir+L"\"").c_str(),nullptr,SW_SHOWNORMAL)<=32)ScSay("Couldn't open the schematics folder.");}
inline void ScCreate(const std::string& name,const int* a,const bool* ra,const int* b,const bool* rb){auto& s=Schematics();std::array<int,3> A{a[0],a[1],a[2]},B{b[0],b[1],b[2]};std::array<bool,3> RA{ra[0],ra[1],ra[2]},RB{rb[0],rb[1],rb[2]};
 std::lock_guard<std::recursive_mutex> l(s.guard);
 s.queue.push_back([name,A,B,RA,RB](const Context& c){auto& s=Schematics();if(s.cap.active){ScSay("Still saving '"+s.cap.name+"'. Wait for it to finish.");return;}
  for(auto& f:ScListFiles())if(ScLower(f.name)==ScLower(name)){ScSay("A schematic named '"+name+"' already exists.");return;}
  const uint64_t region=ActorRegion(c);float me[3]{};if(!region||!ActionPosition(c.actor,me)){ScSay("Join a world first.");return;}uint64_t state{};float feet[3]{},lift=0;Read(c.actor+0x240,&state);if(state)Floats(state,feet,3);
  uintptr_t off{};if(Element(c.registry,c.entity,"OffsetsComponent",0x44,&off))Floats(off,&lift,1);const int p[3]={(int)std::floor(feet[0]),(int)std::floor(feet[1]-lift),(int)std::floor(feet[2])};
  int lo[3],hi[3];for(int i=0;i<3;++i){const int x=RA[i]?p[i]+A[i]:A[i],y=RB[i]?p[i]+B[i]:B[i];lo[i]=std::min(x,y);hi[i]=std::max(x,y);}
  int16_t minH{},maxH{};Read(region+0x3a,&minH);Read(region+0x38,&maxH);lo[1]=std::max(lo[1],(int)minH);hi[1]=std::min(hi[1],(int)maxH-1);if(hi[1]<lo[1]){ScSay("That region is outside the world's height limits.");return;}
  const uint64_t total=(uint64_t)(hi[0]-lo[0]+1)*(hi[1]-lo[1]+1)*(hi[2]-lo[2]+1);if(total>0x400000){ScSay("That region is too large. Schematics can hold up to 4194304 blocks.");return;}
  ScSay("Saving '"+name+"'...");auto& cap=s.cap;cap=ScCapture{};cap.active=true;cap.name=name;memcpy(cap.lo,lo,12);memcpy(cap.hi,hi,12);cap.minY=minH;
  for(int cx=lo[0]>>4;cx<=hi[0]>>4;++cx)for(int cz=lo[2]>>4;cz<=hi[2]>>4;++cz)for(int sy=(lo[1]-minH)>>4;sy<=(hi[1]-minH)>>4;++sy)cap.subs.push_back({cx,cz,sy});
  auto& d=cap.data;d.sx=hi[0]-lo[0]+1;d.sy=hi[1]-lo[1]+1;d.sz=hi[2]-lo[2]+1;d.ox=lo[0];d.oy=lo[1];d.oz=lo[2];d.hasOrigin=true;
  nbt::BlockEntry air;air.name="minecraft:air";air.states=nbt::MakeCompound();d.palette.push_back(air);d.l0.assign((size_t)total,0);d.l1.assign((size_t)total,-1);s.capProgress=0;});}
// Reads one layer: Block* per position 0..4095 (x<<8 | z<<4 | y)
inline bool ScReadLayer(const Context& c,uint64_t storage,std::vector<uint64_t>& out){out.clear();if(!storage)return false;const uint64_t palette=Virtual(c,storage,0xa8),bitsOf=Virtual(c,storage,0xa0),wordsOf=Virtual(c,storage,0x90);if(!palette||!bitsOf||!wordsOf)return false;
 uint64_t p[2]{};reinterpret_cast<void*(*)(uint64_t,uint64_t*)>(palette)(storage,p);if(!p[0]||!p[1])return false;const uint32_t n=(uint32_t)std::min<uint64_t>(p[0],4096);std::vector<uint64_t> blocks(n);for(uint32_t i=0;i<n;++i)Read(p[1]+8ull*i,&blocks[i]);
 out.assign(4096,0);const int bits=reinterpret_cast<int(*)(uint64_t)>(bitsOf)(storage);if(bits<=0){for(auto& v:out)v=blocks[0];return true;}if(bits>32)return false;
 uint64_t words[2]{};reinterpret_cast<void*(*)(uint64_t,uint64_t*)>(wordsOf)(storage,words);if(!words[0]||!words[1])return false;const uint32_t per=32/bits,mask=bits==32?0xffffffffu:((1u<<bits)-1);
 for(uint32_t at=0;at<4096;++at){const uint32_t w=at/per;uint32_t v{};if(w>=words[0]||!Read(words[1]+4ull*w,&v))continue;const uint32_t pick=(v>>((at%per)*bits))&mask;if(pick<n)out[at]=blocks[pick];}return true;}
inline void ScCaptureStep(const Context& c){auto& s=Schematics();auto& cap=s.cap;if(!cap.active)return;const uint64_t region=ActorRegion(c);const uint64_t chunkAt=region?Virtual(c,region,0x148):0;
 if(!chunkAt){cap.active=false;ScSay("Join a world first.");return;}
 if(!cap.checked){int missing=0;std::set<std::pair<int,int>> seen;for(auto& sub:cap.subs){if(!seen.insert({sub[0],sub[1]}).second)continue;int cp[2]={sub[0],sub[1]};if(!reinterpret_cast<uint64_t(*)(uint64_t,int*)>(chunkAt)(region,cp))++missing;}
  if(missing){cap.active=false;ScSay(std::to_string(missing)+" chunks in that region aren't loaded. Move closer and try again.");return;}cap.checked=true;}
 const int64_t t0=ActionNs();auto& d=cap.data;std::vector<uint64_t> l0,l1;
 while(cap.next<cap.subs.size()&&(double)(ActionNs()-t0)/1e6<3.0){const auto sub=cap.subs[cap.next++];int cp[2]={sub[0],sub[1]};const uint64_t chunk=reinterpret_cast<uint64_t(*)(uint64_t,int*)>(chunkAt)(region,cp);
  if(!chunk){cap.active=false;ScSay("1 chunks in that region aren't loaded. Move closer and try again.");return;}uint64_t b{},e{};if(!Read(chunk+0x1c8,&b)||!Read(chunk+0x1d0,&e)||e<b)continue;
  if((uint64_t)sub[2]>=(e-b)/0x68)continue;const uint64_t address=b+0x68ull*sub[2];uint64_t s0{},s1{};Read(address+0x30,&s0);Read(address+0x38,&s1);ScReadLayer(c,s0,l0);ScReadLayer(c,s1,l1);
  for(int layer=0;layer<2;++layer){const auto& blocks=layer?l1:l0;if(blocks.empty())continue;for(uint32_t at=0;at<4096;++at){const uint64_t block=blocks[at];if(!block)continue;
    const int wx=sub[0]*16+(int)(at>>8),wz=sub[1]*16+(int)((at>>4)&15),wy=cap.minY+sub[2]*16+(int)(at&15);if(wx<cap.lo[0]||wx>cap.hi[0]||wy<cap.lo[1]||wy>cap.hi[1]||wz<cap.lo[2]||wz>cap.hi[2])continue;
    auto f=cap.palette.find(block);int32_t idx;if(f==cap.palette.end()){nbt::BlockEntry entry=ScEntryOf(c,block);if(entry.name=="minecraft:air"){cap.palette.emplace(block,0);idx=0;}else{idx=(int32_t)d.palette.size();if(cap.version<0)cap.version=entry.version;d.palette.push_back(std::move(entry));cap.palette.emplace(block,idx);}}else idx=f->second;
    const size_t i=nbt::Index(d,wx-cap.lo[0],wy-cap.lo[1],wz-cap.lo[2]);if(layer==0)d.l0[i]=idx;else if(idx!=0)d.l1[i]=idx;}}}
 s.capProgress=cap.subs.empty()?1.f:(float)cap.next/(float)cap.subs.size();if(cap.next<cap.subs.size())return;
 if(cap.version>=0)d.palette[0].version=(int32_t)cap.version;const std::wstring dir=ScFolder();std::error_code ec;std::filesystem::create_directories(dir,ec);cap.active=false;
 if(dir.empty()){ScSay("Couldn't open the schematics folder.");return;}const std::wstring path=dir+L"\\"+std::wstring(cap.name.begin(),cap.name.end())+L".mcstructure";
 if(std::filesystem::exists(path,ec)){ScSay("A schematic named '"+cap.name+"' already exists.");return;}const auto bytes=nbt::EncodeMcstructure(d);std::ofstream out(path,std::ios::binary);
 if(!out||!out.write((const char*)bytes.data(),(std::streamsize)bytes.size())){ScSay("Couldn't write '"+cap.name+"'.");return;}size_t count=0;for(int32_t v:d.l0)if(v>0)++count;
 ScSay("Saved '"+cap.name+"' ("+std::to_string(d.sx)+" \xc3\x97 "+std::to_string(d.sy)+" \xc3\x97 "+std::to_string(d.sz)+", "+std::to_string(count)+" blocks).");s.filesAt=0;}
inline void ScLoad(const std::string& name){auto& s=Schematics();std::lock_guard<std::recursive_mutex> l(s.guard);
 s.queue.push_back([name](const Context& c){auto& s=Schematics();if(!ActorRegion(c)){ScSay("Join a world first.");return;}for(auto& sc:s.list)if(ScLower(sc->name)==ScLower(name)){s.selected=sc->id;return;}
  ScFile file;bool found=false;for(auto& f:ScListFiles())if(ScLower(f.name)==ScLower(name)){file=f;found=true;}if(!found){ScSay("No schematic named '"+name+"'. Use .schem list to see them.");return;}
  if(file.size>0x4000000){ScSay("Couldn't read '"+name+"'. The file is damaged or isn't a supported schematic.");return;}std::ifstream in(file.path,std::ios::binary);std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(in)),std::istreambuf_iterator<char>());
  auto sc=std::make_unique<ScSchem>();if(!nbt::Decode(bytes,sc->data)){ScSay("Couldn't read '"+name+"'. The file is damaged or isn't a supported schematic.");return;}
  sc->id=s.nextId++;sc->name=file.name;sc->file=ScNarrow(file.path);float me[3]{};uint64_t state{};float feet[3]{},lift=0;Read(c.actor+0x240,&state);if(state)Floats(state,feet,3);uintptr_t off{};if(Element(c.registry,c.entity,"OffsetsComponent",0x44,&off))Floats(off,&lift,1);(void)me;
  const int p[3]={(int)std::floor(feet[0]),(int)std::floor(feet[1]-lift),(int)std::floor(feet[2])};const auto& d=sc->data;
  if(d.hasOrigin&&(d.ox||d.oy||d.oz)&&std::abs(d.ox-p[0])<=256&&std::abs(d.oz-p[2])<=256){sc->px=d.ox;sc->py=d.oy;sc->pz=d.oz;}else{sc->px=p[0];sc->py=p[1];sc->pz=p[2];}
  ScOriginals()[sc->id]=sc->data;ScResolve(c,*sc);uint64_t level{};Read(c.actor+0x200,&level);sc->resolvedLevel=level;sc->warn=sc->unresolvedTypes>0;s.selected=sc->id;
  ScSay("Loaded '"+sc->name+"' ("+std::to_string(d.sx)+" \xc3\x97 "+std::to_string(d.sy)+" \xc3\x97 "+std::to_string(d.sz)+").");
  if(sc->unresolvedTypes)ScSay(std::to_string(sc->unresolvedTypes)+" block types in '"+sc->name+"' aren't available in this version. They show in red.");
  s.list.push_back(std::move(sc));std::lock_guard<std::mutex> dl(s.dirtyGuard);s.classified.clear();});}
inline void ScUnload(int id){auto& s=Schematics();std::lock_guard<std::recursive_mutex> l(s.guard);s.queue.push_back([id](const Context&){auto& s=Schematics();
  for(auto it=s.list.begin();it!=s.list.end();++it)if((*it)->id==id){ScSay("Unloaded '"+(*it)->name+"'.");ScOriginals().erase(id);s.list.erase(it);break;}s.classified.clear();});}
inline void ScUnloadAll(){auto& s=Schematics();std::lock_guard<std::recursive_mutex> l(s.guard);s.queue.push_back([](const Context&){auto& s=Schematics();for(auto& sc:s.list){ScSay("Unloaded '"+sc->name+"'.");ScOriginals().erase(sc->id);}s.list.clear();s.classified.clear();});}
// Changes requested from the UI (queued)
inline void ScEdit(int id,std::function<void(const Context&,ScSchem&)> f){auto& s=Schematics();std::lock_guard<std::recursive_mutex> l(s.guard);s.queue.push_back([id,f](const Context& c){auto& s=Schematics();for(auto& sc:s.list)if(sc->id==id){f(c,*sc);s.classified.clear();}});}
inline void ScTransform(int id,int rotDelta,bool flipX,bool flipZ){ScEdit(id,[rotDelta,flipX,flipZ](const Context& c,ScSchem& sc){sc.rot=(sc.rot+rotDelta+4)&3;if(flipX)sc.mx=!sc.mx;if(flipZ)sc.mz=!sc.mz;auto f=ScOriginals().find(sc.id);if(f!=ScOriginals().end())ScApplyTransform(c,sc,f->second);});}
// ---- Classification (5 ms per tick), nearby sub-chunks
inline void ScClassify(const Context& c){auto& s=Schematics();const uint64_t region=ActorRegion(c);if(!region)return;float me[3]{};if(!ActionPosition(c.actor,me))return;
 for(auto& sc:s.list){uint64_t level{};Read(c.actor+0x200,&level);if(level!=sc->resolvedLevel){auto f=ScOriginals().find(sc->id);if(f!=ScOriginals().end())ScApplyTransform(c,*sc,f->second);sc->resolvedLevel=level;s.classified.clear();}
  if(!sc->bucketsReady)ScBuildBuckets(*sc);}
 std::vector<uint64_t> dirty;{std::lock_guard<std::mutex> l(s.dirtyGuard);dirty.swap(s.dirty);}
 std::set<uint64_t> keys;for(auto& sc:s.list)if(sc->show)for(auto& b:sc->buckets)keys.insert(b.first);
 auto subOf=[](uint64_t key,int* out){auto ext=[](uint64_t v){return (int)((int64_t)(v<<43)>>43);};out[0]=ext((key>>42)&0x1fffff)*16;out[1]=ext((key>>21)&0x1fffff)*16;out[2]=ext(key&0x1fffff)*16;};
 for(auto it=s.classified.begin();it!=s.classified.end();)it=keys.count(it->first)?std::next(it):s.classified.erase(it);
 const int64_t t0=ActionNs();std::vector<uint64_t> order(keys.begin(),keys.end());if(order.empty())return;
 auto classify=[&](uint64_t key){int base[3];subOf(key,base);const float dx=base[0]+8-me[0],dy=base[1]+8-me[1],dz=base[2]+8-me[2];ScRenderSub out{base[0],base[1],base[2],0,{}};
  if(dx*dx+dy*dy+dz*dz<=128.f*128.f)for(auto& sc:s.list){if(!sc->show)continue;auto f=sc->buckets.find(key);if(f==sc->buckets.end())continue;
   for(auto& [local,packed]:f->second){const int idx=packed&0xffff,y=packed>>16;if(!ScLayerOk(*sc,y))continue;const int32_t pos[3]={base[0]+(local>>8),base[1]+(local&15),base[2]+((local>>4)&15)};
    const uint64_t want=idx<(int)sc->resolved.size()?sc->resolved[idx]:0;const uint64_t have=RegionBlock(c,region,pos);uint8_t style=0;
    if(!want)style=2;else if(have==want){if(s.hidePlaced)continue;}else if(!BlockReplaceable(have))style=1;
    out.blocks.push_back({(int8_t)(local>>8),(int8_t)(local&15),(int8_t)((local>>4)&15),style,want});}}
  auto& prev=s.classified[key];bool same=prev.blocks.size()==out.blocks.size();for(size_t i=0;same&&i<out.blocks.size();++i)same=prev.blocks[i].block==out.blocks[i].block&&prev.blocks[i].style==out.blocks[i].style&&prev.blocks[i].x==out.blocks[i].x&&prev.blocks[i].y==out.blocks[i].y&&prev.blocks[i].z==out.blocks[i].z;
  out.version=same?prev.version:++s.serial;prev=std::move(out);};
 for(uint64_t a:dirty)for(uint64_t key:order){int base[3];subOf(key,base);(void)a;}
 std::set<uint64_t> dirtyKeys;for(uint64_t sub:dirty){(void)sub;}   // Modified world sub-chunks: everything is revisited round-robin
 for(size_t n=0;n<order.size()&&(double)(ActionNs()-t0)/1e6<5.0;++n){s.robin=(s.robin+1)%order.size();classify(order[s.robin]);}
 auto snap=std::make_shared<std::vector<ScRenderSub>>();std::vector<std::array<int,3>> red;for(auto& [k,v]:s.classified){if(v.blocks.empty())continue;ScRenderSub r{v.x,v.y,v.z,v.version,{}};
  for(auto& b:v.blocks){if(b.style==2){if(red.size()<4096)red.push_back({v.x+b.x,v.y+b.y,v.z+b.z});continue;}r.blocks.push_back({b.x,b.y,b.z,b.style,b.block});}if(!r.blocks.empty())snap->push_back(std::move(r));}
 std::vector<std::array<int,6>> boxes;for(auto& sc:s.list){if(!sc->show)continue;int w,h,d;ScDims(*sc,&w,&h,&d);boxes.push_back({sc->px,sc->py,sc->pz,sc->px+w,sc->py+h,sc->pz+d});}
 s.render=snap;s.red.swap(red);s.boxes.swap(boxes);}
// ---- Block info: ray along the view direction to the first schematic block (64 blocks)
inline void ScInfo(const Context& c){auto& s=Schematics();s.infoValid=false;if(!s.info)return;ScSchem* sel=nullptr;for(auto& sc:s.list)if(sc->id==s.selected)sel=sc.get();if(!sel||!sel->bucketsReady)return;
 float eye[3]{},p{},y{};if(!ActionPosition(c.actor,eye)||!RotRead(c.actor,&p,&y))return;const float r=.017453292f;const float dir[3]={-std::sin(y*r)*std::cos(p*r),-std::sin(p*r),std::cos(y*r)*std::cos(p*r)};const uint64_t region=ActorRegion(c);
 for(float t=0;t<64;t+=.1f){const int w[3]={(int)std::floor(eye[0]+dir[0]*t),(int)std::floor(eye[1]+dir[1]*t),(int)std::floor(eye[2]+dir[2]*t)};int l[3];if(!ScToLocal(*sel,w,l))continue;
  const int32_t idx=sel->data.l0[nbt::Index(sel->data,l[0],l[1],l[2])];if(idx<0||idx>=(int32_t)sel->isAir.size()||sel->isAir[idx]||!ScLayerOk(*sel,l[1]))continue;
  s.infoName=sel->data.palette[idx].name;memcpy(s.infoPos,w,12);s.infoWrong.clear();const uint64_t want=sel->resolved[idx],have=region?RegionBlock(c,region,w):0;
  if(have&&have!=want&&!BlockReplaceable(have)){char name[96]{};uint64_t legacy{};if(Read(have+0x68,&legacy)&&legacy&&Text(legacy+0xe8,name,sizeof(name)))s.infoWrong=name;}s.infoValid=true;return;}}
// ---- Auto place
inline bool ScStackHolds(uint64_t stack,uint64_t want){uint64_t block{},a{},b{};return stack&&ActionItem(stack)&&Read(stack+0x18,&block)&&block&&Read(block+0x68,&a)&&Read(want+0x68,&b)&&a&&a==b;}
inline void ScAutoPlace(const Context& c){auto& s=Schematics();if(!s.autoPlace||!s.rightHeld.load()){s.held=false;s.cancelBuild=false;return;}s.held=true;s.cancelBuild=true;
 if(s.hasPlace&&ActionMs(s.lastPlace)<=(double)s.delay)return;const float reach=AbrReach(c,c.actor);const float range=reach>0?std::min(reach,s.range):s.range;float eye[3]{};if(!ActionPosition(c.actor,eye))return;
 const uint64_t region=ActorRegion(c);if(!region)return;struct Cand{int32_t p[3];uint64_t block;float d;};std::vector<Cand> cands;const int R=(int)std::ceil(range)+1;
 for(auto& sc:s.list){if(!sc->show)continue;for(int dx=-R;dx<=R;++dx)for(int dy=-R;dy<=R;++dy)for(int dz=-R;dz<=R;++dz){const int w[3]={(int)std::floor(eye[0])+dx,(int)std::floor(eye[1])+dy,(int)std::floor(eye[2])+dz};
   const float cx=w[0]+.5f-eye[0],cy=w[1]+.5f-eye[1],cz=w[2]+.5f-eye[2];const float d2=cx*cx+cy*cy+cz*cz;if(d2>(range+.5f)*(range+.5f))continue;int l[3];if(!ScToLocal(*sc,w,l)||!ScLayerOk(*sc,l[1]))continue;
   const int32_t idx=sc->data.l0[nbt::Index(sc->data,l[0],l[1],l[2])];if(idx<0||idx>=(int32_t)sc->resolved.size()||sc->isAir[idx]||!sc->resolved[idx])continue;const uint64_t have=RegionBlock(c,region,w);
   if(have==sc->resolved[idx]||!BlockReplaceable(have))continue;cands.push_back({{w[0],w[1],w[2]},sc->resolved[idx],d2});}}
 std::sort(cands.begin(),cands.end(),[](const Cand& a,const Cand& b){return a.d<b.d;});const uint64_t box=ActionContainer(c.actor);const int n=std::min(9,ActionSize(c,box));int32_t* sel=ActionSelected(c.actor);if(!sel)return;
 int placed=0;
 if(!s.ghost){for(size_t i=0;i<cands.size()&&placed<s.perTick;++i){int slot=-1;if(ScStackHolds(ActionStackAt(c,box,*sel),cands[i].block))slot=*sel;else for(int k=0;k<n;++k)if(ScStackHolds(ActionStackAt(c,box,k),cands[i].block)){slot=k;break;}
   if(slot<0)continue;if(slot!=*sel){if(placed==0){*sel=slot;s.lastPlace=ActionNs();s.hasPlace=true;}return;}AbrPlace place{};if(!AbrFind(c,c.actor,cands[i].p,range,&place))continue;
   if(AbrPlaceBlock(c,c.actor,place,range)){ActionSwing(c,c.actor,1);++placed;s.lastPlace=ActionNs();s.hasPlace=true;}}}
 else{ActionSequence q;q.module=11;q.silent=1;q.conditions.push_back([](uintptr_t,int64_t start){return ActionMs(start)<=1000.0;});
  for(size_t i=0;i<cands.size()&&placed<s.perTick;++i){AbrPlace place{};if(!AbrFind(c,c.actor,cands[i].p,range,&place))continue;const uint64_t want=cands[i].block;
   q.steps.push_back([want](ActionCtx& x){const uint64_t b=ActionContainer(x.actor);const int m=std::min(9,ActionSize(*x.c,b));for(int k=0;k<m;++k)if(ScStackHolds(ActionStackAt(*x.c,b,k),want)){ActionSwitch(x,k);return 1;}return 2;});
   q.steps.push_back(StepAbrPlace(place,range,false));++placed;}
  if(placed){q.steps.push_back(StepSwitchBack());ActionSubmit(std::move(q));s.lastPlace=ActionNs();s.hasPlace=true;}}}
// Material list computed on the game thread (reads world blocks), read by the UI
inline void ScMaterialsCount(const Context& c){auto& s=Schematics();if(!s.wantMaterials)return;static int every=0;if(++every<10)return;every=0;ScSchem* sc=nullptr;for(auto& x:s.list)if(x->id==s.materialsFor)sc=x.get();if(!sc)return;
 std::map<std::string,int> needed,have;size_t left=0;const uint64_t region=ActorRegion(c);const auto& dt=sc->data;
 for(int x=0;x<dt.sx;++x)for(int y=0;y<dt.sy;++y)for(int z=0;z<dt.sz;++z){const int32_t idx=dt.l0[nbt::Index(dt,x,y,z)];if(idx<0||idx>=(int32_t)sc->isAir.size()||sc->isAir[idx])continue;
  if(region&&sc->resolved[idx]){int w[3];ScToWorld(*sc,x,y,z,w);if(RegionBlock(c,region,w)==sc->resolved[idx])continue;}++needed[dt.palette[idx].name];++left;}
 const uint64_t box=ActionContainer(c.actor);const int n=ActionSize(c,box);for(int i=0;i<n;++i){const uint64_t st=ActionStackAt(c,box,i);uint64_t blk{},legacy{};uint8_t cnt{};char nm[96]{};
  if(st&&ActionItem(st)&&Read(st+0x18,&blk)&&blk&&Read(blk+0x68,&legacy)&&legacy&&Text(legacy+0xe8,nm,sizeof(nm))&&Read(st+0x22,&cnt))have[nm]+=cnt;}
 s.materialNeeded.swap(needed);s.materialHave.swap(have);s.materialLeft=left;}
// ---- Tick
inline void SchematicTickWork(){Context c{};const bool have=Local(&c);if(have){uint64_t level{};Read(c.actor+0x200,&level);if((uint64_t)currentTickLevel!=level)return;}   // CLIENT level tick only
 auto& s=Schematics();std::lock_guard<std::recursive_mutex> l(s.guard);
 if(have){std::deque<std::function<void(const Context&)>> q;q.swap(s.queue);for(auto& f:q)f(c);ScCaptureStep(c);if(!s.list.empty()){ScClassify(c);ScInfo(c);ScAutoPlace(c);ScMaterialsCount(c);}else{s.render.reset();s.red.clear();s.boxes.clear();s.infoValid=false;s.cancelBuild=false;}
  while(!s.messages.empty()){ClientMessage(c,s.messages.front());s.messages.pop_front();}}}
inline void SchematicTick(){auto& s=Schematics();if(s.fault)return;__try{SchematicTickWork();}__except(EXCEPTION_EXECUTE_HANDLER){InterlockedExchange(&s.fault,1);if(actionLog)actionLog("Schematics: fault caught, schematics stopped until restart");}}
inline void SchematicSetBlock(uint64_t sub){auto& s=Schematics();std::lock_guard<std::mutex> l(s.dirtyGuard);if(s.dirty.size()<4096)s.dirty.push_back(sub);}
inline bool SchematicCancelBuild(){return Schematics().cancelBuild.load();}

// ---- Meshes and drawing (LevelRenderPrep, same building blocks as Block ESP)
// Ghost materials: as Block ESP but with "moving_block_blend" on layers 3 and 4
inline uint64_t scMaterials[17][2]{};inline bool scMaterialsReady=false;
inline void ScMaterials(const Context& c){if(scMaterialsReady)return;scMaterialsReady=true;
 static const char* names[17]={"moving_block_double_side","moving_block","moving_block","moving_block_blend","moving_block_blend","moving_block","moving_block","moving_block_alpha","moving_block_alpha","moving_block_seasons","moving_block_alpha_seasons","moving_block_alpha_single_side","moving_block_alpha_single_side","moving_block","moving_block","moving_block","moving_block"};
 const uint64_t factory=c.base+0x129c8110;const uint64_t get=Virtual(c,factory,8);if(!get){scMaterialsReady=false;return;}struct Hashed{uint64_t hash;std::string text;uint64_t last;};
 for(int i=0;i<17;++i){Hashed h{0xcbf29ce484222325ull,names[i],0};for(const char* q=names[i];*q;++q)h.hash=(h.hash*0x100000001b3ull)^(uint8_t)*q;uint64_t out[2]{};reinterpret_cast<void(*)(uint64_t,uint64_t*,Hashed*)>(get)(factory,out,&h);scMaterials[i][0]=out[0];scMaterials[i][1]=out[1];}}
inline volatile bool scSwap=false;
struct ScRenderState{std::unordered_map<uint64_t,BeMeshRec> subs;uint64_t serial=0;};
inline ScRenderState& ScRender(){static auto* r=new ScRenderState;return *r;}
inline void ScDraw(const Context& c,uint64_t screen,uint64_t renderer,const std::vector<ScRenderSub>& subs,const float camera[3],int style,const float* tint){
 auto& r=ScRender();uint64_t stack{};if(!Read(screen+0x18,&stack)||!stack)return;const uint64_t world=stack+0x40;float* top=BeTop(world);
 uint64_t tex[2]{};if(!Read(renderer+0x1d0,&tex[0]))return;const uint64_t getter=Virtual(c,c.client,0x5f0);uint64_t holder{};if(!top||!getter||!CallGetter(getter,c.client,&holder)||!holder||!tex[0]||!Read(holder+0x18,&tex[1])||!tex[1])return;
 const uint8_t saved7b=*(uint8_t*)(screen+0x7b);*(uint8_t*)(screen+0x7b)=1;uint64_t colorObj{};Read(screen+0x30,&colorObj);float savedColor[4]{1,1,1,1};
 if(colorObj){memcpy(savedColor,(void*)colorObj,16);memcpy((void*)colorObj,tint,16);*(uint8_t*)(colorObj+0x10)=1;}float M[16];memcpy(M,top,64);
 for(const auto& sub:subs){auto f=r.subs.find(BeKey(sub.x,sub.y,sub.z));if(f==r.subs.end())continue;bool placed=false;
  for(const auto& m:f->second.meshes){if(m.style!=style||!m.mesh)continue;if(!placed){const float x=(float)sub.x-camera[0],y=(float)sub.y-camera[1],z=(float)sub.z-camera[2];memcpy(top,M,48);
    top[12]=x*M[0]+y*M[4]+z*M[8]+M[12];top[13]=x*M[1]+y*M[5]+z*M[9]+M[13];top[14]=x*M[2]+y*M[6]+z*M[10]+M[14];top[15]=x*M[3]+y*M[7]+z*M[11]+M[15];*(uint8_t*)(world+0x38)=1;placed=true;}
   uint64_t swapped[2]={tex[1],tex[0]};struct Span{uint64_t n;uint64_t* p;} span{2,scSwap?swapped:tex};uint8_t zero[0x58]{};const uint8_t layer=m.layer>0x10?5:m.layer;
   reinterpret_cast<void(*)(void*,uint64_t,void*,Span*,uint64_t,uint64_t,void*)>(c.base+0x4deffe0)(m.mesh,screen+0x10,scMaterials[layer],&span,0,0,zero);}}
 memcpy(top,M,64);*(uint8_t*)(world+0x38)=1;*(uint8_t*)(screen+0x7b)=saved7b;if(colorObj){memcpy((void*)colorObj,savedColor,16);*(uint8_t*)(colorObj+0x10)=1;}}
inline void ScRenderWork(void* screenPtr,void* rendererPtr){auto& s=Schematics();auto& r=ScRender();Context c{};std::shared_ptr<std::vector<ScRenderSub>> subs;{std::lock_guard<std::recursive_mutex> l(s.guard);subs=s.render;}
 if(!subs||subs->empty()){if(!r.subs.empty()&&Client(&c)){for(auto& e:r.subs)for(auto& m:e.second.meshes)BeFreeMesh(c,m.mesh);r.subs.clear();}return;}
 if(!Local(&c))return;const uint64_t screen=(uint64_t)screenPtr,renderer=(uint64_t)rendererPtr;const uint64_t region=ActorRegion(c);if(!region)return;uint64_t holder{};float camera[3];
 if(!Read(renderer+0x480,&holder)||!holder||!Floats(holder+0x658,camera,3))return;ScMaterials(c);const uint64_t btess=reinterpret_cast<uint64_t(*)(uint64_t)>(c.base+0x5353600)(renderer);if(!btess)return;
 ++r.serial;const int64_t t0=ActionNs();const uint64_t cache=btess+0x6b0;const int32_t savedLayer=*(int32_t*)(btess+0x80);const uint64_t savedRegion=*(uint64_t*)(cache+0x18);*(uint64_t*)(cache+0x18)=region;
 for(const auto& sub:*subs){BeMeshRec& rec=r.subs[BeKey(sub.x,sub.y,sub.z)];rec.serial=r.serial;if(rec.version==sub.version)continue;if((double)(ActionNs()-t0)/1e6>2.0)continue;
  BeSub tmp{sub.x,sub.y,sub.z,sub.version,sub.blocks,{}};BeBuild(c,rec,tmp,screen,btess,region,2);/* "Schematic Ghost": mesh flags | 2 */rec.version=sub.version;}
 *(uint64_t*)(cache+0x18)=savedRegion;*(int32_t*)(btess+0x80)=savedLayer;
 for(auto it=r.subs.begin();it!=r.subs.end();){if(it->second.serial!=r.serial){for(auto& m:it->second.meshes)BeFreeMesh(c,m.mesh);it=r.subs.erase(it);}else ++it;}
 static const float ghost[4]={.55f,.75f,1.f,.35f},error[4]={1.f,.35f,.35f,.35f};ScDraw(c,screen,renderer,*subs,camera,0,ghost);ScDraw(c,screen,renderer,*subs,camera,1,error);}
inline void ScRenderGuarded(void* screen,void* renderer){auto& s=Schematics();if(s.fault)return;__try{ScRenderWork(screen,renderer);}__except(EXCEPTION_EXECUTE_HANDLER){InterlockedExchange(&s.fault,1);}}
}
