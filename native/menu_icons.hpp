// Icons for registry lists (blocks / items). Cache keyed "name:b" for a block and "name:i" for an item; loads
// blocks.json, textures/terrain_texture.json, textures/item_texture.json and PNGs. A texture value resolves as:
// object -> first key present among path, textures, texture, default, up, side, north, down (recursively);
// array -> first element; string -> the value. Only the top square of an animated texture is drawn; a missing icon
// shows "?" at 80 % height in (180,168,205). Files are read through the game resource system (ClientInstance
// vtable[0x2f0], pack stack +0x58, game function at RVA 0x17c2840; 0xd8-byte results with the std::string content
// first, destroyed by RVA 0x1158f0). Icons are packed into a 2048 x 2048 atlas of 32 px cells.
#pragma once
#include <windows.h>
#include <wincodec.h>
#include <wrl/client.h>
#include <algorithm>
#include <fstream>
#include <map>
#include <memory>
#include <sstream>
#include <string>
#include <vector>
#pragma comment(lib,"windowscodecs.lib")
#ifdef small
#undef small
#endif

namespace icons{
struct Json{enum Kind{Null,Bool,Number,String,Array,Object} kind=Null;std::string text;std::vector<Json> items;std::vector<std::pair<std::string,Json>> fields;
 const Json* get(const std::string& k)const{for(auto& f:fields)if(f.first==k)return &f.second;return nullptr;}};
struct Parser{const char* p;const char* e;
 void skip(){for(;;){while(p<e&&(unsigned char)*p<=' ')++p;if(p+1<e&&p[0]=='/'&&p[1]=='/'){while(p<e&&*p!='\n')++p;continue;}if(p+1<e&&p[0]=='/'&&p[1]=='*'){p+=2;while(p+1<e&&!(p[0]=='*'&&p[1]=='/'))++p;p+=2;continue;}break;}}
 std::string str(){std::string out;++p;while(p<e&&*p!='"'){if(*p=='\\'&&p+1<e){++p;char c=*p;out+=c=='n'?'\n':c=='t'?'\t':c;}else out+=*p;++p;}++p;return out;}
 Json value(int depth=0){Json v;skip();if(p>=e||depth>64)return v;
  if(*p=='{'){v.kind=Json::Object;++p;for(;;){skip();if(p>=e)break;if(*p=='}'){++p;break;}if(*p==','){++p;continue;}if(*p!='"'){++p;continue;}std::string k=str();skip();if(p<e&&*p==':')++p;v.fields.push_back({k,value(depth+1)});}}
  else if(*p=='['){v.kind=Json::Array;++p;for(;;){skip();if(p>=e)break;if(*p==']'){++p;break;}if(*p==','){++p;continue;}v.items.push_back(value(depth+1));}}
  else if(*p=='"'){v.kind=Json::String;v.text=str();}
  else{const char* s=p;while(p<e&&*p!=','&&*p!='}'&&*p!=']'&&(unsigned char)*p>' ')++p;v.text.assign(s,p);v.kind=v.text=="null"?Json::Null:(v.text=="true"||v.text=="false")?Json::Bool:Json::Number;}
  return v;}};
// reads a file through the game resource system (one content per pack, lowest to highest); provided by menu.cpp
inline std::vector<std::string> (*ReadPackFile)(const std::string& path)=nullptr;
inline Json ParseText(const std::string& all){Parser parser{all.data(),all.data()+all.size()};return parser.value();}
inline void Merge(Json& into,const Json& from){if(into.kind!=Json::Object||from.kind!=Json::Object){into=from;return;}for(auto& f:from.fields){bool found=false;for(auto& g:into.fields)if(g.first==f.first){Merge(g.second,f.second);found=true;break;}if(!found)into.fields.push_back(f);}}
inline void (*Log)(const char*)=nullptr;
inline Json ParsePacks(const std::string& path){Json out;bool first=true;if(!ReadPackFile)return out;for(auto& text:ReadPackFile(path)){Json one=ParseText(text);if(one.kind!=Json::Object)continue;if(first){out=one;first=false;}else Merge(out,one);}return out;}
inline Json ParseFile(const std::wstring& path){std::ifstream in(path,std::ios::binary);if(!in)return Json{};std::stringstream ss;ss<<in.rdbuf();const std::string all=ss.str();Parser parser{all.data(),all.data()+all.size()};return parser.value();}
// resolve a texture value to a path
inline std::string Resolve(const Json* v,int depth=0){if(!v||depth>8)return "";if(v->kind==Json::String)return v->text;if(v->kind==Json::Array)return v->items.empty()?"":Resolve(&v->items[0],depth+1);
 if(v->kind==Json::Object){static const char* order[]={"path","textures","texture","default","up","side","north","down"};for(auto k:order)if(auto f=v->get(k))return Resolve(f,depth+1);}return "";}
// word key: strips "minecraft:", darkoak -> dark_oak, splits on "_", wooden -> wood, golden -> gold, sorts and
// rejoins; used to find a texture by its file name
inline std::string WordKey(std::string n){if(n.rfind("minecraft:",0)==0)n=n.substr(10);const size_t slash=n.find_last_of('/');if(slash!=std::string::npos)n=n.substr(slash+1);
 for(size_t at;(at=n.find("darkoak"))!=std::string::npos;)n.replace(at,7,"dark_oak");
 std::vector<std::string> words;size_t from=0;while(from<=n.size()){size_t to=n.find('_',from);if(to==std::string::npos)to=n.size();std::string w=n.substr(from,to-from);if(w=="wooden")w="wood";else if(w=="golden")w="gold";if(!w.empty())words.push_back(w);from=to+1;}
 std::sort(words.begin(),words.end());std::string key;for(auto& w:words){if(!key.empty())key+="_";key+=w;}return key;}
// every texture referenced by a *_texture.json file (strings, arrays, "path" objects), indexed by WordKey
inline void CollectPaths(const Json* v,std::map<std::string,std::string>& out,int depth=0){if(!v||depth>6)return;if(v->kind==Json::String){if(!v->text.empty())out.emplace(WordKey(v->text),v->text);}
 else if(v->kind==Json::Array)for(auto& item:v->items)CollectPaths(&item,out,depth+1);else if(v->kind==Json::Object){if(auto f=v->get("path"))CollectPaths(f,out,depth+1);else if(auto f2=v->get("textures"))CollectPaths(f2,out,depth+1);}}
inline void IndexTextures(const Json& file,std::map<std::string,std::string>& out){const Json* data=file.get("texture_data");if(!data||data->kind!=Json::Object)return;for(auto& entry:data->fields)if(auto t=entry.second.get("textures"))CollectPaths(t,out);}
struct Cache{
 bool loaded=false;std::wstring root;Json blocks,terrain,items;std::map<std::string,std::string> itemWords,terrainWords;std::map<std::string,int> cells;std::vector<std::string> queue;std::vector<unsigned char> atlas;int used=0;bool dirty=false;double uploaded=0;
 static constexpr int Cell=32,PerRow=64,Size=Cell*PerRow;
 void Load(){if(loaded||!ReadPackFile)return;wchar_t exe[MAX_PATH]{};GetModuleFileNameW(GetModuleHandleW(nullptr),exe,MAX_PATH);std::wstring dir=exe;dir=dir.substr(0,dir.find_last_of(L"\\/")+1);root=dir+L"data\\resource_packs\\vanilla\\";
  blocks=ParsePacks("blocks.json");terrain=ParsePacks("textures/terrain_texture.json");items=ParsePacks("textures/item_texture.json");
  if(Log){const Json* ti=items.get("texture_data");const Json* tt=terrain.get("texture_data");Log(("Icons: packs read (blocks.json "+std::string(blocks.kind==Json::Object?"ok":"missing")+", terrain "+std::to_string(tt&&tt->kind==Json::Object?tt->fields.size():0)+", items "+std::to_string(ti&&ti->kind==Json::Object?ti->fields.size():0)+")").c_str());}
  if(terrain.kind!=Json::Object)return;IndexTextures(items,itemWords);IndexTextures(terrain,terrainWords);loaded=true;atlas.assign((size_t)Size*Size*4,0);}
 static std::string Bare(const std::string& n){const size_t c=n.find(':');return c==std::string::npos?n:n.substr(c+1);}
 std::string BlockPath(const std::string& name){const std::string bare=Bare(name);const Json* entry=blocks.get(bare);if(!entry)entry=blocks.get(name);std::string key=entry?Resolve(entry->get("textures")):"";if(key.empty())key=bare;
  const Json* data=terrain.get("texture_data");const Json* t=data?data->get(key):nullptr;std::string path=t?Resolve(t->get("textures")):"";if(!path.empty())return path;
  auto f=terrainWords.find(WordKey(name));return f!=terrainWords.end()?f->second:"";}
 std::string ItemPath(const std::string& name){const std::string bare=Bare(name);const Json* data=items.get("texture_data");const Json* t=data?data->get(bare):nullptr;std::string path=t?Resolve(t->get("textures")):"";if(!path.empty())return path;
  const std::string key=WordKey(name);auto f=itemWords.find(key);if(f!=itemWords.end())return f->second;path=BlockPath(name);if(!path.empty())return path;auto g=terrainWords.find(key);return g!=terrainWords.end()?g->second:"";}
 bool Decode(const std::string& bytes,std::vector<unsigned char>& rgba,UINT& w,UINT& h){using Microsoft::WRL::ComPtr;if(bytes.empty())return false;CoInitializeEx(nullptr,COINIT_MULTITHREADED);ComPtr<IWICImagingFactory> f;ComPtr<IWICStream> stream;ComPtr<IWICBitmapDecoder> dec;ComPtr<IWICBitmapFrameDecode> frame;ComPtr<IWICFormatConverter> conv;
  if(FAILED(CoCreateInstance(CLSID_WICImagingFactory,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&f)))||FAILED(f->CreateStream(&stream))||FAILED(stream->InitializeFromMemory((BYTE*)bytes.data(),(DWORD)bytes.size()))||FAILED(f->CreateDecoderFromStream(stream.Get(),nullptr,WICDecodeMetadataCacheOnLoad,&dec))||FAILED(dec->GetFrame(0,&frame))||FAILED(f->CreateFormatConverter(&conv))
   ||FAILED(conv->Initialize(frame.Get(),GUID_WICPixelFormat32bppRGBA,WICBitmapDitherTypeNone,nullptr,0,WICBitmapPaletteTypeCustom))||FAILED(conv->GetSize(&w,&h))||!w||!h||w>4096||h>65536)return false;rgba.resize((size_t)w*h*4);return SUCCEEDED(conv->CopyPixels(nullptr,w*4,(UINT)rgba.size(),rgba.data()));}
 // -1: not loaded yet; -2: missing; >= 0: atlas cell
 int Lookup(const std::string& name,bool block){const std::string key=name+(block?":b":":i");auto f=cells.find(key);if(f!=cells.end())return f->second;cells[key]=-1;queue.push_back(key);return -1;}
 void Work(int budget){Load();if(!loaded)return;while(budget-->0&&!queue.empty()){const std::string key=queue.front();queue.erase(queue.begin());const bool block=key.size()>2&&key.compare(key.size()-2,2,":b")==0;const std::string name=key.substr(0,key.size()-2);
   const std::string path=block?BlockPath(name):ItemPath(name);std::vector<unsigned char> px;UINT w=0,h=0;int cell=-2;
   std::string bytes;if(!path.empty()&&ReadPackFile){auto all=ReadPackFile(path+".png");for(auto it=all.rbegin();it!=all.rend();++it)if(!it->empty()){bytes=*it;break;}}
   if(!bytes.empty()&&used<PerRow*PerRow&&Decode(bytes,px,w,h)){cell=used++;const UINT side=std::min(w,h);const int cx=(cell%PerRow)*Cell,cy=(cell/PerRow)*Cell;
    for(int y=0;y<Cell;++y)for(int x=0;x<Cell;++x){const UINT sx=x*side/Cell,sy=y*side/Cell;memcpy(&atlas[((size_t)(cy+y)*Size+cx+x)*4],&px[((size_t)sy*w+sx)*4],4);}dirty=true;}
   if(cell==-2&&Log){static int told=0;if(told<24){++told;Log(("Icons: no icon for "+key+" (path '"+path+"', "+std::to_string(bytes.size())+" bytes)").c_str());}}
   cells[key]=cell;}}
};
inline Cache& Icons(){static Cache* c=new Cache;return *c;}
}
